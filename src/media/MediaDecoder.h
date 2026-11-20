#pragma once
#include "core/Project.h"
#include "ColorPipeline.h"
#include <QCache>
#include <atomic>
#include <functional>
#include <memory>

namespace openframe {
using CancelCheck = std::function<bool()>;
using ProgressCallback = std::function<void(int)>;
QImage readLocalRasterImage(const QString &path);
MediaSource probeMedia(const QString &path, const CancelCheck &cancelled = {},
                       const ProgressCallback &progress = {});
MediaSource probeImageSequence(const QStringList &paths, Rational fps,
                               const CancelCheck &cancelled = {});

class MediaDecoder {
  public:
    explicit MediaDecoder(const MediaSource &source, CancelCheck cancelled = {});
    ~MediaDecoder();
    MediaDecoder(const MediaDecoder &) = delete;
    QImage frame(qint64 ordinal);
    VideoFrame nativeFrame(qint64 ordinal);
    void setFrameCacheKiB(int limit);
    // Interleaved float stereo 48kHz; negative/out-of-file areas are silence.
    QVector<float> audio(qint64 startUs, qint64 durationUs);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
class MediaEngine {
  public:
    explicit MediaEngine(int cacheMiB = 256, CancelCheck cancelled = {});
    ~MediaEngine();
    QImage frame(const MediaSource &source, qint64 ordinal);
    VideoFrame nativeFrame(const MediaSource &source, qint64 ordinal);
    void prefetch(const MediaSource &source, const QVector<qint64> &ordinals);
    QJsonObject sequenceDiagnostics() const;
    QImage overlayImage(const QString &path);
    QVector<float> sourceAudio(const MediaSource &source, qint64 startUs, qint64 durationUs);
    // Owner-relative time, including stream alignment and source/clip audio offsets.
    QVector<float> ownerAudio(const Project &, const QString &ownerId, qint64 startUs,
                              qint64 durationUs);
    QVector<float> timelineAudio(const Project &, const QString &timelineId, qint64 startUs,
                                 qint64 durationUs);
    void setCacheLimitMiB(int mib);
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace openframe
