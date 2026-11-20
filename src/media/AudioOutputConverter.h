#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <QVector>
#include <memory>

namespace openframe {
// Streaming conversion of the internal 48 kHz stereo float bus to a negotiated
// packed device format. Keep one instance per transport epoch, on its worker.
class AudioOutputConverter final {
  public:
    explicit AudioOutputConverter(const QAudioFormat &outputFormat);
    ~AudioOutputConverter();
    AudioOutputConverter(const AudioOutputConverter &) = delete;
    QByteArray convert(const QVector<float> &samples);
    QByteArray finish();
    qint64 inputFrames() const;
    qint64 outputFrames() const;
    static bool supports(const QAudioFormat &format);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace openframe
