#pragma once
#include "core/Project.h"
#include "MediaDecoder.h"
#include <array>
namespace openframe {
class FrameComposer {
  public:
    // Per-frame layers for a gesture that changes only the opacity or wipe position.
    using ComparisonLayers = std::array<QImage, 2>;
    static QImage compose(const Project &, qint64 frame, MediaEngine &, const QSize &size = {},
                          bool annotations = true, ComparisonLayers *layers = nullptr);
    static QImage timeline(const Project &, const QString &timelineId, qint64 frame, MediaEngine &,
                           const QSize &size, bool annotations = true);
    static QImage composeHighPrecision(const Project &, qint64 frame, MediaEngine &,
                                      const QSize &size = {}, bool annotations = true,
                                      ComparisonLayers *layers = nullptr);
    static QImage timelineHighPrecision(const Project &, const QString &owner, qint64 frame,
                                       MediaEngine &, const QSize &, bool annotations = true,
                                       const VideoColorInfo *preserveColor = nullptr);
    static void paintAnnotations(QImage &, const Project &, const QString &timelineId,
                                 qint64 frame);
    static void paintOverlay(QImage &, const OverlaySettings &);
};
} // namespace openframe
