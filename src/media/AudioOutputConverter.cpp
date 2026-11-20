#include "AudioOutputConverter.h"
#include <stdexcept>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
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
AVSampleFormat sampleFormat(QAudioFormat::SampleFormat format) {
    switch (format) {
    case QAudioFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}
} // namespace
struct AudioOutputConverter::Impl {
    SwrContext *swr = nullptr;
    QAudioFormat format;
    qint64 input = 0, output = 0;
    bool finished = false;
    ~Impl() {
        swr_free(&swr);
    }
    QByteArray process(const float *data, int frames) {
        const auto capacity = swr_get_out_samples(swr, frames);
        check(capacity, "Size device audio conversion");
        if (capacity > format.sampleRate() * 2)
            throw std::runtime_error("Device audio conversion exceeded its bounded buffer");
        QByteArray result(capacity * format.bytesPerFrame(), '\0');
        uint8_t *out[] = {reinterpret_cast<uint8_t *>(result.data())};
        const uint8_t *in[] = {reinterpret_cast<const uint8_t *>(data)};
        const auto count = swr_convert(swr, out, capacity, data ? in : nullptr, frames);
        check(count, "Convert device audio");
        result.resize(count * format.bytesPerFrame());
        input += frames;
        output += count;
        return result;
    }
};
bool AudioOutputConverter::supports(const QAudioFormat &format) {
    return format.isValid() && format.sampleRate() >= 8000 && format.sampleRate() <= 384000 &&
           format.channelCount() >= 1 && format.channelCount() <= 8 &&
           sampleFormat(format.sampleFormat()) != AV_SAMPLE_FMT_NONE;
}
AudioOutputConverter::AudioOutputConverter(const QAudioFormat &format)
    : d(std::make_unique<Impl>()) {
    if (!supports(format))
        throw std::invalid_argument(
            "Unsupported device PCM format (8–384 kHz, 1–8 channels required)");
    d->format = format;
    if (format.sampleRate() == 48000 && format.channelCount() == 2 &&
        format.sampleFormat() == QAudioFormat::Float)
        return;
    AVChannelLayout input = AV_CHANNEL_LAYOUT_STEREO, output;
    av_channel_layout_default(&output, format.channelCount());
    const auto result =
        swr_alloc_set_opts2(&d->swr, &output, sampleFormat(format.sampleFormat()),
                            format.sampleRate(), &input, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr);
    av_channel_layout_uninit(&output);
    check(result, "Configure device audio conversion");
    check(swr_init(d->swr), "Start device audio conversion");
}
AudioOutputConverter::~AudioOutputConverter() = default;
QByteArray AudioOutputConverter::convert(const QVector<float> &samples) {
    if (d->finished || samples.size() % 2 || samples.size() > 48000 * 2)
        throw std::invalid_argument("Device audio input must be a bounded stereo block before EOF");
    if (samples.isEmpty())
        return {};
    const auto count = static_cast<int>(samples.size() / 2);
    if (d->swr)
        return d->process(samples.constData(), count);
    d->input += count;
    d->output += count;
    return QByteArray(reinterpret_cast<const char *>(samples.constData()),
                      static_cast<qsizetype>(samples.size() * sizeof(float)));
}
QByteArray AudioOutputConverter::finish() {
    if (d->finished)
        return {};
    d->finished = true;
    QByteArray result;
    if (d->swr)
        for (;;) {
            auto tail = d->process(nullptr, 0);
            if (tail.isEmpty())
                break;
            result += tail;
            if (result.size() > d->format.bytesForDuration(2000000))
                throw std::runtime_error("Device audio tail exceeded its bounded buffer");
        }
    return result;
}
qint64 AudioOutputConverter::inputFrames() const {
    return d->input;
}
qint64 AudioOutputConverter::outputFrames() const {
    return d->output;
}
} // namespace openframe
