#pragma once
#include "core/Project.h"
#include "MediaDecoder.h"
namespace openframe {
class FrameComposer {
  public:
    static QImage compose(const Project &, qint64 frame, MediaEngine &, const QSize &size = {},
                          bool annotations = true);
    static QImage timeline(const Project &, const QString &timelineId, qint64 frame, MediaEngine &,
                           const QSize &size, bool annotations = true);
    static QImage composeHighPrecision(const Project &, qint64 frame, MediaEngine &,
                                      const QSize &size = {}, bool annotations = true);
    static QImage timelineHighPrecision(const Project &, const QString &owner, qint64 frame,
                                       MediaEngine &, const QSize &, bool annotations = true,
                                       const VideoColorInfo *preserveColor = nullptr);
    static void paintAnnotations(QImage &, const Project &, const QString &timelineId,
                                 qint64 frame);
    static void paintOverlay(QImage &, const OverlaySettings &);
};
} // namespace openframe
