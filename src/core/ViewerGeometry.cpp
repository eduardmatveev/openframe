#include "ViewerGeometry.h"
#include "Project.h"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace openframe {
namespace {
double distance(const ViewerSettings &v, QSizeF size, QPointF pixels) {
    const double angle = v.wipeAngleDegrees * std::numbers::pi / 180.0;
    return std::cos(angle) * (pixels.x() - v.wipe * size.width()) +
           std::sin(angle) * (pixels.y() - v.wipePivotY * size.height());
}
QPolygonF bounds(QSizeF size) {
    return {{0, 0}, {size.width(), 0}, {size.width(), size.height()}, {0, size.height()}};
}
} // namespace
QPointF wipePivot(const ViewerSettings &v) {
    return {v.wipe, v.wipePivotY};
}
int wipePaneAt(const ViewerSettings &v, QPointF normalized, QSizeF size) {
    if (size.isEmpty())
        size = v.outputSize;
    return distance(v, size, {normalized.x() * size.width(), normalized.y() * size.height()}) <= 0
               ? 0
               : 1;
}
QPolygonF wipeClipPolygon(const ViewerSettings &v, QSizeF size, int pane) {
    if (size.isEmpty() || pane < 0 || pane > 1)
        return {};
    const auto rectangle = bounds(size);
    QPolygonF result;
    const double sign = pane == 0 ? 1.0 : -1.0;
    for (int i = 0; i < rectangle.size(); ++i) {
        const auto a = rectangle[i], b = rectangle[(i + 1) % rectangle.size()];
        const double da = sign * distance(v, size, a), db = sign * distance(v, size, b);
        const bool insideA = da <= 1e-9, insideB = db <= 1e-9;
        if (insideA)
            result.append(a);
        if (insideA != insideB)
            result.append(a + (b - a) * (da / (da - db)));
    }
    return result;
}
QLineF wipeDivider(const ViewerSettings &v, QSizeF size) {
    if (size.isEmpty())
        return {};
    const auto rectangle = bounds(size);
    QVector<QPointF> points;
    auto append = [&](QPointF point) {
        for (auto existing : points)
            if (QLineF(existing, point).length() < 1e-7)
                return;
        points.append(point);
    };
    for (int i = 0; i < rectangle.size(); ++i) {
        const auto a = rectangle[i], b = rectangle[(i + 1) % rectangle.size()];
        const double da = distance(v, size, a), db = distance(v, size, b);
        if (std::abs(da) < 1e-7)
            append(a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0))
            append(a + (b - a) * (da / (da - db)));
    }
    QLineF result;
    for (auto a : points)
        for (auto b : points)
            if (QLineF(a, b).length() > result.length())
                result = {a, b};
    return result;
}
} // namespace openframe
