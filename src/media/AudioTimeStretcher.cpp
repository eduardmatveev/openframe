#include "AudioTimeStretcher.h"
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace openframe {
namespace {
void check(int result, const char *operation) {
    if (result >= 0)
        return;
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
struct Frame {
    AVFrame *p = av_frame_alloc();
    Frame() {
        if (!p)
            throw std::bad_alloc();
    }
    ~Frame() {
        av_frame_free(&p);
    }
};
} // namespace
struct AudioTimeStretcher::Impl {
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr, *sink = nullptr;
    QVector<float> samples;
    QVector<float> pendingInput;
    qint64 input = 0, output = 0;
    qint64 fed = 0;
    bool eof = false;
    ~Impl() {
        avfilter_graph_free(&graph);
    }
    void append(const float *data, int frames) {
        if (samples.size() / 2 + frames > MaximumBufferedFrames)
            throw std::runtime_error("Time-stretch output queue exceeded its bounded capacity");
        const auto old = samples.size();
        samples.resize(old + frames * 2);
        std::memcpy(samples.data() + old, data, static_cast<size_t>(frames) * 2 * sizeof(float));
        output += frames;
    }
    void drain() {
        Frame frame;
        while (samples.size() / 2 < SampleRate * 2) {
            // Bound each pull too: a slow multi-stage graph can release a large
            // burst at EOF. Leave surplus in the filter until the consumer asks.
            const auto result = av_buffersink_get_samples(sink, frame.p, 4096);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                break;
            check(result, "Receive time-stretched audio");
            if (frame.p->format != AV_SAMPLE_FMT_FLT || frame.p->ch_layout.nb_channels != 2 ||
                frame.p->sample_rate != SampleRate)
                throw std::runtime_error("Unexpected time-stretch output format");
            append(reinterpret_cast<const float *>(frame.p->data[0]), frame.p->nb_samples);
            av_frame_unref(frame.p);
        }
    }
    void feed(int count) {
        Frame frame;
        frame.p->format = AV_SAMPLE_FMT_FLT;
        frame.p->sample_rate = SampleRate;
        frame.p->nb_samples = count;
        frame.p->pts = fed;
        av_channel_layout_default(&frame.p->ch_layout, 2);
        check(av_frame_get_buffer(frame.p, 0), "Allocate audio filter input");
        std::memcpy(frame.p->data[0], pendingInput.constData(),
                    static_cast<size_t>(count) * 2 * sizeof(float));
        check(av_buffersrc_add_frame_flags(source, frame.p, 0), "Feed time stretch");
        pendingInput.remove(0, count * 2);
        fed += count;
        drain();
    }
};
QVector<double> AudioTimeStretcher::tempoStages(double tempo) {
    if (!std::isfinite(tempo) || tempo < .0625 || tempo > 16)
        throw std::invalid_argument("Audio tempo must be between 1/16 and 16");
    if (tempo == 1)
        return {};
    const auto count = static_cast<int>(std::ceil(std::abs(std::log2(tempo))));
    return QVector<double>(count, std::pow(tempo, 1.0 / count));
}
AudioTimeStretcher::AudioTimeStretcher(double tempo) : d(std::make_unique<Impl>()) {
    const auto stages = tempoStages(tempo);
    if (stages.isEmpty())
        return; // 1x is bit-exact and has no lookahead.
    d->graph = avfilter_graph_alloc();
    if (!d->graph)
        throw std::bad_alloc();
    d->graph->nb_threads = 1;
    auto create = [&](const char *type, const QString &name, const QString &args) {
        auto filter = avfilter_get_by_name(type);
        if (!filter)
            throw std::runtime_error(std::string("Required FFmpeg filter is unavailable: ") + type);
        AVFilterContext *context = nullptr;
        check(avfilter_graph_create_filter(&context, filter, name.toUtf8().constData(),
                                           args.isEmpty() ? nullptr : args.toUtf8().constData(),
                                           nullptr, d->graph),
              "Create audio filter");
        return context;
    };
    d->source = create("abuffer", "source",
                       "sample_rate=48000:sample_fmt=flt:channel_layout=stereo:time_base=1/48000");
    auto previous = d->source;
    for (int i = 0; i < stages.size(); ++i) {
        auto stage =
            create("atempo", QString("tempo%1").arg(i), QString::number(stages[i], 'g', 17));
        check(avfilter_link(previous, 0, stage, 0), "Link audio tempo stage");
        previous = stage;
    }
    auto format =
        create("aformat", "format", "sample_fmts=flt:sample_rates=48000:channel_layouts=stereo");
    d->sink = create("abuffersink", "sink", {});
    check(avfilter_link(previous, 0, format, 0), "Link audio format");
    check(avfilter_link(format, 0, d->sink, 0), "Link audio output");
    check(avfilter_graph_config(d->graph, nullptr), "Configure time stretch");
}
AudioTimeStretcher::~AudioTimeStretcher() = default;
void AudioTimeStretcher::push(const QVector<float> &samples) {
    if (d->eof || samples.size() % 2 || samples.size() / 2 > MaximumInputFrames)
        throw std::invalid_argument("Invalid time-stretch input block or push after EOF");
    if (samples.isEmpty())
        return;
    if (availableFrames() >= SampleRate * 2)
        throw std::runtime_error("Consume time-stretch output before pushing more input");
    const auto count = static_cast<int>(samples.size() / 2);
    if (!d->graph) {
        d->append(samples.constData(), count);
    } else {
        // Deterministic packetization avoids dependency of atempo's overlap
        // decisions on decoder blocks, GUI scheduling, or read boundaries.
        d->pendingInput += samples;
        while (d->pendingInput.size() >= 1024 * 2)
            d->feed(1024);
    }
    d->input += count;
}
void AudioTimeStretcher::finish() {
    if (d->eof)
        return;
    d->eof = true;
    if (d->graph) {
        if (!d->pendingInput.isEmpty())
            d->feed(static_cast<int>(d->pendingInput.size() / 2));
        check(av_buffersrc_close(d->source, d->input, 0), "Finish time stretch");
        d->drain();
    }
}
QVector<float> AudioTimeStretcher::take(int count) {
    count = std::clamp(count, 0, availableFrames());
    QVector<float> result(d->samples.cbegin(), d->samples.cbegin() + count * 2);
    d->samples.remove(0, count * 2);
    if (d->graph)
        d->drain();
    return result;
}
int AudioTimeStretcher::availableFrames() const {
    return static_cast<int>(d->samples.size() / 2);
}
qint64 AudioTimeStretcher::inputFrames() const {
    return d->input;
}
qint64 AudioTimeStretcher::outputFrames() const {
    return d->output;
}
bool AudioTimeStretcher::finished() const {
    return d->eof;
}
} // namespace openframe
