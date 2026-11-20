#include "SequenceReader.h"
#include "MediaDecoder.h"
#include <QThread>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
namespace openframe {
struct SequenceReader::Impl {
    struct Item {
        QString key,owner,path;
        qint64 ordinal=0,reserved=0,bytes=0;
        quint64 epoch=0,used=0;
        bool ready=false,active=false,demand=false,cancelled=false;
        VideoFrame frame;
        QString error;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Item>> jobs;
    std::map<QString,std::shared_ptr<Item>> items;
    std::vector<std::jthread> workers;
    Reader reader;
    bool stopping=false;
    quint64 epoch=1,serial=0;
    qint64 budget,cacheBytes=0,reservedBytes=0,maxResident=0,completed=0;
    int active=0,maxActive=0,maxPending=0;
    QString lastOwner; qint64 lastOrdinal=-1;
    explicit Impl(qint64 bytes,int threads,Reader function):reader(std::move(function)),budget(std::max<qint64>(1,bytes)) {
        if(!reader) reader=readLocalRasterImage;
        threads=std::clamp(threads,1,8);
        for(int i=0;i<threads;++i) workers.emplace_back([this]{run();});
    }
    ~Impl() {
        {std::lock_guard lock(mutex);stopping=true;++epoch;}
        changed.notify_all();
        for(auto &worker:workers)worker.join();
    }
    void evict(qint64 required) {
        while(cacheBytes+reservedBytes+required>budget) {
            auto oldest=items.end();
            for(auto it=items.begin();it!=items.end();++it)
                if(it->second->ready&&!it->second->active&&it->second->bytes>0&&
                   (oldest==items.end()||it->second->used<oldest->second->used))oldest=it;
            if(oldest==items.end())break;
            cacheBytes-=oldest->second->bytes;items.erase(oldest);
        }
    }
    void cancelPending(const QString &owner={},qint64 center=-1) {
        for(auto it=jobs.begin();it!=jobs.end();) {
            auto item=*it;
            if(!item->demand&&(item->owner!=owner||std::abs(item->ordinal-center)>int(workers.size()))) {
                reservedBytes-=item->reserved;item->reserved=0;item->cancelled=true;item->ready=true;
                items.erase(item->key);it=jobs.erase(it);
            }else ++it;
        }
    }
    std::shared_ptr<Item> request(const MediaSource &source,qint64 ordinal,bool demand,std::unique_lock<std::mutex> &lock) {
        if(!source.info.hasVideo||source.info.framePts.isEmpty()||source.info.frameCount()<=0||(!source.sequencePaths.isEmpty()&&source.sequencePaths.size()!=source.info.frameCount()))
            throw std::runtime_error("Invalid sequence frame count");
        ordinal=std::clamp<qint64>(ordinal,0,source.info.frameCount()-1);
        auto path=source.sequencePaths.isEmpty()?source.path:source.sequencePaths[ordinal];
        auto key=source.id+':'+path+':'+QString::number(ordinal);
        auto old=items.find(key);
        if(old!=items.end()) {old->second->used=++serial;old->second->demand|=demand;return old->second;}
        const auto required=std::max<qint64>(1,qint64(source.info.width)*source.info.height*16);
        for(;;) {
            evict(required);
            if(stopping)throw std::runtime_error("Sequence reader stopped");
            // One image larger than the budget may be decoded on demand, but is never cached.
            if(jobs.size()<workers.size()*2&&
               (cacheBytes+reservedBytes+required<=budget||(demand&&reservedBytes==0&&cacheBytes==0)))break;
            if(!demand)return {};
            cancelPending(source.id,ordinal);evict(required);
            if(reservedBytes==0&&cacheBytes==0&&jobs.empty())break;
            changed.wait(lock);
        }
        auto item=std::make_shared<Item>();
        item->key=key;item->owner=source.id;item->path=path;item->ordinal=ordinal;item->epoch=epoch;
        item->reserved=required;item->demand=demand;item->used=++serial;
        reservedBytes+=required;items[key]=item;
        if(demand)jobs.push_front(item);else jobs.push_back(item);
        maxPending=std::max(maxPending,int(jobs.size()));maxResident=std::max(maxResident,cacheBytes+reservedBytes);
        changed.notify_all();return item;
    }
    void run() {
        for(;;) {
            std::shared_ptr<Item> item;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||!jobs.empty();});
                if(stopping)return;
                item=jobs.front();jobs.pop_front();item->active=true;++active;maxActive=std::max(maxActive,active);}
            VideoFrame frame;QString error;
            try{frame.raster=reader(item->path);frame.color=rasterColorInfo(frame.raster);frame.ordinal=item->ordinal;}
            catch(const std::exception &e){error=QString::fromUtf8(e.what());}
            {std::lock_guard lock(mutex);
                --active;reservedBytes-=item->reserved;item->reserved=0;item->active=false;item->ready=true;
                item->cancelled=stopping||item->epoch!=epoch;
                if(!item->cancelled) {
                    item->frame=std::move(frame);item->error=error;++completed;
                    const auto bytes=item->frame.byteSize();evict(bytes);
                    if(error.isEmpty()&&bytes>0&&bytes<=budget&&cacheBytes+reservedBytes+bytes<=budget){item->bytes=bytes;cacheBytes+=bytes;}
                    else items.erase(item->key);
                }else if(auto it=items.find(item->key);it!=items.end()&&it->second==item)items.erase(it);
                maxResident=std::max(maxResident,cacheBytes+reservedBytes);
            }
            changed.notify_all();
        }
    }
};
SequenceReader::SequenceReader(qint64 bytes,int threads,Reader reader):d(std::make_unique<Impl>(bytes,threads,std::move(reader))){}
SequenceReader::~SequenceReader()=default;
VideoFrame SequenceReader::frame(const MediaSource &source,qint64 ordinal) {
    std::unique_lock lock(d->mutex);
    d->cancelPending(source.id,ordinal);
    auto item=d->request(source,ordinal,true,lock);
    // Decode independent neighboring files concurrently; the exact requested ordinal wins.
    const int direction=d->lastOwner==source.id&&ordinal<d->lastOrdinal?-1:1;
    d->lastOwner=source.id;d->lastOrdinal=ordinal;
    for(int n=1;n<=int(d->workers.size());++n) {
        const auto next=ordinal+direction*n;
        if(next>=0&&next<source.info.frameCount())d->request(source,next,false,lock);
    }
    d->changed.wait(lock,[&]{return d->stopping||item->ready;});
    if(d->stopping||item->cancelled)throw std::runtime_error("Sequence request cancelled");
    if(!item->error.isEmpty())throw std::runtime_error(item->error.toStdString());
    return item->frame;
}
void SequenceReader::prefetch(const MediaSource &source,const QVector<qint64> &ordinals) {
    std::unique_lock lock(d->mutex);
    for(auto frame:ordinals)if(frame>=0&&frame<source.info.frameCount())d->request(source,frame,false,lock);
}
void SequenceReader::clear() {
    {std::lock_guard lock(d->mutex);++d->epoch;d->cancelPending();d->items.clear();d->cacheBytes=0;}
    d->changed.notify_all();
}
void SequenceReader::setBudget(qint64 bytes) {std::lock_guard lock(d->mutex);d->budget=std::max<qint64>(1,bytes);d->evict(0);}
QJsonObject SequenceReader::diagnostics() const {
    std::lock_guard lock(d->mutex);
    return {{"threads",int(d->workers.size())},{"active",d->active},{"pending",int(d->jobs.size())},
        {"maxActive",d->maxActive},{"maxPending",d->maxPending},{"budgetBytes",double(d->budget)},
        {"cacheBytes",double(d->cacheBytes)},{"reservedBytes",double(d->reservedBytes)},
        {"maxResidentBytes",double(d->maxResident)},{"completed",double(d->completed)}};
}
}
