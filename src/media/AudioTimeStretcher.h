#pragma once
#include <QVector>
#include <memory>

namespace openframe {
// Single-threaded, streaming 48 kHz interleaved stereo float tempo conversion.
// Push bounded blocks and consume output between pushes. Recreate on discontinuity.
class AudioTimeStretcher final {
  public:
    static constexpr int SampleRate = 48000;
    static constexpr int MaximumInputFrames = 4096;
    static constexpr int MaximumBufferedFrames = SampleRate * 4;
    explicit AudioTimeStretcher(double tempo);
    ~AudioTimeStretcher();
    AudioTimeStretcher(const AudioTimeStretcher &) = delete;
    void push(const QVector<float> &interleavedStereo);
    void finish();
    QVector<float> take(int maximumFrames);
    int availableFrames() const;
    qint64 inputFrames() const;
    qint64 outputFrames() const;
    bool finished() const;
    static QVector<double> tempoStages(double tempo);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace openframe
