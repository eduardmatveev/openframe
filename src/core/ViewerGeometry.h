#pragma once
#include <QLineF>
#include <QPolygonF>
#include <QSizeF>
namespace openframe {
struct ViewerSettings;
QPointF wipePivot(const ViewerSettings &settings);
QPolygonF wipeClipPolygon(const ViewerSettings &settings, QSizeF canvas, int pane);
QLineF wipeDivider(const ViewerSettings &settings, QSizeF canvas);
int wipePaneAt(const ViewerSettings &settings, QPointF normalized, QSizeF canvas);
} // namespace openframe
