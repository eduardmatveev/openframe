#include "ReviewSession.h"
#include "ReviewSessionTestAccess.h"
#include "media/MediaDecoder.h"
#include "media/FrameComposer.h"
#include "media/Exporter.h"
#include "media/AudioTimeStretcher.h"
#include "media/AudioOutputConverter.h"
#include "integration/LocalPathPolicy.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QDir>
#include <QRegularExpression>
#include <QMediaDevices>
#include <QPointer>
#include <QTimer>
#include <QUndoCommand>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <map>
#include <QSet>
#include <thread>

namespace openframe {
namespace {
class MediaOperation final {
    MediaEngine &engine;

  public:
    MediaOperation(MediaEngine &value, CancelCheck cancelled) : engine(value) {
        engine.setOperationCancelled(std::move(cancelled));
    }
    ~MediaOperation() {
        engine.setOperationCancelled({});
    }
};
class SnapshotCommand final : public QUndoCommand {
  public:
    SnapshotCommand(QString label, Project before, Project after,
                    std::function<void(const Project &, const Project &)> apply)
        : QUndoCommand(std::move(label)), before_(std::move(before)), after_(std::move(after)),
          apply_(std::move(apply)) {}
    void undo() override {
        apply_(before_, after_);
    }
    void redo() override {
        apply_(after_, before_);
    }

  private:
    Project before_, after_;
    std::function<void(const Project &, const Project &)> apply_;
};
template <class T>
QVector<T> mergeChangedRecords(const QVector<T> &current, const QVector<T> &target,
                               const QVector<T> &baseline) {
    QSet<QString> changed;
    for (const auto &item : baseline) {
        auto found = std::find_if(target.begin(), target.end(),
                                  [&](const T &other) { return other.id == item.id; });
        if (found == target.end() || !(*found == item))
            changed.insert(item.id);
    }
    for (const auto &item : target) {
        auto found = std::find_if(baseline.begin(), baseline.end(),
                                  [&](const T &other) { return other.id == item.id; });
        if (found == baseline.end() || !(*found == item))
            changed.insert(item.id);
    }
    QVector<T> result;
    QSet<QString> visited;
    for (const auto &item : target) {
        visited.insert(item.id);
        if (changed.contains(item.id))
            result.append(item);
        else {
            auto found = std::find_if(current.begin(), current.end(),
                                      [&](const T &other) { return other.id == item.id; });
            if (found != current.end())
                result.append(*found);
        }
    }
    for (const auto &item : current)
        if (!changed.contains(item.id) && !visited.contains(item.id))
            result.append(item);
    return result;
}
class AnnotationCommand final : public QUndoCommand {
  public:
    AnnotationCommand(QString label, QVector<Annotation> before, QVector<Annotation> after,
                      std::function<void(const QVector<Annotation> &)> apply)
        : QUndoCommand(std::move(label)), before_(std::move(before)), after_(std::move(after)),
          apply_(std::move(apply)) {}
    void undo() override {
        apply_(before_);
    }
    void redo() override {
        apply_(after_);
    }

  private:
    QVector<Annotation> before_, after_;
    std::function<void(const QVector<Annotation> &)> apply_;
};
QString viewKey(const Project &p, int pane) {
    auto owner = p.viewer.assignments.value(pane);
    if (owner.isEmpty())
        owner = p.activeTimelineId;
    return owner + ":" + QString::number(pane);
}
void rememberViews(Project &p) {
    p.panZoomByOwner[viewerPaneOwner(p, primaryViewerPane(p.viewer))] = p.viewer.panZoom;
    for (int pane = 0; pane < std::min<qsizetype>(4, p.viewer.transforms.size()); ++pane)
        p.viewTransforms[viewKey(p, pane)] = p.viewer.transforms[pane];
}
void restoreView(Project &p, int pane) {
    while (p.viewer.transforms.size() <= pane)
        p.viewer.transforms.append(ViewTransform{});
    p.viewer.transforms[pane] = p.viewTransforms.value(viewKey(p, pane), ViewTransform{});
    if (pane == primaryViewerPane(p.viewer))
        p.viewer.panZoom = p.panZoomByOwner.value(viewerPaneOwner(p, pane), PanZoom{});
}
QStringList rootedPaths(const QStringList &paths, const QString &root) {
    QStringList result;
    for (const auto &path : paths)
        result.append(!root.isEmpty() && !QDir::isAbsolutePath(path)
                          ? QDir(root).absoluteFilePath(path)
                          : path);
    return result;
}
qint64 heldBookmark(const QVector<qint64> &frames, qint64 requested) {
    auto next = std::upper_bound(frames.begin(), frames.end(), requested);
    return next == frames.begin() ? frames.first() : *std::prev(next);
}
class PcmQueue final : public QIODevice {
  public:
    PcmQueue() {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    bool isSequential() const override {
        return true;
    }
    qint64 bytesAvailable() const override {
        std::lock_guard lock(mutex_);
        return bytes_.size() + QIODevice::bytesAvailable();
    }
    void append(const QByteArray &data) {
        {
            std::lock_guard lock(mutex_);
            if (bytes_.size() + data.size() > capacity_)
                return;
            bytes_.append(data);
        }
        emit readyRead();
    }
    void setCapacity(qint64 bytes) {
        std::lock_guard lock(mutex_);
        capacity_ = bytes;
    }
    void clear() {
        {
            std::lock_guard lock(mutex_);
            bytes_.clear();
        }
        // Clear QIODevice's own state as well: its default read-ahead buffer
        // otherwise survives a transport reset and can replay stale samples.
        QIODevice::close();
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

  protected:
    qint64 readData(char *output, qint64 max) override {
        std::lock_guard lock(mutex_);
        auto n = std::min<qint64>(max, bytes_.size());
        if (n > 0) {
            std::memcpy(output, bytes_.constData(), static_cast<size_t>(n));
            bytes_.remove(0, n);
        }
        return n;
    }
    qint64 writeData(const char *, qint64) override {
        return -1;
    }

  private:
    mutable std::mutex mutex_;
    QByteArray bytes_;
    qint64 capacity_ = 48000 * 8 * 4;
};
QVector<float> speedAudio(const QVector<float> &in, int outputSamples, double rate) {
    QVector<float> out(outputSamples * 2, 0.0f);
    auto inputSamples = in.size() / 2;
    for (int i = 0; i < outputSamples; ++i) {
        double position = rate < 0 ? inputSamples - 1 - i * std::abs(rate) : i * rate;
        auto a = static_cast<qint64>(std::floor(position));
        double frac = position - a;
        if (a < 0 || a >= inputSamples)
            continue;
        auto b = std::min<qint64>(a + 1, inputSamples - 1);
        for (int ch = 0; ch < 2; ++ch)
            out[i * 2 + ch] =
                static_cast<float>(in[a * 2 + ch] * (1 - frac) + in[b * 2 + ch] * frac);
    }
    return out;
}
} // namespace
struct ReviewSession::Impl {
    ReviewSession *q;
    Project project = emptyProject();
    quint64 previewSerial = 0, previewToken = 0;
    std::optional<ViewerSettings> viewerPreview;
    std::optional<Annotation> annotationPreview;
    struct ComparisonPreview {
        // The render worker owns these fields for the lifetime of one slider gesture.
        FrameComposer::ComparisonLayers layers;
        qint64 frame = -1;
        quint64 presentation = 0;
        bool showAnnotations = false;
    };
    std::shared_ptr<ComparisonPreview> comparisonPreview;
    void invalidatePreview() {
        previewToken = 0;
        viewerPreview.reset();
        annotationPreview.reset();
        comparisonPreview.reset();
    }
    QString path;
    QUndoStack undo;
    std::map<QString, std::unique_ptr<QUndoStack>> annotationStacks;
    qint64 frame = 0;
    qint64 renderedFrame = -1;
    quint64 presentedSerial = 0;
    std::atomic<quint64> presentationEpoch{0};
    bool playing = false;
    bool delayActive = false;
    QElapsedTimer delayClock;
    qint64 delayResumeFrame = 0;
    double delayResumeRate = 1.0;
    double rate = 1.0;
    bool audioEnabled = true;
    bool scrubEnabled = true;
    bool timeStretching = true;
    bool loopStep = false;
    bool loopStepRange = false;
    bool lockStepRange = false;
    bool autoplay = false;
    bool hideAnnotations = false;
    double volume = 1.0;
    int cacheMiB = 256;
    int frameStep = 1;
    bool bookmarkMode = false;
    bool reverseAudio = true;
    bool projectUndo = true;
    bool untrackedDirty = false;
    Rational sequenceFps{24, 1};
    qint64 defaultOffset = 1;
    bool defaultAddTimeline = true;
    bool defaultActivateTimeline = true;
    QString defaultSourceRoot;
    bool restorePanZoom = true;
    bool restoreHold = true;
    QImage image;
    QTimer timer;
    QElapsedTimer clock;
    qint64 anchorFrame = 0;
    int imports = 0;
    std::atomic_bool stop{false}, exportCancel{false};
    std::atomic<quint64> renderEpoch{0}, audioEpoch{0}, projectEpoch{0};
#ifdef OPENFRAME_TESTING
    std::function<void()> beforePresentationTestHook; // worker-thread-only, consumed once
#endif
    std::mutex mutex;
    std::condition_variable cv;
    enum class JobKind { Generic, Render, AudioReset, Audio };
    struct WorkItem {
        JobKind kind;
        std::function<void(MediaEngine &)> run;
    };
    std::deque<WorkItem> jobs;
    std::jthread worker, importWorker, exportThread;
    std::mutex importMutex;
    std::condition_variable importCv;
    std::deque<std::function<void(MediaEngine &)>> importJobs;
    bool exporting = false;
    PcmQueue pcm;
    std::unique_ptr<QAudioSink> sink;
    QAudioFormat outputFormat;
    QMediaDevices devices;
    bool audioClock = false;
    bool audioHadData = false;
    qint64 scheduledAudioUs = 0;
    int pendingAudio = 0;
    qint64 stretchInputFrames = 0, stretchOutputFrames = 0;
    int stretchBufferedFrames = 0;
    // Worker-thread-only state. Every discontinuity invalidates it by audioEpoch.
    struct StretchState {
        quint64 epoch = 0;
        std::unique_ptr<AudioTimeStretcher> processor;
        std::unique_ptr<AudioOutputConverter> converter;
        QVector<float> input;
        qsizetype inputPosition = 0;
        qint64 readFrames = 0;
    } stretch;
    explicit Impl(ReviewSession *owner) : q(owner) {
        timer.setInterval(8);
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, q, &ReviewSession::tick);
        QObject::connect(&devices, &QMediaDevices::audioOutputsChanged, q, [this]() {
            resetAudio();
            if (playing)
                startTransport();
        });
        QObject::connect(&undo, &QUndoStack::cleanChanged, q,
                         [this]() { emit q->projectChanged(); });
        worker = std::jthread([this]() {
            MediaEngine engine(cacheMiB, [this]() { return stop.load(); });
            while (!stop) {
                std::function<void(MediaEngine &)> job;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this]() { return stop || !jobs.empty(); });
                    if (stop)
                        break;
                    job = std::move(jobs.front().run);
                    jobs.pop_front();
                }
                try {
                    job(engine);
                } catch (const std::exception &e) {
                    post([message = QString::fromUtf8(e.what())](ReviewSession *s) {
                        emit s->error(message);
                    });
                }
            }
        });
    }
    void startImportWorker() {
        importWorker = std::jthread([this]() {
            MediaEngine engine(16, [this]() { return stop.load(); });
            while (!stop) {
                std::function<void(MediaEngine &)> job;
                {
                    std::unique_lock lock(importMutex);
                    importCv.wait(lock, [this]() { return stop || !importJobs.empty(); });
                    if (stop)
                        break;
                    job = std::move(importJobs.front());
                    importJobs.pop_front();
                }
                try {
                    job(engine);
                } catch (const std::exception &e) {
                    post([message = QString::fromUtf8(e.what())](ReviewSession *s) {
                        emit s->error(message);
                    });
                }
            }
        });
    }
    void enqueueImport(std::function<void(MediaEngine &)> job) {
        if (!importWorker.joinable())
            startImportWorker();
        {
            std::lock_guard lock(importMutex);
            importJobs.push_back(std::move(job));
        }
        importCv.notify_one();
    }
    ~Impl() {
        stop = true;
        exportCancel = true;
        ++renderEpoch;
        ++audioEpoch;
        ++projectEpoch;
        cv.notify_all();
        importCv.notify_all();
        if (worker.joinable())
            worker.join();
        if (importWorker.joinable())
            importWorker.join();
        if (exportThread.joinable())
            exportThread.join();
        resetAudio();
    }
    void enqueue(std::function<void(MediaEngine &)> job, JobKind kind = JobKind::Generic) {
        {
            std::lock_guard lock(mutex);
            if (kind == JobKind::Render || kind == JobKind::AudioReset)
                std::erase_if(jobs, [&](const WorkItem &pending) {
                    return pending.kind == kind ||
                           (kind == JobKind::AudioReset && pending.kind == JobKind::Audio);
                });
            jobs.push_back({kind, std::move(job)});
        }
        cv.notify_one();
    }
    void post(std::function<void(ReviewSession *)> callback) {
        QPointer<ReviewSession> guard(q);
        QMetaObject::invokeMethod(
            q,
            [guard, callback = std::move(callback)]() {
                if (guard)
                    callback(guard);
            },
            Qt::QueuedConnection);
    }
    QString owner() const {
        auto id = project.viewer.assignments.value(primaryViewerPane(project.viewer));
        return project.source(id) || project.timeline(id) ? id : project.activeTimelineId;
    }
    Rational fps() const {
        auto id = owner();
        if (auto s = project.source(id))
            return s->info.fps;
        auto t = project.timeline(id);
        return t ? t->fps : project.fps;
    }
    qint64 timeAtFrame(qint64 f) const {
        if (auto s = project.source(owner()))
            return s->info.frameStartUs(f);
        return fps().frameToUs(f);
    }
    qint64 timeAfterFrame(qint64 f) const {
        if (auto s = project.source(owner()))
            return s->info.frameEndUs(f);
        return fps().frameToUs(f + 1);
    }
    qint64 playedAudioUs() const {
        if (!sink || !audioHadData)
            return 0;
        auto result = sink->processedUSecs();
#ifdef Q_OS_WIN
        // Qt 6.8's WASAPI sink reports submitted input in processedUSecs(),
        // including frames still in the device buffer. Its public bytesFree()
        // reflects WASAPI padding, so remove that pending duration before using
        // the counter as a presentation clock or deciding to discard the sink.
        if (sink->state() == QAudio::ActiveState || sink->state() == QAudio::IdleState) {
            const auto pending = std::clamp<qsizetype>(sink->bufferSize() - sink->bytesFree(), 0,
                                                       sink->bufferSize());
            result -= sink->format().durationForBytes(static_cast<qint32>(pending));
        }
#endif
        return std::max<qint64>(0, result);
    }
    QVector<qint64> bookmarks() const {
        QVector<qint64> result;
        auto first = std::max<qint64>(0, project.rangeIn);
        auto last = project.rangeOut < 0 ? q->durationFrames() - 1 : project.rangeOut;
        for (const auto &b : project.bookmarks)
            if (b.timelineId == owner() && b.frame >= first && b.frame <= last)
                result.append(b.frame);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }
    void resetAudio() {
        const auto epoch = ++audioEpoch;
        if (!stop)
            enqueue(
                [this, epoch](MediaEngine &) {
                    if (stretch.epoch != epoch)
                        stretch = {};
                },
                JobKind::AudioReset);
        pendingAudio = 0;
        scheduledAudioUs = 0;
        audioClock = false;
        audioHadData = false;
        stretchInputFrames = stretchOutputFrames = 0;
        stretchBufferedFrames = 0;
        if (sink) {
            sink->reset();
            sink.reset();
        }
        pcm.clear();
    }
    bool openAudio() {
        if (!audioEnabled)
            return false;
        auto device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return false;
        auto format = device.preferredFormat();
        if (device.supportedSampleFormats().contains(QAudioFormat::Float))
            format.setSampleFormat(QAudioFormat::Float);
        if (device.minimumChannelCount() <= 2 && device.maximumChannelCount() >= 2)
            format.setChannelCount(2);
        if (!device.isFormatSupported(format))
            format = device.preferredFormat();
        if (!device.isFormatSupported(format) || !AudioOutputConverter::supports(format)) {
            emit q->status(QString("Audio device '%1' has no supported PCM output format; select "
                                   "another output device")
                               .arg(device.description()));
            return false;
        }
        outputFormat = format;
        pcm.setCapacity(format.bytesForDuration(4000000));
        sink = std::make_unique<QAudioSink>(device, format);
        sink->setBufferSize(format.bytesForDuration(100000));
        sink->setVolume(volume);
        // Start only after actual PCM is ready; filter lookahead must not advance video.
        return sink->error() == QAudio::NoError;
    }
    void startTransport() {
        const bool wasEmptyDelay = delayActive && project.loopHoldEmpty;
        delayActive = false;
        anchorFrame = frame;
        clock.restart();
        resetAudio();
        audioClock = openAudio();
        if (audioClock) {
            queueAudio(0, 500000, false);
            queueAudio(500000, 500000, false);
            scheduledAudioUs = 1000000;
        }
        if (wasEmptyDelay) {
            ++presentationEpoch;
            q->requestFrame();
        }
    }
    void queueAudio(qint64 outputStartUs, qint64 outputDurationUs, bool scrub) {
        const auto epoch = audioEpoch.load();
        auto snapshot = project;
        auto timeline = owner();
        const double playRate = scrub ? (rate < 0 ? -1.0 : 1.0) : rate;
        const auto baseUs = playRate < 0 ? timeAfterFrame(scrub ? frame : anchorFrame)
                                         : timeAtFrame(scrub ? frame : anchorFrame);
        const bool audible = playRate >= 0 || reverseAudio;
        const bool preservePitch = timeStretching && !scrub;
        const auto deviceFormat = outputFormat;
        const auto first =
            std::clamp<qint64>(project.rangeIn, 0, std::max<qint64>(0, q->durationFrames() - 1));
        const auto last = project.rangeOut < 0 ? q->durationFrames() - 1 : project.rangeOut;
        const auto lowerUs = scrub ? qint64(0) : timeAtFrame(first);
        const auto sourceOwner = project.source(timeline);
        const auto upperUs =
            sourceOwner
                ? sourceOwner->info.frameEndUs(scrub ? sourceOwner->info.frameCount() - 1 : last)
                : fps().frameToUs((scrub ? q->durationFrames() - 1 : last) + 1);
        ++pendingAudio;
        enqueue(
            [this, epoch, snapshot = std::move(snapshot), timeline, playRate, baseUs, outputStartUs,
             outputDurationUs, audible, preservePitch, lowerUs, upperUs,
             deviceFormat](MediaEngine &engine) {
                if (stop || epoch != audioEpoch)
                    return;
                MediaOperation operation(engine,
                                         [this, epoch] { return stop || epoch != audioEpoch; });
                try {
                    if (stretch.epoch != epoch || !stretch.converter) {
                        stretch = {};
                        stretch.epoch = epoch;
                        stretch.converter = std::make_unique<AudioOutputConverter>(deviceFormat);
                    }
                    const int requested = static_cast<int>(outputDurationUs * 48000 / 1000000);
                    auto read = [&](qint64 start, qint64 length) {
                        auto data = engine.ownerAudio(snapshot, timeline, start, length);
                        auto count = data.size() / 2;
                        auto firstSample =
                            std::clamp<qint64>(std::llround((lowerUs - start) * .048), 0, count);
                        auto lastSample =
                            std::clamp<qint64>(std::llround((upperUs - start) * .048), 0, count);
                        std::fill(data.begin(), data.begin() + firstSample * 2, 0.0f);
                        std::fill(data.begin() + lastSample * 2, data.end(), 0.0f);
                        return data;
                    };
                    QVector<float> output;
                    qint64 inputFrames = 0, outputFrames = 0;
                    int buffered = 0;
                    if (!audible) {
                        output.fill(0.0f, requested * 2);
                    } else if (preservePitch) {
                        if (!stretch.processor) {
                            stretch.processor =
                                std::make_unique<AudioTimeStretcher>(std::abs(playRate));
                        }
                        while (stretch.processor->availableFrames() < requested) {
                            if (stop || epoch != audioEpoch)
                                return;
                            if (stretch.inputPosition == stretch.input.size()) {
                                constexpr int readCount =
                                    48000; // one bounded second, independent of rate
                                const auto cursorUs = stretch.readFrames * 1000000 / 48000;
                                auto start =
                                    playRate < 0 ? baseUs - cursorUs - 1000000 : baseUs + cursorUs;
                                stretch.input = read(start, 1000000);
                                stretch.input.resize(readCount * 2);
                                if (playRate < 0)
                                    for (int a = 0, b = readCount - 1; a < b; ++a, --b)
                                        for (int ch = 0; ch < 2; ++ch)
                                            std::swap(stretch.input[a * 2 + ch],
                                                      stretch.input[b * 2 + ch]);
                                stretch.inputPosition = 0;
                                stretch.readFrames += readCount;
                            }
                            auto count =
                                std::min<qsizetype>(AudioTimeStretcher::MaximumInputFrames * 2,
                                                    stretch.input.size() - stretch.inputPosition);
                            stretch.processor->push(QVector<float>(
                                stretch.input.cbegin() + stretch.inputPosition,
                                stretch.input.cbegin() + stretch.inputPosition + count));
                            stretch.inputPosition += count;
                        }
                        output = stretch.processor->take(requested);
                        inputFrames = stretch.processor->inputFrames();
                        outputFrames = stretch.processor->outputFrames();
                        buffered = stretch.processor->availableFrames();
                    } else {
                        auto sourceStart = baseUs + static_cast<qint64>(outputStartUs * playRate);
                        auto sourceLength =
                            static_cast<qint64>(outputDurationUs * std::abs(playRate));
                        if (playRate < 0)
                            sourceStart -= sourceLength;
                        output = speedAudio(read(sourceStart, sourceLength), requested, playRate);
                    }
                    auto bytes = stretch.converter->convert(output);
                    post([epoch, bytes = std::move(bytes), inputFrames, outputFrames,
                          buffered](ReviewSession *s) {
                        auto &d = *s->d;
                        if (epoch != d.audioEpoch)
                            return;
                        --d.pendingAudio;
                        d.pcm.append(bytes);
                        d.stretchInputFrames = inputFrames;
                        d.stretchOutputFrames = outputFrames;
                        d.stretchBufferedFrames = buffered;
                        if (!d.audioHadData && d.sink) {
                            d.sink->start(&d.pcm);
                            d.clock.restart();
                        }
                        d.audioHadData = true;
                    });
                } catch (const std::exception &e) {
                    post([epoch, message = QString::fromUtf8(e.what())](ReviewSession *s) {
                        auto &d = *s->d;
                        if (epoch != d.audioEpoch)
                            return;
                        d.resetAudio();
                        d.anchorFrame = d.frame;
                        d.clock.restart();
                        emit s->status("Audio: " + message);
                    });
                }
            },
            JobKind::Audio);
    }
};
#ifdef OPENFRAME_TESTING
void ReviewSessionTestAccess::beforeNextPresentation(ReviewSession &session,
                                                     std::function<void()> operation) {
    auto impl = session.d.get();
    impl->enqueue([impl, operation = std::move(operation)](MediaEngine &) mutable {
        impl->beforePresentationTestHook = std::move(operation);
    });
}
void ReviewSessionTestAccess::enqueueWorker(ReviewSession &session,
                                            std::function<void()> operation) {
    session.d->enqueue([operation = std::move(operation)](MediaEngine &) { operation(); });
}
#endif
ReviewSession::ReviewSession(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    requestFrame();
}
ReviewSession::~ReviewSession() = default;
const Project &ReviewSession::project() const {
    return d->project;
}
qint64 ReviewSession::currentFrame() const {
    return d->frame;
}
bool ReviewSession::playing() const {
    return d->playing;
}
double ReviewSession::playbackRate() const {
    return d->rate;
}
qint64 ReviewSession::durationFrames() const {
    auto id = d->owner();
    if (auto source = d->project.source(id))
        return source->info.frameCount();
    return d->project.durationFrames(id);
}
QString ReviewSession::activeOwnerId() const {
    return d->owner();
}
QString ReviewSession::projectPath() const {
    return d->path;
}
bool ReviewSession::dirty() const {
    if (d->untrackedDirty || !d->undo.isClean())
        return true;
    for (const auto &entry : d->annotationStacks)
        if (!entry.second->isClean())
            return true;
    return false;
}
QImage ReviewSession::displayImage() const {
    return d->image;
}
QUndoStack *ReviewSession::undoStack() {
    return &d->undo;
}
QUndoStack *ReviewSession::annotationUndoStack(const QString &ownerId, qint64 frame,
                                               bool background) {
    auto owner = ownerId.isEmpty() ? d->owner() : ownerId;
    auto key = owner + QChar(0x1f) + QString::number(background ? 0 : frame) +
               (background ? ":background" : ":frame");
    auto found = d->annotationStacks.find(key);
    if (found != d->annotationStacks.end())
        return found->second.get();
    auto stack = std::make_unique<QUndoStack>();
    auto result = stack.get();
    connect(result, &QUndoStack::cleanChanged, this, [this]() { emit projectChanged(); });
    d->annotationStacks.emplace(key, std::move(stack));
    return result;
}
void ReviewSession::mutateAnnotations(const QString &label, const QString &ownerId, qint64 frame,
                                      bool background,
                                      const std::function<void(Project &)> &operation) {
    auto owner = ownerId.isEmpty() ? d->owner() : ownerId;
    auto inScope = [=](const Annotation &a) {
        return a.timelineId == owner && a.background == background &&
               (background || a.frame == frame);
    };
    Project next = d->project;
    operation(next);
    QVector<Annotation> before, after;
    for (const auto &a : d->project.annotations)
        if (inScope(a))
            before.append(a);
    for (auto a : next.annotations)
        if (inScope(a)) {
            if (a.id.isEmpty())
                a.id = newId();
            QString annotationError;
            if (!validateAnnotation(a, &annotationError)) {
                emit error(annotationError);
                return;
            }
            after.append(a);
        }
    if (before == after)
        return;
    std::optional<Bookmark> automaticBookmark;
    if (!background && before.isEmpty() && !after.isEmpty()) {
        bool exists = false;
        for (const auto &b : d->project.bookmarks)
            if (b.timelineId == owner && b.frame == frame)
                exists = true;
        if (!exists) {
            Bookmark b;
            b.id = newId();
            b.timelineId = owner;
            b.frame = frame;
            b.endFrame = frame;
            b.name = "Annotation";
            automaticBookmark = b;
        }
    }
    annotationUndoStack(owner, frame, background)
        ->push(new AnnotationCommand(
            label, std::move(before), std::move(after),
            [this, inScope, automaticBookmark](const QVector<Annotation> &target) {
                d->invalidatePreview();
                auto &records = d->project.annotations;
                records.erase(std::remove_if(records.begin(), records.end(), inScope),
                              records.end());
                records += target;
                if (automaticBookmark) {
                    auto &bookmarks = d->project.bookmarks;
                    if (target.isEmpty())
                        bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                                       [&](const Bookmark &b) {
                                                           return b.id == automaticBookmark->id;
                                                       }),
                                        bookmarks.end());
                    else {
                        bool exists = false;
                        for (const auto &b : bookmarks)
                            if (b.timelineId == automaticBookmark->timelineId &&
                                b.frame == automaticBookmark->frame)
                                exists = true;
                        if (!exists)
                            bookmarks.append(*automaticBookmark);
                    }
                }
                ++d->presentationEpoch;
                emit projectChanged();
                requestFrame();
            }));
}
bool ReviewSession::busy() const {
    return d->imports > 0;
}
bool ReviewSession::exporting() const {
    return d->exporting;
}
void ReviewSession::newProject() {
    pause();
    ++d->presentationEpoch;
    ++d->projectEpoch;
    d->imports = 0;
    d->path.clear();
    d->frame = 0;
    d->invalidatePreview();
    d->project = emptyProject();
    d->project.sourceRootDirectory = d->defaultSourceRoot;
    for (auto &t : d->project.timelines) {
        t.timecodeOffset = d->defaultOffset;
        t.timecodeOffsetEnabled = d->defaultOffset != 1;
    }
    if (!d->defaultAddTimeline) {
        d->project.timelines.clear();
        d->project.activeTimelineId.clear();
        d->project.defaultTimelineId.clear();
    } else if (!d->defaultActivateTimeline)
        d->project.activeTimelineId.clear();
    d->untrackedDirty = false;
    d->bookmarkMode = false;
    d->undo.clear();
    d->annotationStacks.clear();
    d->enqueue([](MediaEngine &e) { e.clear(); });
    emit busyChanged(false);
    emit projectChanged();
    emit frameChanged(0);
    requestFrame();
}
bool ReviewSession::openProject(const QString &path) {
    Project p;
    QString errorMessage;
    if (!loadProjectFile(path, p, &errorMessage)) {
        emit error(errorMessage);
        return false;
    }
    if (!d->restorePanZoom) {
        p.viewer.panZoom = {};
        p.panZoomByOwner.clear();
        for (auto &transform : p.viewer.transforms) {
            transform.zoom = 1;
            transform.pan = {};
        }
        for (auto &transform : p.viewTransforms) {
            transform.zoom = 1;
            transform.pan = {};
        }
    }
    if (!d->restoreHold) {
        p.viewer.holdAnnotations = false;
        for (auto &a : p.annotations)
            a.hold = false;
    }
    pause();
    ++d->presentationEpoch;
    ++d->projectEpoch;
    d->imports = 0;
    d->untrackedDirty = false;
    d->bookmarkMode = false;
    d->path = QFileInfo(path).absoluteFilePath();
    d->invalidatePreview();
    d->project = std::move(p);
    d->frame = 0;
    d->undo.clear();
    d->annotationStacks.clear();
    d->enqueue([](MediaEngine &e) { e.clear(); });
    emit busyChanged(false);
    emit projectChanged();
    emit frameChanged(0);
    requestFrame();
    refreshLegacyMetadata();
    return true;
}
void ReviewSession::refreshLegacyMetadata() {
    QVector<MediaSource> pending;
    for (const auto &source : d->project.sources)
        if (source.info.hasVideo && !source.missing &&
            (source.info.color.componentBits == 0 || source.info.color.pixelFormat.isEmpty() ||
             source.info.pictureTypes.size() != source.info.frameCount() ||
             source.info.keyFrames.size() != source.info.frameCount()))
            pending.append(source);
    if (pending.isEmpty())
        return;
    ++d->imports;
    emit busyChanged(true);
    const auto epoch = d->projectEpoch.load();
    d->enqueueImport([this, epoch, pending = std::move(pending)](MediaEngine &) {
        for (const auto &old : pending) {
            if (d->stop || epoch != d->projectEpoch)
                break;
            try {
                auto cancel = [this, epoch] { return d->stop || epoch != d->projectEpoch; };
                auto fresh =
                    old.kind == SourceKind::Sequence || old.kind == SourceKind::Image
                        ? probeImageSequence(old.sequencePaths.isEmpty() ? QStringList{old.path}
                                                                         : old.sequencePaths,
                                             old.info.fps, cancel)
                        : probeMedia(old.path, cancel);
                if (fresh.info.framePts != old.info.framePts ||
                    fresh.info.timeBase != old.info.timeBase)
                    throw std::runtime_error("Source presentation index changed; relink this "
                                             "source to refresh its media data");
                d->post([epoch, id = old.id, path = old.path, paths = old.sequencePaths,
                         info = std::move(fresh.info)](ReviewSession *session) {
                    if (epoch != session->d->projectEpoch)
                        return;
                    for (auto &source : session->d->project.sources)
                        if (source.id == id && source.path == path &&
                            source.sequencePaths == paths) {
                            source.info.color = info.color;
                            source.info.frameColors = info.frameColors;
                            source.info.pictureTypes = info.pictureTypes;
                            source.info.keyFrames = info.keyFrames;
                            session->d->invalidatePreview();
                            emit session->projectChanged();
                            session->requestFrame();
                            break;
                        }
                });
            } catch (const std::exception &e) {
                d->post([epoch, message = QString::fromUtf8(e.what())](ReviewSession *session) {
                    if (epoch == session->d->projectEpoch)
                        emit session->error(message);
                });
            }
        }
        d->post([epoch](ReviewSession *session) {
            if (epoch == session->d->projectEpoch) {
                session->d->imports = std::max(0, session->d->imports - 1);
                emit session->busyChanged(session->busy());
            }
        });
    });
}
bool ReviewSession::saveProject(const QString &path) {
    auto destination = path.isEmpty() ? d->path : path;
    if (destination.isEmpty()) {
        emit error("Choose a project filename");
        return false;
    }
    QString message;
    if (!saveProjectFile(d->project, destination, &message)) {
        emit error(message);
        return false;
    }
    d->path = QFileInfo(destination).absoluteFilePath();
    d->untrackedDirty = false;
    d->undo.setClean();
    for (auto &entry : d->annotationStacks)
        entry.second->setClean();
    emit projectChanged();
    emit status("Project saved");
    return true;
}
bool ReviewSession::importProject(const QString &path) {
    Project imported;
    QString message;
    if (!loadProjectFile(path, imported, &message)) {
        emit error(message);
        return false;
    }
    mutateProject("Import project", [&](Project &p) {
        QHash<QString, QString> ids;
        auto map = [&](QString &id) {
            if (id.isEmpty())
                return;
            if (!ids.contains(id))
                ids.insert(id, newId());
            id = ids[id];
        };
        for (auto &s : imported.sources) {
            map(s.id);
            map(s.folderId);
        }
        for (auto &f : imported.folders) {
            map(f.id);
            map(f.parentId);
        }
        for (auto &t : imported.timelines) {
            map(t.id);
            map(t.folderId);
            for (auto &c : t.clips) {
                map(c.id);
                map(c.sourceId);
                map(c.audioSourceId);
            }
        }
        for (auto &b : imported.bookmarks) {
            map(b.id);
            map(b.timelineId);
        }
        for (auto &n : imported.notes) {
            map(n.id);
            map(n.timelineId);
        }
        for (auto &a : imported.annotations) {
            map(a.id);
            map(a.timelineId);
        }
        for (auto it = imported.panZoomByOwner.cbegin(); it != imported.panZoomByOwner.cend();
             ++it) {
            auto owner = it.key();
            map(owner);
            p.panZoomByOwner[owner] = it.value();
        }
        for (auto it = imported.viewTransforms.cbegin(); it != imported.viewTransforms.cend();
             ++it) {
            const auto split = it.key().lastIndexOf(':');
            if (split < 0)
                continue;
            auto owner = it.key().left(split);
            map(owner);
            p.viewTransforms[owner + it.key().mid(split)] = it.value();
        }
        p.sources += imported.sources;
        p.folders += imported.folders;
        p.timelines += imported.timelines;
        p.bookmarks += imported.bookmarks;
        p.notes += imported.notes;
        p.annotations += imported.annotations;
    });
    return true;
}
void ReviewSession::importMedia(const QStringList &inputPaths) {
    auto paths = rootedPaths(inputPaths, d->project.sourceRootDirectory);
    if (paths.isEmpty())
        return;
    auto defaultOffset = d->defaultOffset;
    auto sequenceFps = d->sequenceFps;
    auto epoch = d->projectEpoch.load();
    ++d->imports;
    emit busyChanged(true);
    d->enqueueImport([this, paths, epoch, defaultOffset, sequenceFps](MediaEngine &) {
        for (const auto &path : paths) {
            if (d->stop || epoch != d->projectEpoch)
                break;
            try {
                d->post([name = QFileInfo(path).fileName()](ReviewSession *s) {
                    emit s->status("Indexing " + name);
                });
                auto source = probeMedia(
                    path, [this, epoch]() { return d->stop || epoch != d->projectEpoch; });
                source.timecodeOffset = defaultOffset;
                source.timecodeOffsetEnabled = defaultOffset != 1;
                if (source.kind == SourceKind::Image) {
                    source.info.fps = sequenceFps;
                    source.info.timeBase = {sequenceFps.den, sequenceFps.num};
                    source.info.durationUs = sequenceFps.frameToUs(1);
                }
                d->post([epoch, source = std::move(source)](ReviewSession *s) {
                    if (epoch != s->d->projectEpoch)
                        return;
                    s->mutateProject("Import media", [&](Project &p) {
                        p.sources.append(source);
                        auto target = p.defaultTimelineId;
                        for (auto &t : p.timelines)
                            if (t.id == target) {
                                Clip c;
                                c.id = newId();
                                c.sourceId = source.id;
                                c.name = source.name;
                                c.inFrame = source.inFrame;
                                c.outFrame = source.outFrame;
                                t.clips.append(c);
                                break;
                            }
                    });
                    emit s->status("Imported " + source.name);
                });
            } catch (const std::exception &e) {
                d->post([epoch, message = QString::fromUtf8(e.what())](ReviewSession *s) {
                    if (epoch == s->d->projectEpoch)
                        emit s->error(message);
                });
            }
        }
        d->post([epoch](ReviewSession *s) {
            if (epoch != s->d->projectEpoch)
                return;
            s->d->imports = std::max(0, s->d->imports - 1);
            emit s->busyChanged(s->busy());
            if (!s->busy() && s->d->autoplay)
                s->play();
        });
    });
}
void ReviewSession::importImageSequence(const QStringList &inputPaths, Rational fps) {
    auto paths = rootedPaths(inputPaths, d->project.sourceRootDirectory);
    if (paths.isEmpty())
        return;
    if (!fps.valid())
        fps = d->sequenceFps;
    auto defaultOffset = d->defaultOffset;
    auto epoch = d->projectEpoch.load();
    ++d->imports;
    emit busyChanged(true);
    d->enqueueImport([this, paths, fps, epoch, defaultOffset](MediaEngine &) {
        try {
            auto source = probeImageSequence(
                paths, fps, [this, epoch]() { return d->stop || epoch != d->projectEpoch; });
            source.timecodeOffset = defaultOffset;
            source.timecodeOffsetEnabled = defaultOffset != 1;
            d->post([epoch, source = std::move(source)](ReviewSession *s) {
                if (epoch != s->d->projectEpoch)
                    return;
                s->mutateProject("Import image sequence", [&](Project &p) {
                    p.sources.append(source);
                    for (auto &t : p.timelines)
                        if (t.id == (p.defaultTimelineId)) {
                            Clip c;
                            c.id = newId();
                            c.sourceId = source.id;
                            c.name = source.name;
                            t.clips.append(c);
                            break;
                        }
                });
            });
        } catch (const std::exception &e) {
            d->post([epoch, message = QString::fromUtf8(e.what())](ReviewSession *s) {
                if (epoch == s->d->projectEpoch)
                    emit s->error(message);
            });
        }
        d->post([epoch](ReviewSession *s) {
            if (epoch != s->d->projectEpoch)
                return;
            s->d->imports = std::max(0, s->d->imports - 1);
            emit s->busyChanged(s->busy());
            if (!s->busy() && s->d->autoplay)
                s->play();
        });
    });
}
void ReviewSession::relinkSource(const QString &sourceId, const QString &path) {
    auto previous = d->project.source(sourceId);
    if (!previous) {
        emit error("Source does not exist");
        return;
    }
    auto original = *previous;
    auto epoch = d->projectEpoch.load();
    ++d->imports;
    emit busyChanged(true);
    d->enqueueImport([this, sourceId, path, epoch,
                      original = std::move(original)](MediaEngine &engine) {
        try {
            MediaSource source;
            auto cancelled = [this, epoch]() { return d->stop || epoch != d->projectEpoch; };
            if (original.kind == SourceKind::Sequence) {
                QStringList sequence;
                auto directory = QFileInfo(path).absolutePath();
                QRegularExpression numbering("^(.*?)([0-9]+)(\\.[^.]+)$");
                auto oldFirst =
                    numbering.match(QFileInfo(original.sequencePaths.value(0)).fileName());
                auto newFirst = numbering.match(QFileInfo(path).fileName());
                for (qsizetype i = 0; i < original.sequencePaths.size(); ++i) {
                    auto name = QFileInfo(original.sequencePaths[i]).fileName();
                    auto match = numbering.match(name);
                    if (oldFirst.hasMatch() && newFirst.hasMatch() && match.hasMatch() &&
                        match.captured(1) == oldFirst.captured(1) &&
                        match.captured(3) == oldFirst.captured(3)) {
                        bool oldOk = false, newOk = false, indexOk = false;
                        auto oldIndex = oldFirst.captured(2).toLongLong(&oldOk),
                             newIndex = newFirst.captured(2).toLongLong(&newOk),
                             index = match.captured(2).toLongLong(&indexOk);
                        if (oldOk && newOk && indexOk && oldIndex < 1000000000 &&
                            newIndex < 1000000000 && index < 1000000000)
                            name = newFirst.captured(1) +
                                   QString("%1").arg(newIndex + index - oldIndex,
                                                     newFirst.captured(2).size(), 10, QChar('0')) +
                                   newFirst.captured(3);
                    }
                    sequence.append(i == 0 ? path : QDir(directory).filePath(name));
                }
                source = probeImageSequence(sequence, original.info.fps, cancelled);
            } else
                source = probeMedia(path, cancelled);
            source.id = sourceId;
            engine.clear();
            d->enqueue([](MediaEngine &e) { e.clear(); });
            d->post([source = std::move(source), epoch](ReviewSession *s) {
                if (epoch != s->d->projectEpoch)
                    return;
                s->mutateProject("Relink source", [&](Project &p) {
                    for (auto &old : p.sources)
                        if (old.id == source.id) {
                            auto replacement = source;
                            replacement.folderId = old.folderId;
                            replacement.name = old.name;
                            replacement.inFrame =
                                std::clamp<qint64>(old.inFrame, 0, source.info.frameCount() - 1);
                            replacement.outFrame =
                                old.outFrame < 0
                                    ? -1
                                    : std::clamp<qint64>(old.outFrame, replacement.inFrame,
                                                         source.info.frameCount() - 1);
                            replacement.timecodeOffset = old.timecodeOffset;
                            replacement.timecodeOffsetEnabled = old.timecodeOffsetEnabled;
                            replacement.audioOffsetUs = old.audioOffsetUs;
                            if (!selectSourceAudioTrack(replacement, old.selectedAudioTrack))
                                selectSourceAudioTrack(replacement, -1);
                            old = std::move(replacement);
                        }
                    for (auto &t : p.timelines)
                        for (auto &c : t.clips)
                            if (c.sourceId == source.id) {
                                c.inFrame =
                                    std::clamp<qint64>(c.inFrame, 0, source.info.frameCount() - 1);
                                if (c.outFrame >= 0)
                                    c.outFrame = std::clamp<qint64>(c.outFrame, c.inFrame,
                                                                    source.info.frameCount() - 1);
                            }
                });
            });
        } catch (const std::exception &e) {
            d->post([message = QString::fromUtf8(e.what())](ReviewSession *s) {
                emit s->error(message);
            });
        }
        d->post([epoch](ReviewSession *s) {
            if (epoch != s->d->projectEpoch)
                return;
            s->d->imports = std::max(0, s->d->imports - 1);
            emit s->busyChanged(s->busy());
            if (!s->busy() && s->d->autoplay)
                s->play();
        });
    });
}
void ReviewSession::applyProject(const Project &p) {
    for (const auto &source : p.sources) {
        auto old = d->project.source(source.id);
        if (old && (old->path != source.path || old->sequencePaths != source.sequencePaths ||
                    old->selectedAudioTrack != source.selectedAudioTrack ||
                    old->audioOffsetUs != source.audioOffsetUs)) {
            d->resetAudio();
            d->enqueue([](MediaEngine &engine) { engine.clear(); });
            break;
        }
    }
    ++d->presentationEpoch;
    const bool running = d->playing;
    d->invalidatePreview();
    d->project = p;
    d->frame = std::clamp<qint64>(d->frame, 0, std::max<qint64>(0, durationFrames() - 1));
    if (running)
        d->startTransport();
    emit projectChanged();
    emit frameChanged(d->frame);
    requestFrame();
}
void ReviewSession::mutateProject(const QString &label,
                                  const std::function<void(Project &)> &operation) {
    Project next = d->project;
    operation(next);
    if (!d->projectUndo) {
        d->untrackedDirty = true;
        applyProject(next);
        return;
    }
    d->undo.push(new SnapshotCommand(
        label, d->project, std::move(next), [this](const Project &p, const Project &baseline) {
            auto merged = p;
            merged.annotations =
                mergeChangedRecords(d->project.annotations, p.annotations, baseline.annotations);
            merged.bookmarks =
                mergeChangedRecords(d->project.bookmarks, p.bookmarks, baseline.bookmarks);
            applyProject(merged);
        }));
}
void ReviewSession::seek(qint64 frame, bool scrub) {
    seekInternal(frame, scrub, scrub);
}
void ReviewSession::navigate(qint64 frame, bool scrub) {
    seekInternal(frame, true, scrub, true);
}
void ReviewSession::seekInternal(qint64 frame, bool navigation, bool scrub, bool allowProgress) {
    qint64 first = 0, last = std::max<qint64>(0, durationFrames() - 1);
    if (navigation && (d->lockStepRange || (d->loopStep && d->loopStepRange))) {
        first = std::clamp<qint64>(d->project.rangeIn, 0, last);
        if (d->project.rangeOut >= 0)
            last = std::clamp<qint64>(d->project.rangeOut, first, last);
    }
    qint64 bounded;
    if (navigation && d->loopStep && !d->lockStepRange) {
        auto length = last - first + 1;
        bounded = first + ((frame - first) % length + length) % length;
    } else
        bounded = std::clamp<qint64>(frame, first, last);
    if (d->bookmarkMode) {
        auto bookmarks = d->bookmarks();
        if (bookmarks.isEmpty()) {
            emit error("No bookmarks in the playback range");
            return;
        }
        bounded = heldBookmark(bookmarks, bounded);
    }
    // Mouse motion can report the same integer frame repeatedly. Retain the
    // in-flight exact result instead of perpetually invalidating it.
    if (allowProgress && bounded == d->frame)
        return;
    // During a drag, allow bounded progress from exact previously requested frames.
    // An absolute seek (including mouse release) is a hard latest-frame barrier.
    if (!allowProgress || d->playing)
        ++d->presentationEpoch;
    d->frame = bounded;
    if (d->playing)
        d->startTransport();
    else
        d->resetAudio(); // Invalidate pending/playing scrub even when the new seek is silent.
    emit frameChanged(d->frame);
    requestFrame(scrub && !d->playing);
}
void ReviewSession::step(qint64 frames) {
    pause();
    seek(d->frame + frames);
}
void ReviewSession::play() {
    if (d->bookmarkMode && d->playing)
        pause();
    setBookmarkPlayback(false);
    startPlayback();
}
void ReviewSession::playBookmarks() {
    if (d->playing)
        pause();
    setBookmarkPlayback(true);
    if (d->bookmarkMode)
        startPlayback();
}
void ReviewSession::startPlayback() {
    if (d->playing || durationFrames() <= 0)
        return;
    auto first = std::clamp<qint64>(d->project.rangeIn, 0, durationFrames() - 1);
    auto last = d->project.rangeOut < 0
                    ? durationFrames() - 1
                    : std::clamp<qint64>(d->project.rangeOut, first, durationFrames() - 1);
    if (d->frame < first || d->frame > last)
        d->frame = d->rate < 0 ? last : first;
    d->playing = true;
    d->startTransport();
    d->timer.start();
    emit playbackChanged(true);
    if (d->hideAnnotations)
        requestFrame();
}
void ReviewSession::pause() {
    const bool wasEmptyDelay = d->delayActive && d->project.loopHoldEmpty;
    d->delayActive = false;
    if (d->playing) {
        d->playing = false;
        d->timer.stop();
        emit playbackChanged(false);
        if (d->hideAnnotations)
            requestFrame();
    }
    d->resetAudio();
    if (wasEmptyDelay) {
        ++d->presentationEpoch;
        requestFrame();
    }
}
void ReviewSession::togglePlayback() {
    d->playing ? pause() : play();
}
void ReviewSession::setPlaybackRate(double rate) {
    if (!std::isfinite(rate) || std::abs(rate) < .0625 || std::abs(rate) > 16) {
        emit error("Playback rate must be from 0.0625 to 16, or negative for reverse");
        return;
    }
    d->rate = rate;
    if (d->playing)
        d->startTransport();
    emit transportSettingsChanged();
    emit status(QString("Playback speed: %1x").arg(rate));
}
void ReviewSession::setLoopMode(LoopMode mode) {
    if (d->project.loopMode == mode)
        return;
    mutateProject("Playback loop mode", [&](Project &p) { p.loopMode = mode; });
}
void ReviewSession::setLoopDelay(int milliseconds, bool holdEmpty) {
    if (milliseconds < 0 || milliseconds > 600000) {
        emit error("Loop delay must be between 0 and 600000 milliseconds");
        return;
    }
    if (d->project.loopDelayMs == milliseconds && d->project.loopHoldEmpty == holdEmpty)
        return;
    mutateProject("Loop delay", [&](Project &p) {
        p.loopDelayMs = milliseconds;
        p.loopHoldEmpty = holdEmpty;
    });
    emit transportSettingsChanged();
}
bool ReviewSession::loopDelayActive() const {
    return d->delayActive;
}
void ReviewSession::setSourceAudioTrack(const QString &sourceId, int streamIndex) {
    const auto source = d->project.source(sourceId);
    if (!source) {
        emit error("Audio source does not exist");
        return;
    }
    if (source->selectedAudioTrack == streamIndex)
        return;
    auto changed = *source;
    QString message;
    if (!selectSourceAudioTrack(changed, streamIndex, &message)) {
        emit error(message);
        return;
    }
    mutateProject("Select audio track", [&](Project &p) {
        for (auto &s : p.sources)
            if (s.id == sourceId)
                s = changed;
    });
}
void ReviewSession::setSourceAudioOffset(const QString &sourceId, qint64 offsetUs) {
    const auto source = d->project.source(sourceId);
    QString message;
    if (!source) {
        emit error("Audio source does not exist");
        return;
    }
    if (!validateSourceAudioOffset(offsetUs, &message)) {
        emit error(message);
        return;
    }
    if (source->audioOffsetUs == offsetUs)
        return;
    mutateProject("Source audio offset", [&](Project &p) {
        for (auto &s : p.sources)
            if (s.id == sourceId)
                s.audioOffsetUs = offsetUs;
    });
}
void ReviewSession::setRange(qint64 first, qint64 last) {
    if (last < 0)
        last = std::max<qint64>(0, durationFrames() - 1);
    first = std::clamp<qint64>(first, 0, std::max<qint64>(0, durationFrames() - 1));
    last = std::clamp<qint64>(last, first, std::max<qint64>(first, durationFrames() - 1));
    if (d->project.rangeIn == first && d->project.rangeOut == last)
        return;
    mutateProject("Playback range", [&](Project &p) {
        p.rangeIn = first;
        p.rangeOut = last;
    });
}
void ReviewSession::setActiveTimeline(const QString &id) {
    if (id == d->project.activeTimelineId && d->project.viewer.assignments.isEmpty())
        return;
    if (!d->project.timeline(id)) {
        emit error("Timeline does not exist");
        return;
    }
    pause();
    mutateProject("Activate timeline", [&](Project &p) {
        rememberViews(p);
        p.activeTimelineId = id;
        p.rangeIn = 0;
        p.rangeOut = -1;
        p.viewer.assignments.clear();
        for (int pane = 0; pane < 4; ++pane)
            restoreView(p, pane);
    });
    seek(0, false);
    if (d->autoplay)
        play();
}
void ReviewSession::assignViewer(int pane, const QString &id) {
    if (pane >= 0 && pane < 4 && d->project.viewer.assignments.value(pane) == id)
        return;
    if (pane < 0 || pane > 3 ||
        (!id.isEmpty() && !d->project.source(id) && !d->project.timeline(id))) {
        emit error("Invalid viewer assignment");
        return;
    }
    const bool primary = pane == primaryViewerPane(d->project.viewer);
    if (primary)
        pause();
    mutateProject("Assign viewer", [&](Project &p) {
        if (primary)
            p.panZoomByOwner[viewerPaneOwner(p, pane)] = p.viewer.panZoom;
        while (p.viewer.transforms.size() <= pane)
            p.viewer.transforms.append(ViewTransform{});
        p.viewTransforms[viewKey(p, pane)] = p.viewer.transforms[pane];
        while (p.viewer.assignments.size() <= pane)
            p.viewer.assignments.append(QString{});
        p.viewer.assignments[pane] = id;
        restoreView(p, pane);
        if (primary) {
            p.rangeIn = 0;
            p.rangeOut = -1;
        }
    });
    if (primary && d->autoplay)
        play();
}
void ReviewSession::setViewerSettings(const ViewerSettings &settings) {
    QString message;
    if (!validateViewerSettings(settings, &message)) {
        emit error(message);
        return;
    }
    mutateProject("Viewer settings", [&](Project &p) {
        p.viewer = settings;
        rememberViews(p);
    });
}
quint64 ReviewSession::beginViewerPreview() {
    d->invalidatePreview();
    d->previewToken = ++d->previewSerial;
    d->viewerPreview = d->project.viewer;
    return d->previewToken;
}
bool ReviewSession::previewComparisonMix(quint64 token, double amount) {
    if (!token || token != d->previewToken || !d->viewerPreview ||
        !std::isfinite(amount) || amount < 0 || amount > 1)
        return false;
    auto &viewer = *d->viewerPreview;
    if (viewer.mode == ViewerMode::Blend)
        viewer.opacity = amount;
    else if (viewer.mode == ViewerMode::Wipe)
        viewer.wipe = amount;
    else
        return false;
    if (!d->comparisonPreview)
        d->comparisonPreview = std::make_shared<Impl::ComparisonPreview>();
    requestFrame();
    return true;
}
bool ReviewSession::previewViewerSettings(quint64 token, const ViewerSettings &settings) {
    if (!token || token != d->previewToken)
        return false;
    QString message;
    if (!validateViewerSettings(settings, &message)) {
        emit error(message);
        return false;
    }
    d->comparisonPreview.reset();
    d->viewerPreview = settings;
    requestFrame();
    return true;
}
bool ReviewSession::previewAnnotation(quint64 token, const Annotation &annotation) {
    if (!token || token != d->previewToken)
        return false;
    QString message;
    if (!validateAnnotation(annotation, &message)) {
        emit error(message);
        return false;
    }
    d->comparisonPreview.reset();
    d->annotationPreview = annotation;
    requestFrame();
    return true;
}
bool ReviewSession::commitViewerPreview(quint64 token, const QString &label) {
    if (!token || token != d->previewToken || !d->viewerPreview)
        return false;
    auto settings = *d->viewerPreview;
    d->invalidatePreview();
    mutateProject(label, [&](Project &p) {
        p.viewer = settings;
        rememberViews(p);
    });
    return true;
}
void ReviewSession::clearViewerPreview(quint64 token) {
    if (token && token == d->previewToken) {
        d->invalidatePreview();
        requestFrame();
    }
}
ViewerSettings ReviewSession::displayedViewerSettings() const {
    return d->viewerPreview.value_or(d->project.viewer);
}
QString ReviewSession::addTimeline(const QString &name) {
    auto id = newId();
    mutateProject("Add timeline", [&](Project &p) {
        Timeline t;
        t.id = id;
        t.name = name;
        t.fps = p.fps;
        t.timecodeOffset = d->defaultOffset;
        t.timecodeOffsetEnabled = d->defaultOffset != 1;
        p.timelines.append(t);
    });
    return id;
}
void ReviewSession::appendSource(const QString &sourceId, const QString &timelineId) {
    auto source = d->project.source(sourceId);
    if (!source)
        return;
    auto target = timelineId.isEmpty() ? d->project.activeTimelineId : timelineId;
    mutateProject("Append clip", [&](Project &p) {
        for (auto &t : p.timelines)
            if (t.id == target) {
                Clip c;
                c.id = newId();
                c.sourceId = sourceId;
                c.name = source->name;
                c.inFrame = source->inFrame;
                c.outFrame = source->outFrame;
                t.clips.append(c);
                break;
            }
    });
}
void ReviewSession::removeSource(const QString &sourceId) {
    mutateProject("Remove source", [&](Project &p) {
        p.sources.erase(std::remove_if(p.sources.begin(), p.sources.end(),
                                       [&](auto &s) { return s.id == sourceId; }),
                        p.sources.end());
        for (auto &t : p.timelines) {
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                         [&](auto &c) { return c.sourceId == sourceId; }),
                          t.clips.end());
            for (auto &c : t.clips)
                if (c.audioSourceId == sourceId)
                    c.audioSourceId.clear();
        }
        for (auto &id : p.viewer.assignments)
            if (id == sourceId)
                id.clear();
    });
}
void ReviewSession::addBookmark(const QString &name, qint64 first, qint64 last) {
    mutateProject("Add bookmark", [&](Project &p) {
        Bookmark b;
        b.id = newId();
        b.timelineId = d->owner();
        b.frame = first;
        b.endFrame = last < 0 ? first : last;
        b.name = name;
        p.bookmarks.append(b);
    });
}
void ReviewSession::addNote(const QString &text, const QString &author) {
    mutateProject("Add note", [&](Project &p) {
        Note n;
        n.id = newId();
        n.timelineId = d->owner();
        n.frame = d->frame;
        n.text = text;
        n.author = author;
        p.notes.append(n);
    });
}
void ReviewSession::addAnnotation(const Annotation &annotation) {
    auto a = annotation;
    if (a.id.isEmpty())
        a.id = newId();
    if (a.timelineId.isEmpty())
        a.timelineId = d->owner();
    mutateAnnotations("Add annotation", a.timelineId, a.frame, a.background,
                      [&](Project &p) { p.annotations.append(a); });
}
bool ReviewSession::saveNotesCsv(const QString &path) {
    QString message;
    if (!exportNotesCsv(d->project, path, &message)) {
        emit error(message);
        return false;
    }
    return true;
}
bool ReviewSession::loadNotesCsv(const QString &path) {
    auto p = d->project;
    QString message;
    if (!importNotesCsv(p, path, &message)) {
        emit error(message);
        return false;
    }
    mutateProject("Import notes", [&](Project &target) { target.notes = p.notes; });
    return true;
}
void ReviewSession::requestFrame(bool scrub) {
    auto epoch = ++d->renderEpoch;
    auto presentation = d->presentationEpoch.load();
    auto snapshot = d->project;
    if (d->viewerPreview)
        snapshot.viewer = *d->viewerPreview;
    if (d->annotationPreview)
        snapshot.annotations.append(*d->annotationPreview);
    if (d->playing && d->hideAnnotations)
        snapshot.viewer.showAnnotations = false;
    auto frame = d->frame;
    const bool emptyFrame = d->delayActive && d->project.loopHoldEmpty;
    d->enqueue(
        [this, epoch, presentation, snapshot = std::move(snapshot), frame,
         emptyFrame, comparison = d->comparisonPreview](MediaEngine &engine) {
            if (d->stop || epoch != d->renderEpoch)
                return;
            try {
                QElapsedTimer operationClock;
                operationClock.start();
                MediaOperation operation(engine, [this, epoch, presentation, operationClock,
                                                  mixing = bool(comparison)] {
                    return d->stop || presentation != d->presentationEpoch ||
                           (!mixing && epoch != d->renderEpoch && operationClock.elapsed() >= 80);
                });
                QImage image;
                if (emptyFrame) {
                    image = QImage(snapshot.viewer.outputSize, QImage::Format_RGBA8888);
                    image.fill(snapshot.viewer.background);
                } else {
                    if (comparison && (comparison->frame != frame ||
                                       comparison->presentation != presentation ||
                                       comparison->showAnnotations != snapshot.viewer.showAnnotations)) {
                        comparison->layers = {};
                        comparison->frame = frame;
                        comparison->presentation = presentation;
                        comparison->showAnnotations = snapshot.viewer.showAnnotations;
                    }
                    image = FrameComposer::compose(snapshot, frame, engine, {}, true,
                                                   comparison ? &comparison->layers : nullptr);
                }
#ifdef OPENFRAME_TESTING
                if (auto hook = std::exchange(d->beforePresentationTestHook, {}))
                    hook();
#endif
                d->post([epoch, presentation, frame, image = std::move(image)](ReviewSession *s) {
                    if (presentation != s->d->presentationEpoch || epoch <= s->d->presentedSerial)
                        return;
                    s->d->presentedSerial = epoch;
                    s->d->renderedFrame = frame;
                    s->d->image = image;
                    emit s->imageReady(image);
                });
            } catch (const std::exception &e) {
                d->post([epoch, message = QString::fromUtf8(e.what())](ReviewSession *s) {
                    if (epoch == s->d->renderEpoch)
                        emit s->error(message);
                });
            }
        },
        Impl::JobKind::Render);
    const auto assignment = evaluateAssignment(d->project, d->owner(), d->frame);
    const auto scrubSource = assignment ? d->project.source(assignment->audioSourceId) : nullptr;
    if (scrub && d->audioEnabled && d->scrubEnabled && scrubSource && scrubSource->info.hasAudio &&
        !assignment->muted && assignment->gain != 0) {
        d->resetAudio();
        if (d->openAudio())
            d->queueAudio(0, 100000, true);
    }
}
void ReviewSession::tick() {
    if (!d->playing)
        return;
    if (d->delayActive) {
        if (d->delayClock.elapsed() < d->project.loopDelayMs)
            return;
        const bool directionChanged = d->rate != d->delayResumeRate;
        d->rate = d->delayResumeRate;
        d->frame = d->delayResumeFrame;
        d->startTransport();
        if (directionChanged)
            emit transportSettingsChanged();
        emit frameChanged(d->frame);
        requestFrame();
        return;
    }
    auto elapsed = d->clock.nsecsElapsed() / 1000;
    if (d->audioClock && d->sink) {
        if ((!d->audioHadData && elapsed > 5000000) ||
            (d->sink->error() != QAudio::NoError && d->sink->error() != QAudio::UnderrunError)) {
            d->resetAudio();
            d->anchorFrame = d->frame;
            d->clock.restart();
            elapsed = 0;
            emit status("Audio output unavailable; using monotonic clock");
        } else
            elapsed = d->playedAudioUs();
        if (d->audioClock && d->pendingAudio < 3 && d->scheduledAudioUs - elapsed < 1000000) {
            d->queueAudio(d->scheduledAudioUs, 500000, false);
            d->scheduledAudioUs += 500000;
        }
    }
    auto f =
        d->anchorFrame + (d->rate < 0 ? -1 : 1) *
                             d->fps().usToFrame(static_cast<qint64>(elapsed * std::abs(d->rate)));
    if (auto source = d->project.source(d->owner())) {
        const auto anchorUs =
            d->rate < 0 ? d->timeAfterFrame(d->anchorFrame) - 1 : d->timeAtFrame(d->anchorFrame);
        auto us = anchorUs + static_cast<qint64>(elapsed * d->rate);
        f = us < 0 ? -1
            : us >= source->info.frameEndUs(source->info.frameCount() - 1)
                ? source->info.frameCount()
                : source->info.frameAtUs(us);
    }
    auto first =
        std::clamp<qint64>(d->project.rangeIn, 0, std::max<qint64>(0, durationFrames() - 1));
    auto last = d->project.rangeOut < 0 ? durationFrames() - 1
                                        : std::clamp<qint64>(d->project.rangeOut, first,
                                                             std::max(first, durationFrames() - 1));
    if (f < first || f > last) {
        if (d->project.loopMode == LoopMode::Once) {
            pause();
            seek(d->rate < 0 ? first : last, false);
            return;
        }
        if (d->project.loopDelayMs > 0) {
            d->frame = d->rate < 0 ? first : last;
            d->delayResumeRate = d->project.loopMode == LoopMode::PingPong ? -d->rate : d->rate;
            d->delayResumeFrame = d->project.loopMode == LoopMode::PingPong ? d->frame
                                  : d->rate < 0                             ? last
                                                                            : first;
            d->resetAudio();
            d->delayActive = true;
            d->delayClock.restart();
            ++d->presentationEpoch;
            emit frameChanged(d->frame);
            requestFrame();
            return;
        }
        if (d->project.loopMode == LoopMode::PingPong) {
            d->rate = -d->rate;
            d->frame = d->rate < 0 ? last : first;
        } else
            d->frame = d->rate < 0 ? last : first;
        d->startTransport();
        f = d->frame;
    }
    if (d->bookmarkMode) {
        auto bookmarks = d->bookmarks();
        if (bookmarks.isEmpty()) {
            pause();
            setBookmarkPlayback(false);
            emit error("No bookmarks in the playback range");
            return;
        }
        f = heldBookmark(bookmarks, f);
    } else if (d->frameStep > 1) {
        auto delta = f - d->anchorFrame;
        f = d->anchorFrame + (delta < 0 ? -1 : 1) * (std::abs(delta) / d->frameStep) * d->frameStep;
        f = std::clamp<qint64>(f, first, last);
    }
    if (f != d->frame) {
        d->frame = f;
        emit frameChanged(f);
        requestFrame();
    }
}
bool ReviewSession::exportAsync(const ExportSettings &settings) {
    if (d->exporting) {
        emit error("An export is already running");
        return false;
    }
    if (d->exportThread.joinable())
        d->exportThread.join();
    d->exportCancel = false;
    d->exporting = true;
    auto snapshot = d->project;
    d->exportThread = std::jthread([this, snapshot = std::move(snapshot), settings]() {
        bool ok = false;
        QString message;
        QString publishedFile;
        try {
            publishedFile = exportProject(
                snapshot, settings, [this]() { return d->stop || d->exportCancel; },
                [this](int value) {
                    d->post([value](ReviewSession *s) { emit s->exportProgress(value); });
                });
            ok = true;
            message = settings.path;
        } catch (const std::exception &e) {
            message = QString::fromUtf8(e.what());
        }
        d->post([ok, message, publishedFile](ReviewSession *s) {
            s->d->exporting = false;
            emit s->exportFinished(ok, message, publishedFile);
        });
    });
    return true;
}
void ReviewSession::cancelExport() {
    d->exportCancel = true;
}
void ReviewSession::setAudioEnabled(bool enabled) {
    if (d->audioEnabled == enabled)
        return;
    d->audioEnabled = enabled;
    emit transportSettingsChanged();
    if (d->playing)
        d->startTransport();
    else if (!enabled)
        d->resetAudio();
}
bool ReviewSession::audioEnabled() const {
    return d->audioEnabled;
}
void ReviewSession::setVolume(double volume) {
    auto bounded = std::clamp(volume, 0.0, 1.0);
    if (d->volume == bounded)
        return;
    d->volume = bounded;
    if (d->sink)
        d->sink->setVolume(d->volume);
    emit transportSettingsChanged();
}
double ReviewSession::volume() const {
    return d->volume;
}
void ReviewSession::setAudioScrubEnabled(bool enabled) {
    if (d->scrubEnabled == enabled)
        return;
    d->scrubEnabled = enabled;
    if (!enabled && !d->playing)
        d->resetAudio();
    emit transportSettingsChanged();
}
bool ReviewSession::audioScrubEnabled() const {
    return d->scrubEnabled;
}
void ReviewSession::setAudioTimeStretchingEnabled(bool enabled) {
    if (d->timeStretching == enabled)
        return;
    d->timeStretching = enabled;
    if (d->playing)
        d->startTransport();
    else
        d->resetAudio();
    emit transportSettingsChanged();
}
bool ReviewSession::audioTimeStretchingEnabled() const {
    return d->timeStretching;
}
void ReviewSession::setLoopOnStepScrub(bool enabled) {
    d->loopStep = enabled;
}
bool ReviewSession::loopOnStepScrub() const {
    return d->loopStep;
}
void ReviewSession::setLoopStepScrubToRange(bool enabled) {
    d->loopStepRange = enabled;
}
bool ReviewSession::loopStepScrubToRange() const {
    return d->loopStepRange;
}
void ReviewSession::setLockStepScrubToRange(bool enabled) {
    d->lockStepRange = enabled;
}
bool ReviewSession::lockStepScrubToRange() const {
    return d->lockStepRange;
}
void ReviewSession::setAutoplay(bool enabled) {
    d->autoplay = enabled;
}
bool ReviewSession::autoplay() const {
    return d->autoplay;
}
void ReviewSession::setHideAnnotationsDuringPlayback(bool enabled) {
    d->hideAnnotations = enabled;
    requestFrame();
}
bool ReviewSession::hideAnnotationsDuringPlayback() const {
    return d->hideAnnotations;
}
QJsonObject ReviewSession::audioDiagnostics() const {
    int pendingWorkerJobs = 0;
    {
        std::lock_guard lock(d->mutex);
        pendingWorkerJobs = static_cast<int>(d->jobs.size());
    }
    const auto device = QMediaDevices::defaultAudioOutput();
    QJsonArray formats;
    for (auto format : device.supportedSampleFormats())
        formats.append(static_cast<int>(format));
    QAudioFormat requested;
    requested.setSampleRate(48000);
    requested.setChannelCount(2);
    requested.setSampleFormat(QAudioFormat::Float);
    return {{"deviceAvailable", !device.isNull()},
            {"deviceDescription", device.description()},
            {"deviceFormats", formats},
            {"deviceMinRate", device.minimumSampleRate()},
            {"deviceMaxRate", device.maximumSampleRate()},
            {"deviceMinChannels", device.minimumChannelCount()},
            {"deviceMaxChannels", device.maximumChannelCount()},
            {"deviceSupportsRequestedFormat", device.isFormatSupported(requested)},
            {"outputSampleRate", d->outputFormat.sampleRate()},
            {"outputChannels", d->outputFormat.channelCount()},
            {"outputSampleFormat", static_cast<int>(d->outputFormat.sampleFormat())},
            {"outputBytesPerFrame", d->outputFormat.bytesPerFrame()},
            {"audioEnabled", d->audioEnabled},
            {"sinkActive", static_cast<bool>(d->sink)},
            {"usingAudioClock", d->audioClock},
            {"processedUs", d->sink ? d->sink->processedUSecs() : qint64(0)},
            {"playedUs", d->playedAudioUs()},
            {"sinkBufferBytes", d->sink ? d->sink->bufferSize() : qint64(0)},
            {"sinkFreeBytes", d->sink && d->audioHadData &&
                                      (d->sink->state() == QAudio::ActiveState ||
                                       d->sink->state() == QAudio::IdleState)
                                  ? d->sink->bytesFree()
                                  : qint64(0)},
            {"error", d->sink ? static_cast<int>(d->sink->error()) : 0},
            {"state", d->sink ? static_cast<int>(d->sink->state()) : -1},
            {"bufferedBytes", d->pcm.bytesAvailable()},
            {"pendingChunks", d->pendingAudio},
            {"pendingWorkerJobs", pendingWorkerJobs},
            {"audioEpoch", qint64(d->audioEpoch.load())},
            {"audioPriming", d->audioClock && !d->audioHadData},
            {"audioTimeStretching", d->timeStretching},
            {"stretchInputFrames", d->stretchInputFrames},
            {"stretchOutputFrames", d->stretchOutputFrames},
            {"stretchBufferedFrames", d->stretchBufferedFrames},
            {"loopDelayActive", d->delayActive},
            {"loopEmptyFrame", d->delayActive && d->project.loopHoldEmpty},
            {"currentFrame", d->frame},
            {"renderedFrame", d->renderedFrame},
            {"rate", d->rate}};
}
void ReviewSession::setPlaybackFrameStep(int step) {
    auto bounded = std::clamp(step, 1, 1000000);
    if (d->frameStep == bounded)
        return;
    d->frameStep = bounded;
    emit transportSettingsChanged();
}
int ReviewSession::playbackFrameStep() const {
    return d->frameStep;
}
void ReviewSession::setBookmarkPlayback(bool enabled) {
    if (enabled && d->bookmarks().isEmpty()) {
        if (d->playing)
            pause();
        d->bookmarkMode = false;
        emit transportSettingsChanged();
        emit error("No bookmarks in the playback range");
        return;
    }
    if (d->bookmarkMode == enabled)
        return;
    d->bookmarkMode = enabled;
    if (enabled)
        seekInternal(d->frame, false, false);
    emit transportSettingsChanged();
}
bool ReviewSession::bookmarkPlayback() const {
    return d->bookmarkMode;
}
void ReviewSession::setReverseAudioEnabled(bool enabled) {
    if (d->reverseAudio == enabled)
        return;
    d->reverseAudio = enabled;
    if (d->playing && d->rate < 0)
        d->startTransport();
    emit transportSettingsChanged();
}
bool ReviewSession::reverseAudioEnabled() const {
    return d->reverseAudio;
}
void ReviewSession::setProjectUndoEnabled(bool enabled) {
    if (d->projectUndo == enabled)
        return;
    d->untrackedDirty |= !d->undo.isClean();
    d->undo.clear();
    d->projectUndo = enabled;
    emit transportSettingsChanged();
    emit projectChanged();
}
bool ReviewSession::projectUndoEnabled() const {
    return d->projectUndo;
}
void ReviewSession::setProjectDefaults(Rational fps, qint64 offset, bool addTimeline,
                                       bool activateTimeline, const QString &root) {
    if (!fps.valid() || fps.num > 1000000000 || fps.den > 1000000000 || fps.value() > 1000) {
        emit error("Invalid default sequence frame rate");
        return;
    }
    QString safeRoot;
    if (!root.isEmpty()) {
        QString message;
        safeRoot = LocalPathPolicy::validateDirectory(root, &message);
        if (safeRoot.isEmpty()) {
            emit error(message);
            return;
        }
    }
    d->sequenceFps = fps;
    d->defaultOffset = std::clamp<qint64>(offset, -1000000000000LL, 1000000000000LL);
    d->defaultAddTimeline = addTimeline;
    d->defaultActivateTimeline = activateTimeline;
    d->defaultSourceRoot = safeRoot;
}
void ReviewSession::setProjectLoadOptions(bool restorePanZoom, bool restoreHold) {
    d->restorePanZoom = restorePanZoom;
    d->restoreHold = restoreHold;
}
void ReviewSession::setCacheLimitMiB(int mib) {
    d->cacheMiB = std::clamp(mib, 16, 4096);
    d->enqueue([limit = d->cacheMiB](MediaEngine &e) { e.setCacheLimitMiB(limit); });
}
} // namespace openframe
