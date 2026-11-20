#include "MediaDecoder.h"
#include "SequenceReader.h"
#include "integration/LocalPathPolicy.h"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMap>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
namespace openframe {
namespace {
QString avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
void check(int result, const char *context) {
    if (result < 0)
        throw std::runtime_error(
            (QString::fromUtf8(context) + ": " + avError(result)).toStdString());
}
void checkCancel(const CancelCheck &c) {
    if (c && c())
        throw std::runtime_error("Cancelled");
}
struct Input {
    QFile file;
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    CancelCheck cancelled;
    static int read(void *opaque, uint8_t *data, int count) {
        auto &s = *static_cast<Input *>(opaque);
        if (s.cancelled && s.cancelled())
            return AVERROR_EXIT;
        auto n = s.file.read(reinterpret_cast<char *>(data), count);
        return n > 0 ? static_cast<int>(n) : n == 0 ? AVERROR_EOF : AVERROR(EIO);
    }
    static int64_t seek(void *opaque, int64_t offset, int whence) {
        auto &s = *static_cast<Input *>(opaque);
        if (whence == AVSEEK_SIZE)
            return s.file.size();
        whence &= ~AVSEEK_FORCE;
        qint64 pos = whence == SEEK_SET   ? offset
                     : whence == SEEK_CUR ? s.file.pos() + offset
                     : whence == SEEK_END ? s.file.size() + offset
                                          : -1;
        return pos >= 0 && s.file.seek(pos) ? s.file.pos() : AVERROR(EIO);
    }
    static int interrupt(void *opaque) {
        auto &s = *static_cast<Input *>(opaque);
        return s.cancelled && s.cancelled();
    }
    static int rejectNested(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) {
        return AVERROR(EACCES);
    }
    Input(const QString &path, CancelCheck c) : cancelled(std::move(c)) {
        try {
            QString error;
            auto safe = LocalPathPolicy::validateInput(path, &error);
            if (safe.isEmpty())
                throw std::runtime_error(error.toStdString());
            file.setFileName(safe);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error(file.errorString().toStdString());
            auto buffer = static_cast<unsigned char *>(av_malloc(65536));
            if (!buffer)
                throw std::bad_alloc();
            io = avio_alloc_context(buffer, 65536, 0, this, read, nullptr, seek);
            if (!io) {
                av_free(buffer);
                throw std::bad_alloc();
            }
            format = avformat_alloc_context();
            if (!format)
                throw std::bad_alloc();
            format->pb = io;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
            format->io_open = rejectNested;
            format->interrupt_callback = {interrupt, this};
            AVDictionary *options = nullptr;
            av_dict_set(&options, "protocol_whitelist", "file,pipe", 0);
            av_dict_set(&options, "format_whitelist",
                        "mov,matroska,webm,avi,mpegts,mpeg,mpegvideo,ogg,wav,mp3,flac,aac,aiff,asf,"
                        "apng,gif",
                        0);
            av_dict_set(&options, "enable_drefs", "0", 0);
            av_dict_set(&options, "use_absolute_path", "0", 0);
            auto utf8 = safe.toUtf8();
            auto result = avformat_open_input(&format, utf8.constData(), nullptr, &options);
            av_dict_free(&options);
            check(result, "Open local media");
            check(avformat_find_stream_info(format, nullptr), "Read stream information");
        } catch (...) {
            cleanup();
            throw;
        }
    }
    void cleanup() {
        if (format)
            avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        file.close();
    }
    ~Input() {
        cleanup();
    }
};
struct Codec {
    AVCodecContext *ctx = nullptr;
    int stream = -1;
    explicit Codec(Input &input, AVMediaType type, int selected = -1) {
        stream =
            selected >= 0 ? selected : av_find_best_stream(input.format, type, -1, -1, nullptr, 0);
        if (selected >= 0 && (selected >= int(input.format->nb_streams) ||
                              input.format->streams[selected]->codecpar->codec_type != type))
            throw std::runtime_error("Selected media stream does not exist");
        if (type == AVMEDIA_TYPE_VIDEO && stream >= 0 &&
            (input.format->streams[stream]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            stream = -1;
            for (unsigned i = 0; i < input.format->nb_streams; ++i)
                if (input.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    !(input.format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                    stream = static_cast<int>(i);
                    break;
                }
        }
        if (stream < 0)
            return;
        auto par = input.format->streams[stream]->codecpar;
        auto codec = avcodec_find_decoder(par->codec_id);
        if (!codec)
            throw std::runtime_error("Media decoder is not installed");
        ctx = avcodec_alloc_context3(codec);
        if (!ctx)
            throw std::bad_alloc();
        try {
            check(avcodec_parameters_to_context(ctx, par), "Copy codec parameters");
            ctx->thread_count = std::min(8, std::max(1, QThread::idealThreadCount()));
            check(avcodec_open2(ctx, codec, nullptr), "Open codec");
        } catch (...) {
            avcodec_free_context(&ctx);
            throw;
        }
    }
    ~Codec() {
        avcodec_free_context(&ctx);
    }
};
struct Frame {
    AVFrame *p = av_frame_alloc();
    ~Frame() {
        av_frame_free(&p);
    }
};
struct Packet {
    AVPacket *p = av_packet_alloc();
    ~Packet() {
        av_packet_free(&p);
    }
};
qint64 timestamp(const AVFrame *f) {
    return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
}
qint64 toUs(qint64 value, AVRational tb) {
    return av_rescale_q(value, tb, {1, 1000000});
}
} // namespace
QImage readLocalRasterImage(const QString &path) {
    QString error;
    auto safe = LocalPathPolicy::validateInput(path, &error);
    if (safe.isEmpty())
        throw std::runtime_error(error.toStdString());
    QImageReader reader(safe);
    const QList<QByteArray> rasterFormats = {"png",  "jpeg", "bmp", "tiff", "webp", "tga", "ico",
                                             "icns", "pbm",  "pgm", "ppm",  "xbm",  "xpm", "gif"};
    if (!rasterFormats.contains(reader.format().toLower()))
        throw std::runtime_error("Only self-contained raster image formats are allowed");
    // Lock the vetted raster decoder: a mislabeled .png must never fall through
    // to SVG or another plugin capable of resolving nested resources.
    reader.setFormat(reader.format().toLower());
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(true);
    auto size = reader.size();
    if (size.width() > 32768 || size.height() > 32768 ||
        static_cast<qint64>(size.width()) * size.height() > 134217728)
        throw std::runtime_error("Image dimensions exceed memory safety limit");
    auto image = reader.read();
    if (image.isNull())
        throw std::runtime_error(reader.errorString().toStdString());
    return image;
}

MediaSource probeImageSequence(const QStringList &paths, Rational fps,
                               const CancelCheck &cancelled) {
    if (paths.isEmpty() || !fps.valid() || paths.size() > 4000000)
        throw std::runtime_error("Invalid image sequence");
    MediaSource s;
    s.id = newId();
    s.name = QFileInfo(paths[0]).fileName();
    s.path = paths[0];
    s.kind = paths.size() == 1 ? SourceKind::Image : SourceKind::Sequence;
    s.info.fps = fps;
    s.info.timeBase = {fps.den, fps.num};
    s.info.hasVideo = true;
    for (qsizetype n = 0; n < paths.size(); ++n) {
        checkCancel(cancelled);
        QString e;
        auto path = LocalPathPolicy::validateInput(paths[n], &e);
        if (path.isEmpty())
            throw std::runtime_error(e.toStdString());
        if (n == 0) {
            auto image = readLocalRasterImage(path);
            s.info.color = rasterColorInfo(image);
            s.info.width = image.width();
            s.info.height = image.height();
            s.path = path;
        }
        s.sequencePaths.append(path);
        s.info.framePts.append(n);
        s.info.frameDurations.append(1);
        s.info.pictureTypes.append('?');
        s.info.keyFrames.append('1');
    }
    s.info.durationUs = fps.frameToUs(paths.size());
    return s;
}
MediaSource probeMedia(const QString &path, const CancelCheck &cancelled,
                       const ProgressCallback &progress) {
    checkCancel(cancelled);
    QString error;
    auto safe = LocalPathPolicy::validateInput(path, &error);
    if (safe.isEmpty())
        throw std::runtime_error(error.toStdString());
    QImageReader imageReader(safe);
    if (imageReader.canRead() && !imageReader.supportsAnimation())
        return probeImageSequence({safe}, {24, 1}, cancelled);
    Input input(safe, cancelled);
    MediaSource s;
    s.id = newId();
    s.path = safe;
    s.name = QFileInfo(safe).fileName();
    auto &info = s.info;
    int audio = av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio >= 0) {
        auto stream = input.format->streams[audio];
        info.hasAudio = true;
        info.sampleRate = stream->codecpar->sample_rate;
        info.channels = stream->codecpar->ch_layout.nb_channels;
        info.audioStartUs =
            stream->start_time == AV_NOPTS_VALUE ? 0 : toUs(stream->start_time, stream->time_base);
    }
    info.durationUs = input.format->duration == AV_NOPTS_VALUE ? 0 : input.format->duration;
    struct TrackIndex {
        int slot = 0;
        int blockAlign = 0;
        qint64 cursor = 0;
        qint64 endUs = 0;
    };
    QMap<int, TrackIndex> trackIndices;
    for (unsigned n = 0; n < input.format->nb_streams; ++n) {
        auto stream = input.format->streams[n];
        auto par = stream->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (info.audioTracks.size() >= 64)
            throw std::runtime_error("Audio track limit exceeded");
        AudioTrack track;
        track.streamIndex = int(n);
        track.codec = QString::fromUtf8(avcodec_get_name(par->codec_id));
        if (auto tag = av_dict_get(stream->metadata, "title", nullptr, 0))
            track.title = QString::fromUtf8(tag->value);
        if (auto tag = av_dict_get(stream->metadata, "language", nullptr, 0))
            track.language = QString::fromUtf8(tag->value);
        track.sampleRate = par->sample_rate;
        track.channels = par->ch_layout.nb_channels;
        if (track.sampleRate <= 0 || track.channels <= 0)
            throw std::runtime_error("Audio stream has no usable sample format");
        track.startUs =
            stream->start_time == AV_NOPTS_VALUE ? 0 : toUs(stream->start_time, stream->time_base);
        track.durationUs =
            stream->duration == AV_NOPTS_VALUE ? 0 : toUs(stream->duration, stream->time_base);
        track.isDefault = int(n) == audio;
        auto bits = av_get_bits_per_sample(par->codec_id);
        trackIndices[int(n)] = {int(info.audioTracks.size()),
                                bits > 0 && bits % 8 == 0 ? bits / 8 * track.channels : 0};
        info.audioTracks.append(track);
    }
    qint64 audioPackets = 0;
    auto indexPcm = [&](const AVPacket *packet) {
        auto found = trackIndices.find(packet->stream_index);
        if (found == trackIndices.end())
            return;
        if (++audioPackets > 4000000)
            throw std::runtime_error("Audio index exceeds safety limit");
        auto &index = found.value();
        auto &track = info.audioTracks[index.slot];
        auto tb = input.format->streams[packet->stream_index]->time_base;
        auto pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (pts != AV_NOPTS_VALUE)
            index.endUs =
                std::max(index.endUs,
                         toUs(pts + std::max<int64_t>(0, packet->duration), tb) - track.startUs);
        if (index.blockAlign <= 0)
            return;
        if (pts != AV_NOPTS_VALUE) {
            if (track.packetPts.isEmpty())
                track.startUs = toUs(pts, tb);
            else {
                auto actualUs = toUs(pts - track.packetPts.first(), tb);
                auto expectedUs = av_rescale(index.cursor, 1000000, track.sampleRate);
                auto quantum = std::max<qint64>(1, toUs(1, tb));
                if (actualUs - expectedUs > 2 * quantum)
                    index.cursor = av_rescale(actualUs, track.sampleRate, 1000000);
            }
            track.packetPts.append(pts);
            track.sampleStarts.append(index.cursor);
        }
        index.cursor += packet->size / index.blockAlign;
    };
    auto finishTracks = [&]() {
        for (const auto &index : trackIndices) {
            auto &track = info.audioTracks[index.slot];
            track.durationUs = index.cursor > 0
                                   ? av_rescale(index.cursor, 1000000, track.sampleRate)
                                   : std::max(track.durationUs, index.endUs);
        }
        selectSourceAudioTrack(s, -1);
    };
    Codec codec(input, AVMEDIA_TYPE_VIDEO);
    if (!codec.ctx) {
        if (!info.hasAudio)
            throw std::runtime_error("File contains no supported audio or video stream");
        s.kind = SourceKind::Audio;
        info.framePts.clear();
        if (info.durationUs <= 0) {
            auto stream = input.format->streams[audio];
            info.durationUs =
                stream->duration == AV_NOPTS_VALUE ? 0 : toUs(stream->duration, stream->time_base);
        }
        {
            Packet packet;
            int result;
            while ((result = av_read_frame(input.format, packet.p)) >= 0) {
                checkCancel(cancelled);
                indexPcm(packet.p);
                av_packet_unref(packet.p);
            }
            if (result != AVERROR_EOF)
                check(result, "Index PCM audio");
        }
        finishTracks();
        if (info.durationUs <= 0)
            throw std::runtime_error("Cannot determine audio duration");
        return s;
    }
    s.kind = SourceKind::Video;
    info.hasVideo = true;
    info.width = codec.ctx->width;
    info.height = codec.ctx->height;
    if (info.width <= 0 || info.height <= 0 ||
        static_cast<qint64>(info.width) * info.height > 134217728)
        throw std::runtime_error("Video dimensions exceed memory safety limit");
    auto stream = input.format->streams[codec.stream];
    info.color = videoColorInfo(stream->codecpar);
    auto fps = av_guess_frame_rate(input.format, stream, nullptr);
    if (fps.num <= 0 || fps.den <= 0)
        fps = {24, 1};
    info.fps = {fps.num, fps.den};
    info.timeBase = {stream->time_base.num, stream->time_base.den};
    Packet packet;
    Frame frame;
    auto drain = [&]() {
        while (true) {
            checkCancel(cancelled);
            auto r = avcodec_receive_frame(codec.ctx, frame.p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            check(r, "Decode media index");
            auto pts = timestamp(frame.p);
            if (pts == AV_NOPTS_VALUE)
                pts = info.framePts.isEmpty()
                          ? 0
                          : info.framePts.last() + std::max<qint64>(1, info.frameDurations.last());
            if (!info.framePts.isEmpty() && pts < info.framePts.last())
                throw std::runtime_error("Non-monotonic decoded presentation timestamps");
            auto color = videoColorInfo(frame.p, info.color);
            if (info.framePts.isEmpty())
                info.color = color;
            else if (color != frameColorInfo(info, info.framePts.size() - 1))
                info.frameColors[info.framePts.size()] = color;
            char picture = '?';
            switch (frame.p->pict_type) {
            case AV_PICTURE_TYPE_I:
                picture = 'I';
                break;
            case AV_PICTURE_TYPE_P:
                picture = 'P';
                break;
            case AV_PICTURE_TYPE_B:
                picture = 'B';
                break;
            case AV_PICTURE_TYPE_S:
                picture = 'S';
                break;
            case AV_PICTURE_TYPE_SI:
                picture = 'i';
                break;
            case AV_PICTURE_TYPE_SP:
                picture = 'p';
                break;
            case AV_PICTURE_TYPE_BI:
                picture = 'b';
                break;
            default:
                break;
            }
            info.pictureTypes.append(picture);
            info.keyFrames.append((frame.p->flags & AV_FRAME_FLAG_KEY) ? '1' : '0');
            info.framePts.append(pts);
            info.frameDurations.append(frame.p->duration);
            if (info.framePts.size() > 4000000)
                throw std::runtime_error("Media index exceeds 4 million frames");
            if (progress && info.durationUs > 0)
                progress(static_cast<int>(std::clamp<qint64>(
                    toUs(pts, stream->time_base) * 100 / info.durationUs, 0, 99)));
            av_frame_unref(frame.p);
        }
    };
    int r = 0;
    while ((r = av_read_frame(input.format, packet.p)) >= 0) {
        checkCancel(cancelled);
        indexPcm(packet.p);
        if (packet.p->stream_index == codec.stream) {
            auto send = avcodec_send_packet(codec.ctx, packet.p);
            if (send == AVERROR(EAGAIN)) {
                drain();
                send = avcodec_send_packet(codec.ctx, packet.p);
            }
            check(send, "Feed media index");
            drain();
        }
        av_packet_unref(packet.p);
    }
    if (r != AVERROR_EOF)
        check(r, "Read media index");
    check(avcodec_send_packet(codec.ctx, nullptr), "Flush media index");
    drain();
    if (info.framePts.isEmpty())
        throw std::runtime_error("Video contains no decodable frames");
    for (qsizetype n = 0; n < info.framePts.size(); ++n) {
        auto expected = av_rescale_q(
            n, {static_cast<int>(info.fps.den), static_cast<int>(info.fps.num)}, stream->time_base);
        if (std::abs(info.framePts[n] - info.framePts[0] - expected) > 1) {
            info.variableFrameRate = true;
            break;
        }
    }
    info.videoStartUs = toUs(info.framePts[0], stream->time_base);
    // Container duration can be an absolute end timestamp (e.g. Matroska with a
    // nonzero start). Report the decoded local span, not that absolute endpoint.
    info.durationUs = info.frameEndUs(info.framePts.size() - 1);
    finishTracks();
    if (progress)
        progress(100);
    return s;
}

struct MediaDecoder::Impl {
    MediaSource source;
    CancelCheck cancelled, operationCancelled;
    std::unique_ptr<Input> video, audioInput;
    std::unique_ptr<Codec> videoCodec, audioCodec;
    SwsContext *sws = nullptr;
    SwrContext *swr = nullptr;
    QCache<qint64, VideoFrame> nearby{16 * 1024};
    qint64 lastOrdinal = -1;
    QVector<qint64> keyframes;
    bool eof = false;
    Impl(MediaSource s, CancelCheck c) : source(std::move(s)), cancelled(std::move(c)) {
        for (qint64 n = 0; n < source.info.keyFrames.size(); ++n)
            if (source.info.keyFrames[n] == '1')
                keyframes.append(n);
    }
    void checkCancelled() const {
        checkCancel(cancelled);
        checkCancel(operationCancelled);
    }
    bool canContinue(qint64 ordinal) const {
        if (eof || lastOrdinal < 0 || ordinal <= lastOrdinal)
            return false;
        if (ordinal == lastOrdinal + 1)
            return true;
        auto next = std::upper_bound(keyframes.cbegin(), keyframes.cend(), ordinal);
        if (next == keyframes.cbegin())
            return ordinal - lastOrdinal <= 32;
        // Advancing within the same GOP is cheaper than rewinding and decoding it again.
        return *std::prev(next) <= lastOrdinal;
    }
    ~Impl() {
        sws_freeContext(sws);
        swr_free(&swr);
    }
    void initVideo() {
        if (video)
            return;
        video = std::make_unique<Input>(source.path, cancelled);
        videoCodec = std::make_unique<Codec>(*video, AVMEDIA_TYPE_VIDEO);
        if (!videoCodec->ctx)
            throw std::runtime_error("No video stream");
    }
    void initAudio() {
        if (audioInput)
            return;
        audioInput = std::make_unique<Input>(source.path, cancelled);
        auto selected = source.selectedAudioTrack;
        if (selected < 0 && !source.info.audioTracks.isEmpty()) {
            selected = source.info.audioTracks.first().streamIndex;
            for (const auto &track : source.info.audioTracks)
                if (track.isDefault) {
                    selected = track.streamIndex;
                    break;
                }
        }
        audioCodec = std::make_unique<Codec>(*audioInput, AVMEDIA_TYPE_AUDIO, selected);
        if (!audioCodec->ctx)
            return;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                                  &audioCodec->ctx->ch_layout, audioCodec->ctx->sample_fmt,
                                  audioCodec->ctx->sample_rate, 0, nullptr),
              "Configure audio conversion");
        check(swr_init(swr), "Start audio conversion");
    }
};
MediaDecoder::MediaDecoder(const MediaSource &s, CancelCheck c)
    : d(std::make_unique<Impl>(s, std::move(c))) {}
MediaDecoder::~MediaDecoder() = default;
QImage MediaDecoder::frame(qint64 ordinal) {
    return ColorPipeline::quantize(ColorPipeline::displayRgba(nativeFrame(ordinal)), 8);
}
void MediaDecoder::setFrameCacheKiB(int limit) {
    d->nearby.setMaxCost(std::max(0, limit));
}
void MediaDecoder::setOperationCancelled(CancelCheck cancelled) {
    d->operationCancelled = std::move(cancelled);
}
VideoFrame MediaDecoder::nativeFrame(qint64 ordinal) {
    d->checkCancelled();
    if (!d->source.info.hasVideo)
        return {};
    ordinal = std::clamp<qint64>(ordinal, 0, d->source.info.frameCount() - 1);
    if (auto cached = d->nearby.object(ordinal))
        return *cached;
    if (d->source.kind == SourceKind::Image || d->source.kind == SourceKind::Sequence) {
        auto raster = readLocalRasterImage(
            d->source.sequencePaths.isEmpty() ? d->source.path : d->source.sequencePaths[ordinal]);
        return {{}, raster, rasterColorInfo(raster), ordinal};
    }
    d->initVideo();
    auto ctx = d->videoCodec->ctx;
    auto stream = d->video->format->streams[d->videoCodec->stream];
    auto target = d->source.info.framePts[ordinal];
    if (ordinal > 0 && d->source.info.framePts[ordinal - 1] == target)
        --target;
    if (!d->canContinue(ordinal)) {
        check(av_seek_frame(d->video->format, d->videoCodec->stream, target, AVSEEK_FLAG_BACKWARD),
              "Seek video");
        avcodec_flush_buffers(ctx);
        d->eof = false;
        d->lastOrdinal = -1;
    }
    Frame f;
    Packet p;
    VideoFrame result;
    bool found = false;
    auto drain = [&]() {
        while (true) {
            d->checkCancelled();
            auto r = avcodec_receive_frame(ctx, f.p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            check(r, "Decode video frame");
            auto pts = timestamp(f.p);
            auto it = std::lower_bound(d->source.info.framePts.begin(),
                                       d->source.info.framePts.end(), pts);
            qint64 index = it == d->source.info.framePts.end()
                               ? d->source.info.frameCount() - 1
                               : std::distance(d->source.info.framePts.begin(), it);
            if (pts == AV_NOPTS_VALUE)
                index = d->lastOrdinal + 1;
            else if (d->lastOrdinal >= 0 && d->lastOrdinal + 1 < d->source.info.framePts.size() &&
                     d->source.info.framePts[d->lastOrdinal + 1] == pts)
                index = d->lastOrdinal + 1;
            {
                auto image = retainVideoFrame(f.p, index, frameColorInfo(d->source.info, index));
                d->nearby.insert(index, new VideoFrame(image),
                                 std::max(1, static_cast<int>(image.byteSize() / 1024)));
                if (index == ordinal) {
                    result = image;
                    found = true;
                }
            }
            d->lastOrdinal = index;
            av_frame_unref(f.p);
        }
    };
    // Receive already queued B-frames before consuming another packet.
    drain();
    while (!found && !d->eof) {
        d->checkCancelled();
        int r = av_read_frame(d->video->format, p.p);
        if (r == AVERROR_EOF) {
            d->eof = true;
            check(avcodec_send_packet(ctx, nullptr), "Flush video");
            drain();
            break;
        }
        check(r, "Read video packet");
        if (p.p->stream_index == d->videoCodec->stream) {
            auto r2 = avcodec_send_packet(ctx, p.p);
            if (r2 == AVERROR(EAGAIN)) {
                drain();
                r2 = avcodec_send_packet(ctx, p.p);
            }
            check(r2, "Feed video decoder");
            drain();
        }
        av_packet_unref(p.p);
    }
    if (!found)
        throw std::runtime_error("Requested frame was not found after seek");
    return result;
}
QVector<float> MediaDecoder::audio(qint64 startUs, qint64 durationUs) {
    if (durationUs <= 0)
        return {};
    if (durationUs > 10000000)
        throw std::runtime_error("Audio request exceeds ten-second bound");
    const qint64 count = av_rescale(durationUs, 48000, 1000000);
    QVector<float> result(count * 2, 0.0f);
    if (!d->source.info.hasAudio || startUs <= -durationUs)
        return result;
    // Large user offsets are silence, not huge seeks or overflowing sample indices.
    qint64 audioEnd = d->source.info.durationUs;
    for (const auto &track : d->source.info.audioTracks)
        audioEnd = std::max(audioEnd, track.durationUs);
    if (audioEnd > 0 && startUs >= audioEnd)
        return result;
    d->checkCancelled();
    d->initAudio();
    if (!d->audioCodec->ctx)
        return result;
    auto ctx = d->audioCodec->ctx;
    auto stream = d->audioInput->format->streams[d->audioCodec->stream];
    auto origin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    if (stream->duration != AV_NOPTS_VALUE && startUs >= toUs(stream->duration, stream->time_base))
        return result;
    auto seekUs = std::max<qint64>(0, startUs - 200000);
    auto target = origin + av_rescale_q(seekUs, {1, 1000000}, stream->time_base);
    check(av_seek_frame(d->audioInput->format, d->audioCodec->stream, target,
                        AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY),
          "Seek audio");
    avcodec_flush_buffers(ctx);
    swr_close(d->swr);
    check(swr_init(d->swr), "Reset audio resampler");
    Frame f;
    Packet p;
    bool done = false;
    qint64 tailOffset = 0;
    std::optional<qint64> nextInputSample;
    auto drain = [&]() {
        while (!done) {
            d->checkCancelled();
            auto r = avcodec_receive_frame(ctx, f.p);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            check(r, "Decode audio");
            auto pts = timestamp(f.p);
            qint64 localUs = pts == AV_NOPTS_VALUE ? seekUs : toUs(pts - origin, stream->time_base);
            qint64 inputSample = av_rescale(localUs, ctx->sample_rate, 1000000);
            bool indexed = false;
            auto position = std::lower_bound(d->source.info.audioPacketPts.begin(),
                                             d->source.info.audioPacketPts.end(), pts);
            if (position != d->source.info.audioPacketPts.end() && *position == pts) {
                auto index = std::distance(d->source.info.audioPacketPts.begin(), position);
                inputSample = d->source.info.audioSampleStarts[index];
                indexed = true;
            }
            if (!indexed && nextInputSample) {
                auto tolerance =
                    std::max<qint64>(1, av_rescale_q(2, stream->time_base, {1, ctx->sample_rate}));
                if (std::abs(inputSample - *nextInputSample) <= tolerance)
                    inputSample = *nextInputSample;
            }
            nextInputSample = inputSample + f.p->nb_samples;
            localUs = av_rescale(inputSample, 1000000, ctx->sample_rate);
            auto delay = swr_get_delay(d->swr, ctx->sample_rate);
            auto capacity =
                av_rescale_rnd(delay + f.p->nb_samples, 48000, ctx->sample_rate, AV_ROUND_UP);
            QVector<float> converted(capacity * 2);
            uint8_t *output[] = {reinterpret_cast<uint8_t *>(converted.data())};
            int samples =
                swr_convert(d->swr, output, static_cast<int>(capacity),
                            const_cast<const uint8_t **>(f.p->extended_data), f.p->nb_samples);
            check(samples, "Resample audio");
            auto offset = av_rescale(localUs - startUs, 48000, 1000000) -
                          av_rescale(delay, 48000, ctx->sample_rate);
            auto first = std::max<qint64>(0, -offset);
            auto last = std::min<qint64>(samples, count - offset);
            if (last > first)
                std::memcpy(result.data() + (offset + first) * 2, converted.constData() + first * 2,
                            static_cast<size_t>((last - first) * 2) * sizeof(float));
            tailOffset = offset + samples;
            if (tailOffset >= count)
                done = true;
            av_frame_unref(f.p);
        }
    };
    while (!done) {
        auto r = av_read_frame(d->audioInput->format, p.p);
        if (r == AVERROR_EOF) {
            check(avcodec_send_packet(ctx, nullptr), "Flush audio");
            drain();
            while (!done && swr_get_delay(d->swr, 48000) > 0) {
                QVector<float> tail(8192);
                uint8_t *output[] = {reinterpret_cast<uint8_t *>(tail.data())};
                auto n = swr_convert(d->swr, output, 4096, nullptr, 0);
                check(n, "Flush audio resampler");
                if (n == 0)
                    break;
                auto first = std::max<qint64>(0, -tailOffset);
                auto last = std::min<qint64>(n, count - tailOffset);
                if (last > first)
                    std::memcpy(result.data() + (tailOffset + first) * 2,
                                tail.constData() + first * 2,
                                static_cast<size_t>((last - first) * 2) * sizeof(float));
                tailOffset += n;
                if (tailOffset >= count)
                    done = true;
            }
            break;
        }
        check(r, "Read audio packet");
        if (p.p->stream_index == d->audioCodec->stream) {
            auto send = avcodec_send_packet(ctx, p.p);
            if (send == AVERROR(EAGAIN)) {
                drain();
                send = avcodec_send_packet(ctx, p.p);
            }
            check(send, "Feed audio decoder");
            drain();
        }
        av_packet_unref(p.p);
    }
    return result;
}
struct MediaEngine::Impl {
    QCache<QString, VideoFrame> cache;
    QCache<QString, QImage> overlays{1024};
    int budgetKiB;
    SequenceReader sequences;
    std::map<QString, std::unique_ptr<MediaDecoder>> decoders;
    QStringList usage;
    CancelCheck cancelled, operationCancelled;
    Impl(int mib, CancelCheck c)
        : cache(mib * 512), budgetKiB(mib * 1024),
          sequences(qint64(mib) * 1024 * 256,
                    std::min(4, std::max(1, QThread::idealThreadCount() / 2))),
          cancelled(std::move(c)) {}
    MediaDecoder &decoder(const MediaSource &s) {
        const auto key = s.id + ':' + s.path + ':' + QString::number(s.selectedAudioTrack);
        usage.removeAll(key);
        usage.append(key);
        if (!decoders.contains(key)) {
            while (decoders.size() >= 4) {
                decoders.erase(usage.takeFirst());
            }
            auto decoder = std::make_unique<MediaDecoder>(s, cancelled);
            decoder->setFrameCacheKiB(budgetKiB / 16);
            decoder->setOperationCancelled(
                [this] { return operationCancelled && operationCancelled(); });
            decoders.emplace(key, std::move(decoder));
        }
        return *decoders.at(key);
    }
};
MediaEngine::MediaEngine(int mib, CancelCheck c) : d(std::make_unique<Impl>(mib, std::move(c))) {}
MediaEngine::~MediaEngine() = default;
QImage MediaEngine::frame(const MediaSource &s, qint64 f) {
    return ColorPipeline::quantize(ColorPipeline::displayRgba(nativeFrame(s, f)), 8);
}
VideoFrame MediaEngine::nativeFrame(const MediaSource &s, qint64 f) {
    checkCancelled();
    f = std::clamp<qint64>(f, 0, std::max<qint64>(0, s.info.frameCount() - 1));
    if (s.kind == SourceKind::Image || s.kind == SourceKind::Sequence)
        return d->sequences.frame(s, f);
    auto key = s.id + ':' + s.path + ':' + QString::number(f);
    if (auto frame = d->cache.object(key))
        return *frame;
    auto frame = d->decoder(s).nativeFrame(f);
    if (!frame.isNull())
        d->cache.insert(key, new VideoFrame(frame), std::max(1, int(frame.byteSize() / 1024)));
    return frame;
}
void MediaEngine::prefetch(const MediaSource &source, const QVector<qint64> &ordinals) {
    if (source.kind == SourceKind::Image || source.kind == SourceKind::Sequence)
        d->sequences.prefetch(source, ordinals);
}
QJsonObject MediaEngine::sequenceDiagnostics() const {
    return d->sequences.diagnostics();
}
QImage MediaEngine::overlayImage(const QString &path) {
    QString error;
    const auto safe = LocalPathPolicy::validateInput(path, &error);
    if (safe.isEmpty())
        throw std::runtime_error(error.toStdString());
    const QFileInfo info(safe);
    const auto key = "overlay:" + safe + ':' + QString::number(info.size()) + ':' +
                     QString::number(info.lastModified().toMSecsSinceEpoch());
    if (auto cached = d->overlays.object(key))
        return *cached;
    auto image = readLocalRasterImage(safe);
    d->overlays.insert(key, new QImage(image), std::max(1, int(image.sizeInBytes() / 1024)));
    return image;
}
QVector<float> MediaEngine::sourceAudio(const MediaSource &s, qint64 start, qint64 length) {
    return d->decoder(s).audio(start, length);
}
QVector<float> MediaEngine::ownerAudio(const Project &p, const QString &id, qint64 start,
                                       qint64 length) {
    if (auto s = p.source(id))
        return sourceAudio(*s, sourceAudioTime(*s, start), length);
    return timelineAudio(p, id, start, length);
}
QVector<float> MediaEngine::timelineAudio(const Project &p, const QString &timelineId,
                                          qint64 startUs, qint64 durationUs) {
    const auto samples = av_rescale(durationUs, 48000, 1000000);
    QVector<float> result(samples * 2, 0.0f);
    auto t = p.timeline(timelineId.isEmpty() ? p.activeTimelineId : timelineId);
    if (!t)
        return result;
    qint64 startFrame = 0;
    for (const auto &c : t->clips) {
        const auto count = clipDurationFrames(p, *t, c);
        auto clipStart = t->fps.frameToUs(startFrame);
        auto clipEnd = t->fps.frameToUs(startFrame + count);
        startFrame += count;
        auto from = std::max(startUs, clipStart), to = std::min(startUs + durationUs, clipEnd);
        if (to <= from || c.muted)
            continue;
        auto s = p.source(c.audioSourceId.isEmpty() ? c.sourceId : c.audioSourceId);
        auto video = p.source(c.sourceId);
        if (!s || !video || !s->info.hasAudio)
            continue;
        auto sourceUs = from - clipStart;
        if (c.audioSourceId.isEmpty())
            sourceUs = sourceAudioTime(*video, sourceUs + video->info.frameStartUs(c.inFrame));
        else
            sourceUs = shiftedAudioTime(sourceUs, s->audioOffsetUs);
        sourceUs = shiftedAudioTime(sourceUs, c.audioOffsetUs);
        auto data = sourceAudio(*s, sourceUs, to - from);
        auto offset = av_rescale(from - startUs, 48000, 1000000);
        auto n = std::min<qint64>(data.size() / 2, samples - offset);
        for (qint64 i = 0; i < n * 2; ++i)
            result[offset * 2 + i] = std::clamp(data[i] * static_cast<float>(c.gain), -1.0f, 1.0f);
    }
    return result;
}
void MediaEngine::setOperationCancelled(CancelCheck cancelled) {
    d->operationCancelled = std::move(cancelled);
}
void MediaEngine::checkCancelled() const {
    checkCancel(d->cancelled);
    checkCancel(d->operationCancelled);
}
void MediaEngine::setCacheLimitMiB(int mib) {
    d->budgetKiB = std::clamp(mib, 16, 4096) * 1024;
    d->cache.setMaxCost(d->budgetKiB / 2);
    d->sequences.setBudget(qint64(d->budgetKiB) * 256);
    for (auto &[key, decoder] : d->decoders)
        decoder->setFrameCacheKiB(d->budgetKiB / 16);
}
void MediaEngine::clear() {
    d->cache.clear();
    d->overlays.clear();
    d->sequences.clear();
    d->decoders.clear();
    d->usage.clear();
}
} // namespace openframe
