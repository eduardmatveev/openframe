#include "Exporter.h"
#include "FrameComposer.h"
#include "integration/LocalPathPolicy.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImageWriter>
#include <QSaveFile>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
namespace openframe {
namespace {
void check(int r, const char *operation) {
    if (r < 0) {
        char message[256];
        av_strerror(r, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}
void cancel(const CancelCheck &c) {
    if (c && c())
        throw std::runtime_error("Cancelled");
}
void publishSequence(const QString &stage, const QString &destination, bool overwrite,
                     const CancelCheck &cancelled) {
    const auto names = QDir(stage).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    QStringList collisions;
    QString error;
    for (const auto &name : names) {
        const auto target = QDir(destination).filePath(name);
        if (LocalPathPolicy::validateOutput(target, &error).isEmpty())
            throw std::runtime_error(error.toStdString());
        if (QFileInfo::exists(target)) {
            if (!overwrite)
                throw std::runtime_error(
                    ("Export will not overwrite an existing sequence file: " + name).toStdString());
            collisions.append(name);
        }
    }

    // The backup is separate from staging: a failed rollback must never erase
    // the sole remaining copy of a user's original output.
    QString backup;
    if (!collisions.isEmpty()) {
        backup = QDir(destination).filePath(".openframe-backup-" + newId());
        if (!QDir().mkpath(backup))
            throw std::runtime_error("Cannot create overwrite backup directory");
    }
    QStringList backedUp;
    QStringList published;
    try {
        for (const auto &name : collisions) {
            cancel(cancelled);
            const auto target = QDir(destination).filePath(name);
            if (LocalPathPolicy::validateOutput(target, &error).isEmpty())
                throw std::runtime_error(error.toStdString());
            if (!QFile::rename(target, QDir(backup).filePath(name)))
                throw std::runtime_error("Cannot back up existing sequence file");
            backedUp.append(name);
        }
        for (const auto &name : names) {
            cancel(cancelled);
            const auto target = QDir(destination).filePath(name);
            if (LocalPathPolicy::validateOutput(target, &error).isEmpty())
                throw std::runtime_error(error.toStdString());
            // QFile::rename refuses overwrite, protecting a newly introduced
            // file even if it appeared after the preflight checks.
            if (!QFile::rename(QDir(stage).filePath(name), target))
                throw std::runtime_error("Cannot publish completed sequence file");
            published.append(name);
        }
        cancel(cancelled);
    } catch (...) {
        bool restored = true;
        for (auto it = published.crbegin(); it != published.crend(); ++it)
            restored &= QFile::rename(QDir(destination).filePath(*it), QDir(stage).filePath(*it));
        for (const auto &name : backedUp)
            restored &=
                QFile::rename(QDir(backup).filePath(name), QDir(destination).filePath(name));
        if (!restored)
            throw std::runtime_error(
                ("Output rollback could not finish. Original backups retained in: " + backup)
                    .toStdString());
        if (!backup.isEmpty())
            QDir().rmdir(backup);
        throw;
    }
    if (!backup.isEmpty() && !QDir(backup).removeRecursively())
        throw std::runtime_error(
            ("Export completed, but old output backups remain in: " + backup).toStdString());
    QDir().rmdir(stage);
}
QVector<float> selectedAudio(MediaEngine &engine, const Project &project, const QString &timelineId,
                             const QVector<qint64> &frames, AVRational frameTimeBase,
                             qint64 outputStart, qint64 count, const MediaSource *sourceOwner) {
    QVector<float> result(count * 2, 0.0f);
    qint64 done = 0;
    while (done < count) {
        auto position = outputStart + done;
        auto index = av_rescale_q_rnd(position, {1, 48000}, frameTimeBase, AV_ROUND_DOWN);
        index = std::clamp<qint64>(index, 0, frames.size() - 1);
        auto frameStart = av_rescale_q(index, frameTimeBase, {1, 48000});
        qint64 runEnd = index + 1;
        while (!sourceOwner && runEnd < frames.size() && frames[runEnd] == frames[runEnd - 1] + 1)
            ++runEnd;
        auto endSample = av_rescale_q(runEnd, frameTimeBase, {1, 48000});
        auto amount = std::min(count - done, std::max<qint64>(1, endSample - position));
        const auto sourceStartUs = project.timeline(timelineId)->fps.frameToUs(frames[index]) +
                                   av_rescale(position - frameStart, 1000000, 48000);
        auto data =
            sourceOwner
                ? engine.sourceAudio(
                      *sourceOwner,
                      sourceAudioTime(*sourceOwner,
                                      sourceOwner->info.frameStartUs(frames[index]) +
                                          av_rescale(position - frameStart, 1000000, 48000)),
                      av_rescale(amount, 1000000, 48000))
                : engine.timelineAudio(project, timelineId, sourceStartUs,
                                       av_rescale(amount, 1000000, 48000));
        auto copy = std::min<qint64>(amount, data.size() / 2);
        if (copy > 0)
            std::memcpy(result.data() + done * 2, data.constData(),
                        static_cast<size_t>(copy * 2) * sizeof(float));
        done += amount;
    }
    return result;
}
struct ColorPlan {
    VideoColorInfo color;
    int bits=8;
    AVPixelFormat format=AV_PIX_FMT_NONE;
    bool preserve=false;
};
QString exportOwner(const Project &p,const ExportSettings &s) {
    return s.timelineId.isEmpty() ? viewerPaneOwner(p, primaryViewerPane(p.viewer)) : s.timelineId;
}
ColorPlan colorPlan(const Project &p,const ExportSettings &s) {
    ColorPlan plan;
    if(s.kind==ExportKind::Audio)return plan;
    if(s.bitDepth!=0&&s.bitDepth!=8&&s.bitDepth!=10&&s.bitDepth!=12&&s.bitDepth!=16)
        throw std::runtime_error("Output bit depth must be auto, 8, 10, 12 or 16");
    plan.preserve=s.colorMode==ExportColorMode::PreserveSource;
    if(s.colorMode!=ExportColorMode::PreserveSource&&s.colorMode!=ExportColorMode::DisplayConverted)
        throw std::runtime_error("Invalid export color mode");
    if(plan.preserve&&s.viewerComposition)
        throw std::runtime_error("Preserve-source color requires Timeline Sources export");
    if(plan.preserve) {
        bool found=false;
        auto accept=[&](const VideoColorInfo &color) {
            if(color.componentBits<=0||color.pixelFormat.isEmpty())
                throw std::runtime_error("Source color metadata is unavailable; relink or reimport the source");
            if(!found){plan.color=color;found=true;}
            else if(!equivalentColorEncoding(plan.color,color)||plan.color.pixelFormat!=color.pixelFormat)
                throw std::runtime_error("Preserve-source export requires a homogeneous color encoding and pixel format; use display-converted for mixed sources");
        };
        auto sourceRange=[&](const MediaSource &source,qint64 first,qint64 last) {
            first=std::clamp<qint64>(first,0,std::max<qint64>(0,source.info.frameCount()-1));
            last=last<0?source.info.frameCount()-1:std::min(last,source.info.frameCount()-1);
            if(last<first)return;
            accept(frameColorInfo(source.info,first));
            for(auto it=source.info.frameColors.upperBound(first);it!=source.info.frameColors.cend()&&it.key()<=last;++it)accept(it.value());
        };
        const auto owner=exportOwner(p,s);
        if(auto source=p.source(owner))sourceRange(*source,s.firstFrame,s.lastFrame);
        else if(auto timeline=p.timeline(owner)) {
            qint64 start=0;auto last=s.lastFrame<0?p.durationFrames(owner)-1:s.lastFrame;
            for(const auto &clip:timeline->clips) {
                const auto count=clipDurationFrames(p,*timeline,clip);
                const auto from=std::max(start,s.firstFrame),to=std::min(start+count-1,last);
                if(from<=to) {
                    auto a=evaluateTimeline(p,owner,from),b=evaluateTimeline(p,owner,to);
                    if(a&&b)if(auto source=p.source(clip.sourceId))sourceRange(*source,a->sourceFrame,b->sourceFrame);
                }
                start+=count;
            }
        }
        if(!found)throw std::runtime_error("No source color data in export range");
    }else plan.color=sdrColorInfo();
    plan.bits=s.bitDepth?s.bitDepth:plan.preserve?plan.color.componentBits:8;
    if(s.kind==ExportKind::ImageSequence) {
        const auto fmt=s.imageFormat.toLower().toUtf8();
        if(!QImageWriter::supportedImageFormats().contains(fmt))throw std::runtime_error("Image writer is unavailable in this build");
        if(s.bitDepth==0&&plan.bits>8)plan.bits=16;
        if(plan.bits!=8&&plan.bits!=16)throw std::runtime_error("Image sequences support explicit 8 or 16 bit depth");
        if(plan.bits==16&&fmt!="png")throw std::runtime_error("16-bit image export requires PNG");
        if(plan.preserve&&plan.color.range=="tv")throw std::runtime_error("PNG cannot preserve limited-range RGB/YUV coding; use FFV1 or display-converted");
        return plan;
    }
    const auto codec=avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData());
    if(!codec)throw std::runtime_error("Requested video encoder is unavailable");
    if(plan.preserve&&(codec->id!=AV_CODEC_ID_FFV1||QFileInfo(s.path).suffix().compare("mkv",Qt::CaseInsensitive)))
        throw std::runtime_error("Preserve-source video requires FFV1 in MKV");
    if(isHdrColor(plan.color)&&plan.bits<10)throw std::runtime_error("HDR preserve-source output requires at least 10 bits");
    auto supported=[&](AVPixelFormat fmt){if(!codec->pix_fmts)return fmt==AV_PIX_FMT_YUV420P;for(auto q=codec->pix_fmts;*q!=AV_PIX_FMT_NONE;++q)if(*q==fmt)return true;return false;};
    const auto original=av_get_pix_fmt(plan.color.pixelFormat.toUtf8().constData());
    if(plan.preserve&&plan.bits==plan.color.componentBits&&supported(original))plan.format=original;
    else if(codec->id==AV_CODEC_ID_FFV1) {
        auto alpha=av_pix_fmt_desc_get(original);
        const bool hasAlpha=alpha&&(alpha->flags&AV_PIX_FMT_FLAG_ALPHA);
        const auto name=plan.bits==8?QByteArray(hasAlpha?"bgra":"bgr0"):
            QByteArray(hasAlpha?"gbrap":"gbrp")+QByteArray::number(plan.bits)+"le";
        auto fmt=av_get_pix_fmt(name.constData());if(supported(fmt))plan.format=fmt;
    }else if(plan.bits==8) {
        for(auto fmt:{AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_YUVJ420P,AV_PIX_FMT_BGRA})if(supported(fmt)){plan.format=fmt;break;}
    }
    if(plan.format==AV_PIX_FMT_NONE)throw std::runtime_error("Selected encoder does not support the requested bit depth/color mode");
    const auto desc=av_pix_fmt_desc_get(plan.format);
    if((s.size.width()%(1<<desc->log2_chroma_w))||(s.size.height()%(1<<desc->log2_chroma_h)))
        throw std::runtime_error("Output dimensions are incompatible with encoder chroma subsampling");
    if(!plan.preserve) {
        plan.color=sdrColorInfo(plan.bits);
        plan.color.matrix=(desc->flags&AV_PIX_FMT_FLAG_RGB)?"gbr":"bt709";
        plan.color.range=(desc->flags&AV_PIX_FMT_FLAG_RGB)||plan.format==AV_PIX_FMT_YUVJ420P?"pc":"tv";
    }
    if(plan.preserve&&(desc->flags&AV_PIX_FMT_FLAG_RGB)) {
        const auto originalDescriptor=av_pix_fmt_desc_get(original);
        if(originalDescriptor&&!(originalDescriptor->flags&AV_PIX_FMT_FLAG_RGB)) {
            plan.color.matrix="gbr";plan.color.range="pc";
        }
    }
    plan.color.pixelFormat=QString::fromLatin1(av_get_pix_fmt_name(plan.format));
    plan.color.componentBits=plan.bits;
    return plan;
}
void setVideoColor(AVCodecContext *codec,const VideoColorInfo &c) {
    codec->color_primaries=AVColorPrimaries(av_color_primaries_from_name(c.primaries.toUtf8().constData()));
    codec->color_trc=AVColorTransferCharacteristic(av_color_transfer_from_name(c.transfer.toUtf8().constData()));
    codec->colorspace=AVColorSpace(av_color_space_from_name(c.matrix.toUtf8().constData()));
    codec->color_range=AVColorRange(av_color_range_from_name(c.range.toUtf8().constData()));
    codec->chroma_sample_location=AVChromaLocation(av_chroma_location_from_name(c.chromaLocation.toUtf8().constData()));
    if(codec->color_primaries<0)codec->color_primaries=AVCOL_PRI_UNSPECIFIED;
    if(codec->color_trc<0)codec->color_trc=AVCOL_TRC_UNSPECIFIED;
    if(codec->colorspace<0)codec->colorspace=AVCOL_SPC_UNSPECIFIED;
    if(codec->color_range<0)codec->color_range=AVCOL_RANGE_UNSPECIFIED;
    if(codec->chroma_sample_location<0)codec->chroma_sample_location=AVCHROMA_LOC_UNSPECIFIED;
    if(c.masteringMaxNits>0||c.masteringChromaticities.size()==8) {
        auto side=av_packet_side_data_new(&codec->coded_side_data,&codec->nb_coded_side_data,AV_PKT_DATA_MASTERING_DISPLAY_METADATA,sizeof(AVMasteringDisplayMetadata),0);
        if(!side)throw std::bad_alloc();
        auto m=reinterpret_cast<AVMasteringDisplayMetadata *>(side->data);std::memset(m,0,sizeof(*m));
        if(c.masteringMaxNits>0){m->has_luminance=1;m->min_luminance=av_d2q(c.masteringMinNits,1000000);m->max_luminance=av_d2q(c.masteringMaxNits,1000000);}
        if(c.masteringChromaticities.size()==8){m->has_primaries=1;for(int i=0;i<6;++i)m->display_primaries[i/2][i%2]=av_d2q(c.masteringChromaticities[i],1000000);for(int i=0;i<2;++i)m->white_point[i]=av_d2q(c.masteringChromaticities[6+i],1000000);}
    }
    if(c.maxCLL>0||c.maxFALL>0) {
        auto side=av_packet_side_data_new(&codec->coded_side_data,&codec->nb_coded_side_data,AV_PKT_DATA_CONTENT_LIGHT_LEVEL,sizeof(AVContentLightMetadata),0);
        if(!side)throw std::bad_alloc();auto m=reinterpret_cast<AVContentLightMetadata *>(side->data);m->MaxCLL=unsigned(c.maxCLL);m->MaxFALL=unsigned(c.maxFALL);
    }
}
bool hasAnnotations(const Project &p,const QString &owner) {
    if(p.viewer.annotationBackground.alpha()>0)return true;
    return std::any_of(p.annotations.cbegin(),p.annotations.cend(),[&](const Annotation &a){return a.visible&&(a.timelineId.isEmpty()||a.timelineId==owner);});
}
void packRgb(const QImage &input,AVFrame *out,const VideoColorInfo &color) {
    auto image=input.convertToFormat(QImage::Format_RGBA32FPx4);
    const auto desc=av_pix_fmt_desc_get(AVPixelFormat(out->format));
    for(int y=0;y<image.height();++y){const auto src=reinterpret_cast<const float *>(image.constScanLine(y));
        for(int x=0;x<image.width();++x)for(int n=0;n<desc->nb_components;++n){
            const auto &d=desc->comp[n];const auto maximum=(quint64(1)<<d.depth)-1;
            auto v=std::isfinite(src[x*4+n])?double(src[x*4+n]):0;
            auto value=color.range=="tv"&&n<3?(16+219*v)*std::ldexp(1.0,d.depth-8):v*maximum;
            quint64 code=quint64(std::llround(std::clamp(value,0.0,double(maximum))))<<d.shift;
            auto dst=out->data[d.plane]+qint64(y)*out->linesize[d.plane]+qint64(x)*d.step+d.offset;
            auto bytes=(d.depth+d.shift+7)/8;for(int b=0;b<bytes;++b)dst[b]=uchar(code>>(8*((desc->flags&AV_PIX_FMT_FLAG_BE)?bytes-b-1:b)));
        }
    }
}
struct Output {
    QSaveFile file;
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    AVCodecContext *video = nullptr;
    AVCodecContext *audio = nullptr;
    AVStream *videoStream = nullptr;
    AVStream *audioStream = nullptr;
    AVFrame *videoFrame = nullptr;
    AVFrame *audioFrame = nullptr;
    SwsContext *scaler = nullptr;
    SwrContext *resampler = nullptr;
    bool committed = false;
    static int write(void *opaque, const uint8_t *bytes, int count) {
        auto &o = *static_cast<Output *>(opaque);
        auto n = o.file.write(reinterpret_cast<const char *>(bytes), count);
        return n == count ? count : AVERROR(EIO);
    }
    static int64_t seek(void *opaque, int64_t offset, int whence) {
        auto &o = *static_cast<Output *>(opaque);
        if (whence == AVSEEK_SIZE)
            return o.file.size();
        whence &= ~AVSEEK_FORCE;
        auto pos = whence == SEEK_SET   ? offset
                   : whence == SEEK_CUR ? o.file.pos() + offset
                   : whence == SEEK_END ? o.file.size() + offset
                                        : -1;
        return pos >= 0 && o.file.seek(pos) ? o.file.pos() : AVERROR(EIO);
    }
    static int rejectNested(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) {
        return AVERROR(EACCES);
    }
    ~Output() {
        sws_freeContext(scaler);
        swr_free(&resampler);
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (format)
            avformat_free_context(format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (!committed)
            file.cancelWriting();
    }
    void packets(AVCodecContext *codec, AVStream *stream) {
        AVPacket *packet = av_packet_alloc();
        if (!packet)
            throw std::bad_alloc();
        try {
            for (;;) {
                int r = avcodec_receive_packet(codec, packet);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                    break;
                check(r, "Receive encoded packet");
                if (codec->codec_type == AVMEDIA_TYPE_VIDEO && packet->duration <= 0)
                    packet->duration = 1;
                av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
                packet->stream_index = stream->index;
                check(av_interleaved_write_frame(format, packet), "Write output packet");
                av_packet_unref(packet);
            }
        } catch (...) {
            av_packet_free(&packet);
            throw;
        }
        av_packet_free(&packet);
    }
    void send(AVCodecContext *codec, AVStream *stream, AVFrame *frame) {
        auto r = avcodec_send_frame(codec, frame);
        if (r == AVERROR(EAGAIN)) {
            packets(codec, stream);
            r = avcodec_send_frame(codec, frame);
        }
        check(r, "Encode frame");
        packets(codec, stream);
    }
};
} // namespace
bool validateExportSettings(const Project &project,const ExportSettings &settings,QString *error) {
    try {
        if(settings.size.width()<16||settings.size.height()<16||settings.size.width()>8192||settings.size.height()>8192)
            throw std::runtime_error("Export dimensions must be between 16 and 8192");
        colorPlan(project,settings);
        return true;
    }catch(const std::exception &e){if(error)*error=QString::fromUtf8(e.what());return false;}
}
QString exportProject(const Project &project, const ExportSettings &requested,
                      const CancelCheck &cancelled, const ProgressCallback &progress) {
    auto settings = requested;
    const auto color=colorPlan(project,settings);
    Project snapshot = project;
    auto timelineId =
        settings.timelineId.isEmpty() ? snapshot.activeTimelineId : settings.timelineId;
    QString sourceOwnerId;
    const auto primary = exportOwner(snapshot, settings);
    if (!primary.isEmpty()) {
        if (snapshot.timeline(primary))
            timelineId = primary;
        else if (auto source = snapshot.source(primary)) {
            sourceOwnerId = primary;
            Timeline sourceTimeline;
            sourceTimeline.id = newId();
            sourceTimeline.name = source->name;
            sourceTimeline.fps = source->info.fps;
            Clip clip;
            clip.id = newId();
            clip.sourceId = source->id;
            clip.inFrame = source->inFrame;
            clip.outFrame = source->outFrame;
            sourceTimeline.clips.append(clip);
            timelineId = sourceTimeline.id;
            snapshot.timelines.append(sourceTimeline);
        }
    }
    auto t = snapshot.timeline(timelineId);
    if (!t)
        throw std::runtime_error("Export timeline does not exist");
    const bool automaticRate = settings.outputFps.num == 0 && settings.outputFps.den == 0;
    if (!automaticRate && (!settings.outputFps.valid() || settings.outputFps.num > 1000000 ||
                           settings.outputFps.den > 1000000 || settings.outputFps.value() < .001 ||
                           settings.outputFps.value() > 1000))
        throw std::runtime_error("Output FPS must be a rational rate between 0.001 and 1000");
    const auto outputFps = automaticRate ? t->fps : settings.outputFps;
    const auto sourceOwner = snapshot.source(sourceOwnerId);
    const auto annotationOwner = sourceOwner ? sourceOwnerId : timelineId;
    auto duration =
        sourceOwner ? sourceOwner->info.frameCount() : snapshot.durationFrames(timelineId);
    settings.firstFrame = std::max<qint64>(0, settings.firstFrame);
    settings.lastFrame =
        settings.lastFrame < 0 ? duration - 1 : std::min(settings.lastFrame, duration - 1);
    if (settings.lastFrame < settings.firstFrame)
        throw std::runtime_error("Export range is empty");
    if (settings.size.width() < 16 || settings.size.height() < 16 || settings.size.width() > 8192 ||
        settings.size.height() > 8192)
        throw std::runtime_error("Export dimensions must be between 16 and 8192");
    QString error;
    auto destination =
        settings.kind == ExportKind::ImageSequence && settings.publishSequenceIntoExistingDirectory
            ? LocalPathPolicy::validateDirectory(settings.path, &error)
            : LocalPathPolicy::validateOutput(settings.path, &error);
    if (destination.isEmpty())
        throw std::runtime_error(error.toStdString());
    cancel(cancelled);
    if (settings.frameStep < 1 || settings.frameStep > 1000000)
        throw std::runtime_error("Invalid export frame step");
    if (settings.bookmarksOnly && settings.kind != ExportKind::ImageSequence)
        throw std::runtime_error("Bookmarks-only export requires an image sequence");
    QVector<qint64> selectedFrames;
    if (settings.bookmarksOnly) {
        for (const auto &b : snapshot.bookmarks)
            if (b.timelineId == annotationOwner && b.frame >= settings.firstFrame &&
                b.frame <= settings.lastFrame)
                selectedFrames.append(b.frame);
        std::sort(selectedFrames.begin(), selectedFrames.end());
        selectedFrames.erase(std::unique(selectedFrames.begin(), selectedFrames.end()),
                             selectedFrames.end());
    } else
        for (auto f = settings.firstFrame; f <= settings.lastFrame; f += settings.frameStep)
            selectedFrames.append(f);
    if (selectedFrames.isEmpty())
        throw std::runtime_error("No frames match the export selection");
    qint64 count = selectedFrames.size();
    MediaEngine engine(256, cancelled);
    snapshot.activeTimelineId = timelineId;
    snapshot.viewer.outputSize = settings.size;
    if (settings.overrideAnnotationBackground)
        snapshot.viewer.annotationBackground = settings.annotationBackground;
    if (settings.kind == ExportKind::ImageSequence) {
        if (settings.sequencePadding < 1 || settings.sequencePadding > 18 ||
            settings.sequenceBaseName.isEmpty() || settings.sequenceBaseName.contains('/') ||
            settings.sequenceBaseName.contains('\\') || settings.sequenceBaseName.contains(':') ||
            settings.sequenceBaseName.contains(QChar(0)))
            throw std::runtime_error("Invalid image sequence naming");
        if (settings.frameNumbering != "one_based" && settings.frameNumbering != "zero_based" &&
            settings.frameNumbering != "match_frame" && settings.frameNumbering != "custom")
            throw std::runtime_error("Invalid frame numbering mode");
        auto format = settings.imageFormat.toLower();
        if (format != "png" && format != "jpg" && format != "jpeg" && format != "bmp" &&
            format != "tiff")
            throw std::runtime_error("Unsupported image sequence format");
        if (settings.publishSequenceIntoExistingDirectory) {
            if (LocalPathPolicy::validateDirectory(destination, &error).isEmpty())
                throw std::runtime_error(error.toStdString());
        } else if (QFileInfo::exists(destination))
            throw std::runtime_error("Image sequence destination must be a new directory");
        auto stage = settings.publishSequenceIntoExistingDirectory
                         ? QDir(destination).filePath(".openframe-partial-" + newId())
                         : destination + ".partial-" + newId();
        if (!QDir().mkpath(stage))
            throw std::runtime_error("Cannot create image sequence staging directory");
        QString firstPublishedImage;
        try {
            for (qint64 i = 0; i < count; ++i) {
                cancel(cancelled);
                auto image =
                    settings.viewerComposition
                        ? FrameComposer::composeHighPrecision(snapshot,selectedFrames[i],engine,settings.size,settings.includeAnnotations)
                        : FrameComposer::timelineHighPrecision(snapshot,sourceOwner?sourceOwnerId:timelineId,selectedFrames[i],engine,settings.size,settings.includeAnnotations,color.preserve?&color.color:nullptr);
                image=ColorPipeline::quantize(image,color.bits);
                qint64 number = settings.frameNumbering == "zero_based" ? i
                                : settings.frameNumbering == "custom"   ? settings.sequenceStart + i
                                                                        : i + 1;
                if (settings.frameNumbering == "match_frame") {
                    number = displayFrameNumber(snapshot, annotationOwner, selectedFrames[i]);
                }
                const auto name = QString("%1_%2.%3")
                                      .arg(settings.sequenceBaseName)
                                      .arg(number, settings.sequencePadding, 10, QChar('0'))
                                      .arg(format);
                QImageWriter writer(QDir(stage).filePath(name), format.toUtf8());
                if (!writer.write(image))
                    throw std::runtime_error(writer.errorString().toStdString());
                if (i == 0)
                    firstPublishedImage = QDir(destination).filePath(name);
                if (progress)
                    progress(static_cast<int>((i + 1) * 95 / count));
            }
            if (settings.includeAudio && settings.frameStep == 1 && !settings.bookmarksOnly) {
                auto audioSettings = settings;
                audioSettings.kind = ExportKind::Audio;
                audioSettings.path = QDir(stage).filePath("audio.wav");
                audioSettings.audioCodec = "pcm_s16le";
                exportProject(snapshot, audioSettings, cancelled, {});
            }
            cancel(cancelled);
            if (settings.publishSequenceIntoExistingDirectory) {
                publishSequence(stage, destination, settings.forceOverwrite, cancelled);
            } else if (!QDir().rename(stage, destination))
                throw std::runtime_error("Cannot publish completed image sequence");
            if (progress)
                progress(100);
        } catch (...) {
            QDir(stage).removeRecursively();
            throw;
        }
        return firstPublishedImage;
    }
    Output out;
    auto utf8 = destination.toUtf8();
    check(avformat_alloc_output_context2(&out.format, nullptr, nullptr, utf8.constData()),
          "Select output container");
    if (!out.format)
        throw std::runtime_error("Unsupported output extension");
    if (out.format->oformat->flags & AVFMT_NOFILE)
        throw std::runtime_error("Export container must be a single local file");
    auto formatName = QString::fromUtf8(out.format->oformat->name);
    if (formatName != "mp4" && formatName != "mov" && formatName != "matroska" &&
        formatName != "avi" && formatName != "wav" && formatName != "flac" && formatName != "mp3" &&
        formatName != "ogg" && formatName != "webm" && formatName != "ipod")
        throw std::runtime_error("Unsupported or nonlocal output container");
    const AVRational videoTimeBase{static_cast<int>(outputFps.den),
                                   static_cast<int>(outputFps.num)};
    const AVRational ownerTimeBase{static_cast<int>(t->fps.den), static_cast<int>(t->fps.num)};
    const bool continuous = settings.frameStep == 1 && !settings.bookmarksOnly;
    const bool variableSource = sourceOwner && sourceOwner->info.variableFrameRate;
    const auto firstUs = variableSource ? sourceOwner->info.frameStartUs(settings.firstFrame)
                                        : t->fps.frameToUs(settings.firstFrame);
    const auto spanUs = variableSource
                            ? sourceOwner->info.frameEndUs(settings.lastFrame) - firstUs
                            : t->fps.frameToUs(settings.lastFrame - settings.firstFrame + 1);
    qint64 totalSamples;
    if (continuous) {
        const auto inputFrames = settings.lastFrame - settings.firstFrame + 1;
        count = variableSource
                    ? av_rescale_q_rnd(spanUs, {1, 1000000}, videoTimeBase, AV_ROUND_UP)
                    : av_rescale_q_rnd(inputFrames, ownerTimeBase, videoTimeBase, AV_ROUND_UP);
        if (count <= 0 || count > 4000000)
            throw std::runtime_error("Resampled export exceeds the 4 million frame limit");
        selectedFrames.clear();
        selectedFrames.reserve(count);
        // Ordinals identify source images/annotations, but elapsed time comes from PTS.
        // CFR output repeats or skips images on its explicit sampling grid without
        // shortening the source interval or slicing its continuous audio into frame chunks.
        for (qint64 i = 0; i < count; ++i) {
            auto inputFrame =
                variableSource
                    ? sourceOwner->info.frameAtSamplingUs(firstUs + outputFps.frameToUs(i))
                    : settings.firstFrame +
                          av_rescale_q_rnd(i, videoTimeBase, ownerTimeBase, AV_ROUND_DOWN);
            selectedFrames.append(std::clamp(inputFrame, settings.firstFrame, settings.lastFrame));
        }
        totalSamples = variableSource ? av_rescale(spanUs, 48000, 1000000)
                                      : av_rescale_q(inputFrames, ownerTimeBase, {1, 48000});
    } else
        totalSamples = av_rescale_q(count, videoTimeBase, {1, 48000});
    if (settings.kind == ExportKind::Video) {
        auto name = settings.videoCodec.toUtf8();
        auto codec = avcodec_find_encoder_by_name(name.constData());
        if (!codec)
            throw std::runtime_error(
                ("Video encoder is unavailable: " + settings.videoCodec).toStdString());
        out.video = avcodec_alloc_context3(codec);
        if (!out.video)
            throw std::bad_alloc();
        out.video->width = settings.size.width();
        out.video->height = settings.size.height();
        out.video->time_base = videoTimeBase;
        out.video->framerate = {videoTimeBase.den, videoTimeBase.num};
        out.video->pix_fmt=color.format;
        out.video->bit_rate = std::clamp<qint64>(static_cast<qint64>(settings.size.width()) *
                                                     settings.size.height() * 8,
                                                 1000000, 80000000);
        out.video->gop_size = 24;
        out.video->max_b_frames = codec->id == AV_CODEC_ID_MPEG4 ? 2 : 0;
        setVideoColor(out.video,color.color);
        if (out.format->oformat->flags & AVFMT_GLOBALHEADER)
            out.video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *options = nullptr;
        av_dict_set(&options, "crf", QByteArray::number(settings.crf).constData(), 0);
        av_dict_set(&options, "preset", "medium", 0);
        if(codec->id==AV_CODEC_ID_FFV1){av_dict_set(&options,"level","3",0);av_dict_set(&options,"coder","1",0);}
        auto r = avcodec_open2(out.video, codec, &options);
        av_dict_free(&options);
        check(r, "Open video encoder");
        out.videoStream = avformat_new_stream(out.format, nullptr);
        if (!out.videoStream)
            throw std::bad_alloc();
        out.videoStream->time_base = out.video->time_base;
        out.videoStream->avg_frame_rate = out.video->framerate;
        out.videoStream->r_frame_rate = out.video->framerate;
        check(avcodec_parameters_from_context(out.videoStream->codecpar, out.video),
              "Copy output video parameters");
        out.videoFrame = av_frame_alloc();
        out.videoFrame->format = out.video->pix_fmt;
        out.videoFrame->width = out.video->width;
        out.videoFrame->height = out.video->height;
        check(av_frame_get_buffer(out.videoFrame, 32), "Allocate output video");
        out.scaler = sws_getContext(settings.size.width(), settings.size.height(), AV_PIX_FMT_RGBA64LE,
                                    settings.size.width(), settings.size.height(),
                                    out.video->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!out.scaler)
            throw std::runtime_error("Cannot create output color conversion");
        const auto matrix=color.color.matrix=="bt2020nc"?SWS_CS_BT2020:color.color.matrix=="bt470bg"||color.color.matrix=="smpte170m"?SWS_CS_ITU601:SWS_CS_ITU709;
        sws_setColorspaceDetails(out.scaler,sws_getCoefficients(matrix),1,sws_getCoefficients(matrix),out.video->color_range==AVCOL_RANGE_JPEG,0,1<<16,1<<16);
    }
    if (settings.includeAudio || settings.kind == ExportKind::Audio) {
        auto name = settings.audioCodec.toUtf8();
        if (settings.kind == ExportKind::Audio &&
            QFileInfo(destination).suffix().compare("wav", Qt::CaseInsensitive) == 0)
            name = "pcm_s16le";
        auto codec = avcodec_find_encoder_by_name(name.constData());
        if (!codec)
            throw std::runtime_error(
                ("Audio encoder is unavailable: " + QString::fromUtf8(name)).toStdString());
        out.audio = avcodec_alloc_context3(codec);
        if (!out.audio)
            throw std::bad_alloc();
        out.audio->sample_rate = 48000;
        if (codec->supported_samplerates) {
            bool supported = false;
            for (auto r = codec->supported_samplerates; *r; ++r)
                if (*r == 48000)
                    supported = true;
            if (!supported)
                throw std::runtime_error("Audio encoder does not support 48 kHz");
        }
        out.audio->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        out.audio->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        out.audio->time_base = {1, 48000};
        out.audio->bit_rate = 192000;
        if (out.format->oformat->flags & AVFMT_GLOBALHEADER)
            out.audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(out.audio, codec, nullptr), "Open audio encoder");
        out.audioStream = avformat_new_stream(out.format, nullptr);
        if (!out.audioStream)
            throw std::bad_alloc();
        out.audioStream->time_base = out.audio->time_base;
        check(avcodec_parameters_from_context(out.audioStream->codecpar, out.audio),
              "Copy output audio parameters");
        out.audioFrame = av_frame_alloc();
        out.audioFrame->format = out.audio->sample_fmt;
        out.audioFrame->sample_rate = 48000;
        av_channel_layout_copy(&out.audioFrame->ch_layout, &out.audio->ch_layout);
        out.audioFrame->nb_samples = out.audio->frame_size > 0 ? out.audio->frame_size : 1024;
        check(av_frame_get_buffer(out.audioFrame, 0), "Allocate output audio");
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&out.resampler, &out.audio->ch_layout, out.audio->sample_fmt,
                                  48000, &stereo, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr),
              "Create output audio resampler");
        check(swr_init(out.resampler), "Initialize output audio resampler");
    }
    out.file.setFileName(destination);
    if (!out.file.open(QIODevice::WriteOnly))
        throw std::runtime_error(out.file.errorString().toStdString());
    auto buffer = static_cast<uint8_t *>(av_malloc(65536));
    if (!buffer)
        throw std::bad_alloc();
    out.io = avio_alloc_context(buffer, 65536, 1, &out, nullptr, Output::write, Output::seek);
    if (!out.io) {
        av_free(buffer);
        throw std::bad_alloc();
    }
    out.format->pb = out.io;
    out.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    out.format->io_open = Output::rejectNested;
    check(avformat_write_header(out.format, nullptr), "Write output header");
    qint64 videoIndex = 0, audioIndex = 0;
    QVector<float> audioChunk;
    qint64 chunkStart = -1;
    while ((out.video && videoIndex < count) || (out.audio && audioIndex < totalSamples)) {
        cancel(cancelled);
        bool nextVideo = out.video && videoIndex < count &&
                         (!out.audio || audioIndex >= totalSamples ||
                          av_compare_ts(videoIndex, videoTimeBase, audioIndex, {1, 48000}) <= 0);
        if (nextVideo) {
            const auto owner=sourceOwner?sourceOwnerId:timelineId;
            const auto e=evaluateAssignment(snapshot,owner,selectedFrames[videoIndex]);
            VideoFrame native;
            if(color.preserve&&e)if(auto source=snapshot.source(e->sourceId))native=engine.nativeFrame(*source,e->sourceFrame);
            const bool exact=color.preserve&&native.native&&native.size()==settings.size&&
                native.native->format==out.video->pix_fmt&&(!settings.includeAnnotations||!hasAnnotations(snapshot,owner));
            if(exact) {
                auto frame=av_frame_clone(native.native.get());if(!frame)throw std::bad_alloc();
                frame->pts=videoIndex;frame->duration=1;frame->time_base=out.video->time_base;frame->pict_type=AV_PICTURE_TYPE_NONE;
                try{out.send(out.video,out.videoStream,frame);}catch(...){av_frame_free(&frame);throw;}
                av_frame_free(&frame);
            }else {
                auto image=settings.viewerComposition?
                    FrameComposer::composeHighPrecision(snapshot,selectedFrames[videoIndex],engine,settings.size,settings.includeAnnotations):
                    FrameComposer::timelineHighPrecision(snapshot,owner,selectedFrames[videoIndex],engine,settings.size,settings.includeAnnotations,color.preserve?&color.color:nullptr);
                check(av_frame_make_writable(out.videoFrame),"Prepare output video");
                if(av_pix_fmt_desc_get(out.video->pix_fmt)->flags&AV_PIX_FMT_FLAG_RGB)packRgb(image,out.videoFrame,color.color);
                else {
                    auto high=ColorPipeline::quantize(image,16);
                    const uint8_t *data[]={high.constBits(),nullptr,nullptr,nullptr};int lines[]={high.bytesPerLine(),0,0,0};
                    sws_scale(out.scaler,data,lines,0,high.height(),out.videoFrame->data,out.videoFrame->linesize);
                }
                out.videoFrame->color_primaries=out.video->color_primaries;out.videoFrame->color_trc=out.video->color_trc;
                out.videoFrame->colorspace=out.video->colorspace;out.videoFrame->color_range=out.video->color_range;
                out.videoFrame->pts=videoIndex;out.videoFrame->duration=1;
                out.send(out.video,out.videoStream,out.videoFrame);
            }
            ++videoIndex;
        } else {
            const int capacity = out.audio->frame_size > 0 ? out.audio->frame_size : 1024;
            const int available =
                static_cast<int>(std::min<qint64>(capacity, totalSamples - audioIndex));
            QVector<float> input(capacity * 2, 0.0f);
            for (int j = 0; j < available;) {
                auto sample = audioIndex + j;
                if (chunkStart < 0 || sample < chunkStart ||
                    sample >= chunkStart + audioChunk.size() / 2) {
                    chunkStart = sample;
                    auto n = std::min<qint64>(48000, totalSamples - chunkStart);
                    if (continuous) {
                        const auto audioUs = firstUs + av_rescale(chunkStart, 1000000, 48000);
                        const auto audioDurationUs = av_rescale(n, 1000000, 48000);
                        audioChunk =
                            sourceOwner ? engine.sourceAudio(*sourceOwner,
                                                             sourceAudioTime(*sourceOwner, audioUs),
                                                             audioDurationUs)
                                        : engine.timelineAudio(snapshot, timelineId, audioUs,
                                                               audioDurationUs);
                    } else
                        audioChunk = selectedAudio(engine, snapshot, timelineId, selectedFrames,
                                                   videoTimeBase, chunkStart, n, sourceOwner);
                    if (audioChunk.isEmpty())
                        audioChunk.resize(n * 2);
                }
                auto offset = sample - chunkStart;
                auto n = std::min<qint64>(available - j, audioChunk.size() / 2 - offset);
                if (n <= 0)
                    break;
                std::memcpy(input.data() + j * 2, audioChunk.constData() + offset * 2,
                            static_cast<size_t>(n * 2) * sizeof(float));
                j += static_cast<int>(n);
            }
            check(av_frame_make_writable(out.audioFrame), "Prepare output audio");
            const uint8_t *data[] = {reinterpret_cast<const uint8_t *>(input.constData())};
            auto actual = out.audio->frame_size > 0 ? capacity : available;
            out.audioFrame->nb_samples = actual;
            check(swr_convert(out.resampler, out.audioFrame->data, actual, data, actual),
                  "Convert output audio");
            out.audioFrame->pts = audioIndex;
            out.send(out.audio, out.audioStream, out.audioFrame);
            audioIndex += available;
        }
        if (progress) {
            auto done = out.video ? videoIndex * 100 / count : audioIndex * 100 / totalSamples;
            progress(static_cast<int>(std::min<qint64>(99, done)));
        }
    }
    cancel(cancelled);
    if (out.video)
        out.send(out.video, out.videoStream, nullptr);
    if (out.audio)
        out.send(out.audio, out.audioStream, nullptr);
    check(av_write_trailer(out.format), "Finalize output container");
    avio_flush(out.io);
    if (out.io->error < 0)
        check(out.io->error, "Flush output file");
    cancel(cancelled);
    if (!out.file.commit())
        throw std::runtime_error(out.file.errorString().toStdString());
    out.committed = true;
    if (progress)
        progress(100);
    return destination;
}
} // namespace openframe
