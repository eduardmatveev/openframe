#include "AnnotationRenderer.h"
#include "core/Project.h"
#include <QColorSpace>
#include <QFont>
#include <QPainter>
#include <QPainterPathStroker>
#include <algorithm>
#include <cmath>
#include <limits>
namespace openframe {
namespace {
QImage layer(QSize size) {
    QImage image(size, QImage::Format_RGBA64_Premultiplied);
    if (image.isNull())
        throw std::bad_alloc();
    image.setColorSpace(QColorSpace::SRgb);
    image.fill(Qt::transparent);
    return image;
}
QPainterPath outline(const Annotation &a, QSize size) {
    const double factor = size.width() / 1920.0;
    const double width = std::max(.5, a.width * factor);
    auto point = [&](QPointF p) { return QPointF(p.x() * size.width(), p.y() * size.height()); };
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    if (a.tool == AnnotationTool::Text || !a.text.isEmpty()) {
        QFont font("Segoe UI");
        font.setPixelSize(std::max(8, int(a.fontSize * factor)));
        result.addText(point(a.textPosition), font, a.text);
        return result;
    }
    if (a.points.isEmpty())
        return result;
    const bool freehand = a.tool == AnnotationTool::Pencil || a.tool == AnnotationTool::Eraser;
    if (freehand && a.widthScales.size() == a.points.size()) {
        for (qsizetype i = 0; i < a.points.size(); ++i) {
            const auto p = point(a.points[i]);
            const auto radius = width * std::clamp(a.widthScales[i], 0.0, 1.0) / 2;
            if (radius > 0)
                result.addEllipse(p, radius, radius);
            if (i == 0)
                continue;
            const auto previous = point(a.points[i - 1]);
            const auto delta = p - previous;
            const auto length = std::hypot(delta.x(), delta.y());
            if (length < 1e-9)
                continue;
            const QPointF normal(-delta.y() / length, delta.x() / length);
            const auto priorRadius = width * std::clamp(a.widthScales[i - 1], 0.0, 1.0) / 2;
            result.addPolygon({previous + normal * priorRadius, previous - normal * priorRadius,
                               p - normal * radius, p + normal * radius});
            result.closeSubpath();
        }
        return result;
    }
    QPainterPath path;
    const auto first = point(a.points.first()), last = point(a.points.last());
    if (a.points.size() == 1) {
        result.addEllipse(first, width / 2, width / 2);
        return result;
    }
    const auto rectangle = QRectF(first, last).normalized();
    if (a.tool == AnnotationTool::Rectangle)
        path.addRect(rectangle);
    else if (a.tool == AnnotationTool::Ellipse)
        path.addEllipse(rectangle);
    else {
        path.moveTo(first);
        if (a.tool == AnnotationTool::Arrow)
            path.lineTo(last);
        else
            for (qsizetype i = 1; i < a.points.size(); ++i)
                path.lineTo(point(a.points[i]));
    }
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    result.addPath(stroker.createStroke(path));
    if (a.tool == AnnotationTool::Arrow) {
        const double angle = std::atan2(last.y() - first.y(), last.x() - first.x());
        const double length = std::max(8.0, 18 * factor);
        QPainterPath head;
        head.addPolygon({last, last - QPointF(std::cos(angle - .5), std::sin(angle - .5)) * length,
                         last - QPointF(std::cos(angle + .5), std::sin(angle + .5)) * length});
        head.closeSubpath();
        result = result.united(head);
    }
    return result;
}
struct Appearance {
    bool visible = false;
    QColor color;
    double opacity = 0;
};
Appearance appearance(const Annotation &a, const Project &p, qint64 frame,
                      const QVector<qint64> &keys) {
    qint64 last = std::max(a.frame, a.endFrame);
    if (!a.background && (a.hold || p.viewer.holdAnnotations)) {
        last = std::numeric_limits<qint64>::max();
        for (const auto &other : p.annotations)
            if (!other.background && other.timelineId == a.timelineId && other.layer == a.layer &&
                other.frame > a.frame)
                last = std::min(last, other.frame - 1);
    }
    qint64 distance = a.background      ? 0
                      : frame < a.frame ? a.frame - frame
                      : frame > last    ? frame - last
                                        : 0;
    Appearance result{true, a.color, a.opacity};
    if (distance == 0)
        return result;
    const bool previous = frame > last;
    const int limit = p.viewer.ghostEnabled
                          ? (previous ? p.viewer.ghostPrevious : p.viewer.ghostNext)
                          : a.ghostFrames;
    if (p.viewer.ghostEnabled && p.viewer.ghostByAnnotations) {
        distance = 0;
        for (auto key : keys)
            if (previous ? (key >= a.frame && key < frame) : (key <= a.frame && key > frame))
                ++distance;
    }
    if (distance < 1 || distance > limit)
        return {};
    result.opacity *= .45 * (1.0 - double(distance) / (limit + 1));
    if (p.viewer.ghostEnabled) {
        result.color = previous ? p.viewer.ghostPreviousTint : p.viewer.ghostNextTint;
        result.color.setAlpha(a.color.alpha());
    }
    return result;
}
} // namespace
QImage renderAnnotationLayer(const Project &p, const QString &owner, qint64 frame, QSize size) {
    if (size.isEmpty())
        return {};
    QImage result = layer(size);
    result.fill(p.viewer.annotationBackground);
    struct Group {
        bool background;
        qint64 frame;
        QString layer;
        QVector<const Annotation *> annotations;
    };
    QVector<Group> groups;
    QVector<qint64> keys;
    for (const auto &a : p.annotations) {
        if (!a.visible || (!a.timelineId.isEmpty() && a.timelineId != owner))
            continue;
        if (!a.background)
            keys.append(a.frame);
        const auto key = a.background ? qint64(0) : a.frame;
        auto group = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
            return g.background == a.background && g.frame == key && g.layer == a.layer;
        });
        if (group == groups.end()) {
            groups.append({a.background, key, a.layer, {&a}});
        } else {
            group->annotations.append(&a);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::stable_sort(groups.begin(), groups.end(),
                     [](const Group &a, const Group &b) { return a.background && !b.background; });
    QPainter target(&result);
    for (const auto &group : groups) {
        QImage groupImage;
        for (const auto *annotation : group.annotations) {
            const auto &a = *annotation;
            const bool erase = a.tool == AnnotationTool::Eraser;
            const auto style = appearance(a, p, frame, keys);
            if ((!erase && !style.visible) || (erase && groupImage.isNull()))
                continue;
            if (groupImage.isNull())
                groupImage = layer(size);
            QPainter painter(&groupImage);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(erase ? QColor(Qt::white) : style.color);
            painter.setOpacity(erase ? a.opacity : style.opacity);
            if (erase)
                painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            painter.drawPath(outline(a, size));
        }
        if (!groupImage.isNull())
            target.drawImage(0, 0, groupImage);
    }
    target.end();
    return result;
}
} // namespace openframe
