#include "FrameComposer.h"
#include "AnnotationRenderer.h"
#include "core/ViewerGeometry.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace openframe {
namespace {
class PainterState {
    QPainter &painter;
    bool active = true;

  public:
    explicit PainterState(QPainter &p) : painter(p) {
        painter.save();
    }
    ~PainterState() {
        restore();
    }
    void restore() {
        if (active) {
            painter.restore();
            active = false;
        }
    }
};
void pane(QPainter &painter, const QRectF &bounds, const QImage &image,
          const ViewTransform &transform) {
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    if (image.isNull()) {
        painter.setPen(QColor(130, 134, 145));
        painter.drawText(bounds, Qt::AlignCenter, "No video");
        painter.restore();
        return;
    }
    const auto rotated =
        transform.rotation % 180 != 0 ? QSize(image.height(), image.width()) : image.size();
    double scale = std::min(bounds.width() / rotated.width(), bounds.height() / rotated.height());
    painter.translate(bounds.center());
    painter.rotate(transform.rotation);
    painter.scale(scale * (transform.flipHorizontal ? -1 : 1),
                  scale * (transform.flipVertical ? -1 : 1));
    painter.drawImage(QPointF(-image.width() / 2.0, -image.height() / 2.0), image);
    painter.restore();
}
QImage assigned(const Project &p, const QString &assignment, qint64 frame, MediaEngine &engine) {
    auto e = evaluateAssignment(p, assignment, frame);
    if (!e)
        return {};
    auto s = p.source(e->sourceId);
    if (!s || s->missing)
        return {};
    auto native = engine.nativeFrame(*s, e->sourceFrame);
    engine.checkCancelled();
    auto image = ColorPipeline::displayRgba(native, p.viewer.color);
    engine.checkCancelled();
    return image;
}
void overlay(QPainter &painter, const Project &project, qint64 frame, const QSize &size) {
    auto &v = project.viewer;
    auto w = size.width(), h = size.height();
    painter.save();
    if (v.showOverlay || v.showPictureType) {
        QFont font("Segoe UI");
        font.setPixelSize(std::max(11, w / 80));
        painter.setFont(font);
        auto paneIndex = isSinglePaneMode(v.mode) ? activeViewerPane(v) : 0;
        auto owner = viewerPaneOwner(project, paneIndex);
        frame = viewerPaneFrame(project, frame, paneIndex);
        auto t = project.timeline(owner);
        auto source = project.source(owner);
        auto fps = source ? source->info.fps : t ? t->fps : project.fps;
        auto millis = (source ? source->info.frameStartUs(frame) : fps.frameToUs(frame)) / 1000;
        auto label = QString("%1  |  %2:%3:%4.%5  |  %6 fps")
                         .arg(displayFrameNumber(project, owner, frame))
                         .arg(millis / 3600000, 2, 10, QChar('0'))
                         .arg(millis / 60000 % 60, 2, 10, QChar('0'))
                         .arg(millis / 1000 % 60, 2, 10, QChar('0'))
                         .arg(millis % 1000, 3, 10, QChar('0'))
                         .arg(fps.value(), 0, 'f', 3);
        if (!v.showOverlay)
            label.clear();
        if (v.showPictureType) {
            const auto e = evaluateAssignment(project, owner, frame);
            const auto media = e ? project.source(e->sourceId) : nullptr;
            const auto type =
                media ? framePictureType(media->info, e->sourceFrame) : QString("Unknown");
            label += (label.isEmpty() ? QString{} : QString("  |  ")) + type;
        }
        QRectF rect(8, h - 32, std::min(w - 16, 600), 24);
        painter.fillRect(rect, QColor(0, 0, 0, 150));
        painter.setPen(Qt::white);
        painter.drawText(rect.adjusted(7, 0, -5, 0), Qt::AlignVCenter, label);
    }
    painter.restore();
}
void drawOverlay(QPainter &p, const OverlaySettings &v, QSize size, MediaEngine &engine) {
    if (v.type == OverlayType::Off)
        return;
    const double w = size.width(), h = size.height(), factor = w / 1920.0;
    const auto point = [&](QPointF a) { return QPointF(a.x() * w, a.y() * h); };
    PainterState state(p);
    p.setClipRect(QRectF(0, 0, w, h), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(v.opacity);
    p.setPen(QPen(v.color, std::max(.5, v.lineWidth * factor)));
    p.setBrush(Qt::NoBrush);
    if (v.type == OverlayType::SafeArea) {
        for (auto margin : {v.safeMargins, v.safeInnerMargins})
            p.drawRect(QRectF(w * margin.x(), h * margin.y(), w * (1 - 2 * margin.x()),
                              h * (1 - 2 * margin.y())));
    } else if (v.type == OverlayType::Grid) {
        for (int n = 1; n < v.gridColumns; ++n)
            p.drawLine(QPointF(w * n / v.gridColumns, 0), QPointF(w * n / v.gridColumns, h));
        for (int n = 1; n < v.gridRows; ++n)
            p.drawLine(QPointF(0, h * n / v.gridRows), QPointF(w, h * n / v.gridRows));
    } else if (v.type == OverlayType::Center) {
        const double radius = v.centerSize * factor / 2;
        p.drawLine(QPointF(w / 2 - radius, h / 2), QPointF(w / 2 + radius, h / 2));
        p.drawLine(QPointF(w / 2, h / 2 - radius), QPointF(w / 2, h / 2 + radius));
    } else if (v.type == OverlayType::PhiGrid) {
        const double phi = (1 + std::sqrt(5.0)) / 2;
        for (double t : {1 / (phi * phi), 1 / phi}) {
            p.drawLine(QPointF(w * t, 0), QPointF(w * t, h));
            p.drawLine(QPointF(0, h * t), QPointF(w, h * t));
        }
    } else if (v.type == OverlayType::DynamicSymmetry) {
        const QPointF a(0, 0), b(w, h), c(w, 0), d(0, h);
        p.drawLine(a, b);
        p.drawLine(c, d);
        const auto perpendicular = [&](QPointF corner, QPointF start, QPointF end) {
            auto delta = end - start;
            const auto t =
                QPointF::dotProduct(corner - start, delta) / QPointF::dotProduct(delta, delta);
            p.drawLine(corner, start + delta * t);
        };
        perpendicular(c, a, b);
        perpendicular(d, a, b);
        perpendicular(a, c, d);
        perpendicular(b, c, d);
    } else if (v.type == OverlayType::Image && !v.imagePath.isEmpty()) {
        const auto image = engine.overlayImage(v.imagePath);
        QRectF target(point(v.imageRect.topLeft()),
                      QSizeF(v.imageRect.width() * w, v.imageRect.height() * h));
        if (v.imageKeepAspect) {
            const auto fitted = QSizeF(image.size()).scaled(target.size(), Qt::KeepAspectRatio);
            target =
                QRectF(target.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
        }
        p.setOpacity(v.opacity * v.imageOpacity);
        p.drawImage(target, image);
    } else if (v.type == OverlayType::Custom) {
        for (const auto &s : v.shapes) {
            if (!s.visible || s.points.isEmpty())
                continue;
            p.setOpacity(v.opacity * s.opacity);
            p.setPen(QPen(s.color, std::max(.5, s.width * factor), Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
            p.setBrush(s.fill);
            QPolygonF points;
            for (const auto &a : s.points)
                points.append(point(a));
            if (s.type == OverlayShapeType::Text) {
                QFont font("Segoe UI");
                font.setPixelSize(std::max(1, int(s.fontSize * factor)));
                p.setFont(font);
                p.drawText(points.first(), s.text);
            } else if (s.type == OverlayShapeType::Polygon && points.size() > 2)
                p.drawPolygon(points);
            else if (points.size() > 1) {
                const auto rect = QRectF(points.first(), points.last()).normalized();
                if (s.type == OverlayShapeType::Rectangle)
                    p.drawRect(rect);
                else if (s.type == OverlayShapeType::Ellipse)
                    p.drawEllipse(rect);
                else
                    p.drawPolyline(points);
            } else
                p.drawPoint(points.first());
        }
    }
}
} // namespace
QImage FrameComposer::compose(const Project &p, qint64 frame, MediaEngine &engine,
                              const QSize &requested, bool annotations, ComparisonLayers *layers) {
    auto image = composeHighPrecision(p, frame, engine, requested, annotations, layers);
    // Source-over composition on an opaque canvas keeps every alpha at one.
    if (p.viewer.background.alphaF() == 1.0f)
        image.reinterpretAsFormat(QImage::Format_RGBA32FPx4);
    return ColorPipeline::quantize(image, 8);
}
QImage FrameComposer::composeHighPrecision(const Project &p, qint64 frame, MediaEngine &engine,
                                           const QSize &requested, bool annotations,
                                           ComparisonLayers *layers) {
    QSize size = requested.isValid() ? requested : p.viewer.outputSize;
    size = size.expandedTo({16, 16}).boundedTo({8192, 8192});
    QImage out(size, QImage::Format_RGBA32FPx4_Premultiplied);
    if (out.isNull())
        throw std::bad_alloc();
    out.fill(p.viewer.background);
    ColorPipeline::tagImage(out, sdrColorInfo(32));
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    auto &v = p.viewer;
    // Two float RGBA layers at UHD occupy about 253 MiB.
    if ((v.mode != ViewerMode::Blend && v.mode != ViewerMode::Wipe) ||
        qint64(size.width()) * size.height() > 8 * 1024 * 1024)
        layers = nullptr;
    PainterState canvasState(painter);
    if (v.panZoom.enabled) {
        painter.translate(size.width() * (.5 + v.panZoom.pan.x()),
                          size.height() * (.5 + v.panZoom.pan.y()));
        painter.scale(v.panZoom.zoom, v.panZoom.zoom);
        painter.translate(-size.width() * .5, -size.height() * .5);
    }
    auto transform = [&](int n) {
        return n < v.transforms.size() ? v.transforms[n] : ViewTransform{};
    };
    const int paneCount = v.mode == ViewerMode::Grid ? 4 : 2;
    for (int n = 0; n < paneCount; ++n)
        if (!viewerPaneRect(v, n).isEmpty() && (!layers || (*layers)[n].isNull())) {
            auto e = evaluateAssignment(p, viewerPaneOwner(p, n), frame);
            if (e)
                if (auto source = p.source(e->sourceId))
                    engine.prefetch(*source, {e->sourceFrame});
        }
    auto drawPane = [&](int n) {
        const auto rect = viewerPaneRect(v, n);
        const QRectF bounds(rect.x() * size.width(), rect.y() * size.height(),
                            rect.width() * size.width(), rect.height() * size.height());
        if (bounds.isEmpty())
            return;
        if (layers && !(*layers)[n].isNull()) {
            painter.drawImage(bounds, (*layers)[n]);
            return;
        }
        const auto owner = viewerPaneOwner(p, n);
        auto paintContents = [&](QPainter &target, const QSize &extent) {
            pane(target, QRectF(QPointF{}, QSizeF(extent)), assigned(p, owner, frame, engine),
                 transform(n));
            if (annotations && v.showAnnotations &&
                (!p.annotations.isEmpty() || v.annotationBackground.alphaF() > 0.0f)) {
                target.drawImage(0, 0, renderAnnotationLayer(p, owner, viewerPaneFrame(p, frame, n),
                                                            extent));
            }
        };
        // A single opaque pane at its native canvas size can use the output buffer directly.
        if (isSinglePaneMode(v.mode) && painter.worldTransform().isIdentity() &&
            v.background.alphaF() == 1.0f) {
            paintContents(painter, size);
            return;
        }
        // Keep each pane's picture and annotations together for comparison and transforms.
        QImage layer(bounds.size().toSize().expandedTo({1, 1}),
                     QImage::Format_RGBA32FPx4_Premultiplied);
        if (layer.isNull())
            throw std::bad_alloc();
        layer.fill(v.background);
        QPainter local(&layer);
        local.setRenderHint(QPainter::SmoothPixmapTransform);
        paintContents(local, layer.size());
        local.end();
        if (layers)
            (*layers)[n] = layer;
        painter.drawImage(bounds, layer);
    };
    if (isSinglePaneMode(v.mode)) {
        drawPane(activeViewerPane(v));
    } else if (v.mode == ViewerMode::Split) {
        drawPane(0);
        drawPane(1);
    } else if (v.mode == ViewerMode::Grid) {
        for (int n = 0; n < 4; ++n)
            drawPane(n);
    } else {
        drawPane(0);
        PainterState comparisonState(painter);
        if (v.mode == ViewerMode::Wipe) {
            QPainterPath clip;
            clip.addPolygon(wipeClipPolygon(v, size, 1));
            painter.setClipPath(clip, Qt::IntersectClip);
        } else
            painter.setOpacity(v.opacity);
        drawPane(1);
        comparisonState.restore();
        if (v.mode == ViewerMode::Wipe) {
            painter.setPen(QColor(225, 225, 230, 190));
            painter.drawLine(wipeDivider(v, size));
        }
    }
    drawOverlay(painter, v.overlay, size, engine);
    canvasState.restore();
    overlay(painter, p, frame, size);
    painter.end();
    return out;
}
void FrameComposer::paintOverlay(QImage &image, const OverlaySettings &settings) {
    ViewerSettings viewer;
    viewer.overlay = settings;
    QString error;
    if (!validateViewerSettings(viewer, &error))
        throw std::runtime_error(error.toStdString());
    MediaEngine engine(16);
    const auto original = image.format();
    auto precise = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    ColorPipeline::tagImage(precise, sdrColorInfo(32));
    {
        QPainter painter(&precise);
        drawOverlay(painter, settings, precise.size(), engine);
    }
    image =
        original == QImage::Format_RGBA32FPx4 || original == QImage::Format_RGBA32FPx4_Premultiplied
            ? precise
            : ColorPipeline::quantize(precise, image.depth() > 32 ? 16 : 8);
}
QImage FrameComposer::timeline(const Project &p, const QString &owner, qint64 frame,
                               MediaEngine &engine, const QSize &size, bool annotations) {
    return ColorPipeline::quantize(
        timelineHighPrecision(p, owner, frame, engine, size, annotations), 8);
}
QImage FrameComposer::timelineHighPrecision(const Project &p, const QString &owner, qint64 frame,
                                            MediaEngine &engine, const QSize &size,
                                            bool annotations, const VideoColorInfo *preserveColor) {
    QImage out(size, QImage::Format_RGBA32FPx4_Premultiplied);
    if (out.isNull())
        throw std::bad_alloc();
    out.fill(preserveColor ? Qt::transparent : Qt::black);
    const auto target = preserveColor ? *preserveColor : sdrColorInfo(32);
    ColorPipeline::tagImage(out, target);
    QImage sourceImage;
    auto evaluated = evaluateAssignment(p, owner, frame);
    if (evaluated)
        if (auto source = p.source(evaluated->sourceId); source && !source->missing) {
            auto decoded = engine.nativeFrame(*source, evaluated->sourceFrame);
            sourceImage = preserveColor
                              ? ColorPipeline::convertEncoded(ColorPipeline::encodedRgba(decoded),
                                                              decoded.color, target, p.viewer.color)
                              : ColorPipeline::displayRgba(decoded, p.viewer.color);
        }
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    pane(painter, {QPointF{}, QSizeF(size)}, sourceImage, {});
    painter.end();
    if (annotations) {
        auto layer =
            renderAnnotationLayer(p, owner.isEmpty() ? p.activeTimelineId : owner, frame, size);
        if (preserveColor)
            layer = ColorPipeline::convertEncoded(layer, sdrColorInfo(16), target, p.viewer.color);
        QPainter overlay(&out);
        overlay.drawImage(0, 0, layer);
    }
    return out;
}
void FrameComposer::paintAnnotations(QImage &image, const Project &p, const QString &owner,
                                     qint64 frame) {
    auto layer = renderAnnotationLayer(p, owner, frame, image.size());
    QPainter target(&image);
    target.drawImage(0, 0, layer);
}
} // namespace openframe
