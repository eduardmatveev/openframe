#include "ViewerWidget.h"
#include "core/ReviewSession.h"
#include "core/ViewerGeometry.h"
#include "media/AnnotationRenderer.h"
#include "media/FrameComposer.h"
#include "TabletInput.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QScreen>
#include <QCursor>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QWheelEvent>
#include <QTimer>
#include <QFontMetrics>
#include <QTabletEvent>
#include <QPointingDevice>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
namespace openframe {
class InlineAnnotationText final : public QPlainTextEdit {
  public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void(bool, const QString &)> completed;
    void finish(bool commit) {
        if (m_finished)
            return;
        m_finished = true;
        if (completed)
            completed(commit, toPlainText());
        deleteLater();
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            event->accept();
        } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                   (event->modifiers() & Qt::ControlModifier)) {
            finish(true);
            event->accept();
        } else
            QPlainTextEdit::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent *event) override {
        QPlainTextEdit::focusOutEvent(event);
        finish(true);
    }

  private:
    bool m_finished = false;
};
ViewerWidget::ViewerWidget(ReviewSession *s, QWidget *parent) : QWidget(parent), m_session(s) {
    setObjectName("reviewViewer");
    setMinimumSize(320, 180);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setTabletTracking(true);
    setAcceptDrops(true);
    setToolTip("Drag: scrub frames · Ctrl+wheel: zoom · Ctrl+middle drag: pan · Double-click: fit");
    connect(s, &ReviewSession::imageReady, this, [this](const QImage &image) {
        m_image = image;
        if (m_transientTool == TransientTool::Eyedropper && m_sampleColor.isValid())
            updateSample(m_samplePosition);
        update();
    });
    m_laserTimer = new QTimer(this);
    m_laserTimer->setInterval(16);
    connect(m_laserTimer, &QTimer::timeout, this, [this] {
        if (m_laserClock.elapsed() >= m_laserFadeMs) {
            m_laserPoints.clear();
            m_laserTimer->stop();
        }
        update();
    });
    connect(s, &ReviewSession::projectChanged, this, [this] {
        if (!m_committingStroke && (m_drag == Drag::Draw || m_previewToken))
            cancelInteraction();
        update();
    });
    connect(s, &ReviewSession::frameChanged, this, [this] {
        if (m_drag == Drag::Draw && m_session->currentFrame() != m_anchorFrame)
            cancelInteraction();
    });
}
void ViewerWidget::setTabletOptions(bool pressure, double smoothing, const QString &backend) {
    cancelInteraction();
    m_pressureEnabled = pressure;
    m_smoothing = std::clamp(smoothing, 0.0, 1.0);
    m_tabletBackend = backend;
}
void ViewerWidget::setWipeOptions(bool snapping, bool guides) {
    m_wipeSnap = snapping;
    m_wipeGuides = guides;
    update();
}
void ViewerWidget::cancelInteraction() {
    const auto token = std::exchange(m_previewToken, 0);
    m_drag = Drag::None;
    m_draft = {};
    m_tabletContact = false;
    if (token)
        m_session->clearViewerPreview(token);
    update();
}
bool ViewerWidget::suppressMouse(QMouseEvent *event) const {
    return !m_dispatchingTablet &&
           (m_tabletContact || (event->source() != Qt::MouseEventNotSynthesized &&
                                m_tabletActivity.isValid() && m_tabletActivity.elapsed() < 250));
}
QPointF ViewerWidget::canvasScreenPoint(QPointF point, const ViewerSettings &settings) const {
    const auto normalized = canvasToViewer(settings, point);
    const auto r = imageRect();
    return {r.left() + normalized.x() * r.width(), r.top() + normalized.y() * r.height()};
}
QPointF ViewerWidget::wipeRotationHandle(const ViewerSettings &settings) const {
    const auto pivot = canvasScreenPoint(wipePivot(settings), settings);
    const auto angle = settings.wipeAngleDegrees * std::numbers::pi / 180;
    return pivot + QPointF(std::cos(angle), std::sin(angle)) * 42;
}
void ViewerWidget::setAnnotationTool(AnnotationTool tool) {
    m_tool = tool;
    setTransientTool(TransientTool::None);
    setCursor(m_annotationsEnabled ? Qt::CrossCursor : Qt::ArrowCursor);
}
void ViewerWidget::setAnnotationsEnabled(bool enabled) {
    cancelInteraction();
    if (!enabled)
        setTransientTool(TransientTool::None);
    m_annotationsEnabled = enabled;
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    m_drag = Drag::None;
    emit annotationContextChanged();
    update();
}
void ViewerWidget::setTransientTool(TransientTool tool) {
    cancelInteraction();
    m_transientTool = tool;
    m_drag = Drag::None;
    m_laserTimer->stop();
    m_laserPoints.clear();
    m_sampleColor = {};
    setCursor(tool != TransientTool::None || m_annotationsEnabled ? Qt::CrossCursor
                                                                  : Qt::ArrowCursor);
    emit transientToolChanged();
    update();
}
void ViewerWidget::setLaserSettings(QColor color, double width, int fadeMilliseconds) {
    if (color.isValid())
        m_laserColor = color;
    m_laserWidth = std::clamp(width, 1.0, 40.0);
    m_laserFadeMs = std::clamp(fadeMilliseconds, 50, 3000);
}
QColor ViewerWidget::framebufferColorAt(QPointF position) const {
    if (m_image.isNull() || !imageRect().contains(position))
        return {};
    QImage pixel(1, 1, QImage::Format_ARGB32_Premultiplied);
    pixel.fill(Qt::transparent);
    {
        QPainter painter(&pixel);
        const auto dpr = devicePixelRatioF();
        painter.translate(-std::floor(position.x() * dpr), -std::floor(position.y() * dpr));
        painter.scale(dpr, dpr);
        // Reuse the displayed raster path, including scaling and alpha compositing.
        // The one-pixel clip excludes the transient cursor/readout itself.
        paintFrame(painter);
    }
    return pixel.pixelColor(0, 0);
}
void ViewerWidget::updateSample(QPointF position) {
    m_samplePosition = position;
    m_sampleColor = framebufferColorAt(position);
    emit colorSampled(m_sampleColor);
    update();
}
void ViewerWidget::paintTransient(QPainter &p) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (!m_laserPoints.isEmpty()) {
        p.setClipRect(imageRect());
        const auto r = imageRect();
        QPainterPath path;
        for (int i = 0; i < m_laserPoints.size(); ++i) {
            const QPointF point(r.left() + r.width() * m_laserPoints[i].x(),
                                r.top() + r.height() * m_laserPoints[i].y());
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        const double opacity =
            m_laserTimer->isActive()
                ? std::max(0.0, 1.0 - double(m_laserClock.elapsed()) / m_laserFadeMs)
                : 1;
        p.setOpacity(opacity * .25);
        p.setPen(QPen(m_laserColor, m_laserWidth + 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.setOpacity(opacity);
        p.setPen(QPen(m_laserColor, m_laserWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        if (m_laserPoints.size() == 1)
            p.drawPoint(path.currentPosition());
    }
    p.restore();
    if (m_transientTool != TransientTool::Eyedropper || !m_sampleColor.isValid())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(m_samplePosition, 6, 6);
    p.setPen(QPen(Qt::white, 1));
    p.drawEllipse(m_samplePosition, 6, 6);
    const QString text = QString("RGB %1, %2, %3   %4")
                             .arg(m_sampleColor.red())
                             .arg(m_sampleColor.green())
                             .arg(m_sampleColor.blue())
                             .arg(m_sampleColor.name().toUpper());
    QFont f = font();
    f.setPointSize(10);
    p.setFont(f);
    const int labelWidth = std::min(width() - 12, QFontMetrics(f).horizontalAdvance(text) + 47);
    const QRectF box(
        std::clamp(m_samplePosition.x() + 16, 6.0, double(std::max(6, width() - labelWidth - 6))),
        std::clamp(m_samplePosition.y() + 18, 6.0, double(std::max(6, height() - 38))), labelWidth,
        30);
    p.setPen(QColor("#77818b"));
    p.setBrush(QColor(24, 26, 30, 245));
    p.drawRoundedRect(box, 3, 3);
    p.fillRect(box.adjusted(6, 6, -box.width() + 24, -6), m_sampleColor);
    p.setPen(Qt::white);
    p.drawText(box.adjusted(31, 0, -5, 0), Qt::AlignVCenter, text);
    p.restore();
}
void ViewerWidget::setPreviewImage(const QImage &image) {
    m_image = image;
    update();
}
QRectF ViewerWidget::imageRect() const {
    QSizeF sz =
        m_image.isNull() ? QSizeF(m_session->project().viewer.outputSize) : QSizeF(m_image.size());
    if (sz.isEmpty())
        sz = QSizeF(960, 540);
    sz.scale(QSizeF(width(), height()), Qt::KeepAspectRatio);
    return QRectF((width() - sz.width()) / 2, (height() - sz.height()) / 2, sz.width(),
                  sz.height());
}
QPointF ViewerWidget::normalized(QPointF p) const {
    auto r = imageRect();
    return QPointF(std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0),
                   std::clamp((p.y() - r.top()) / r.height(), 0.0, 1.0));
}
QPointF ViewerWidget::annotationPoint(QPointF position) const {
    const auto point = viewerToCanvas(m_session->project().viewer, normalized(position));
    return {std::clamp((point.x() - m_draftPaneRect.left()) / m_draftPaneRect.width(), 0.0, 1.0),
            std::clamp((point.y() - m_draftPaneRect.top()) / m_draftPaneRect.height(), 0.0, 1.0)};
}
void ViewerWidget::selectPaneAt(QPointF position) {
    if (!imageRect().contains(position))
        return;
    auto viewer = m_session->project().viewer;
    const auto canvas = viewerToCanvas(viewer, normalized(position));
    if (!QRectF(0, 0, 1, 1).contains(canvas))
        return;
    const int pane = viewerPaneAt(viewer, canvas);
    if (pane >= 0 && pane != viewer.activePane) {
        viewer.activePane = pane;
        m_session->setViewerSettings(viewer);
        emit annotationContextChanged();
    }
}
void ViewerWidget::paintFrame(QPainter &p) const {
    p.fillRect(rect(), QColor("#1b1d20"));
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    auto r = imageRect();
    if (!m_image.isNull())
        p.drawImage(r, m_image);
    else {
        p.setPen(QColor("#747a83"));
        QFont f = font();
        f.setPointSize(22);
        f.setWeight(QFont::Light);
        p.setFont(f);
        p.drawText(rect().adjusted(0, -25, 0, -25), Qt::AlignCenter, "OPENFRAME");
        f.setPointSize(10);
        p.setFont(f);
        p.setPen(QColor("#90969f"));
        p.drawText(rect().adjusted(0, 45, 0, 45), Qt::AlignCenter,
                   "Drop media here or press Ctrl+I to import");
    }
    const auto settings = m_session->displayedViewerSettings();
    if (settings.mode == ViewerMode::Wipe && !m_image.isNull() && m_wipeGuides) {
        const auto line = wipeDivider(settings, m_image.size());
        auto screenPoint = [&](QPointF point) {
            return canvasScreenPoint({point.x() / m_image.width(), point.y() / m_image.height()},
                                     settings);
        };
        p.save();
        p.setClipRect(r);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(220, 225, 230, 170), 1));
        p.drawLine(screenPoint(line.p1()), screenPoint(line.p2()));
        const auto pivot = canvasScreenPoint(wipePivot(settings), settings);
        const auto handle = wipeRotationHandle(settings);
        p.setPen(QPen(QColor("#8ccce7"), 1, Qt::DashLine));
        p.drawLine(pivot, handle);
        p.setPen(QPen(QColor("#e4edf2"), 1.5));
        p.setBrush(QColor("#243541"));
        p.drawEllipse(pivot, 5, 5);
        p.drawEllipse(handle, 5, 5);
        p.restore();
    }
}
void ViewerWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    paintFrame(painter);
    paintTransient(painter);
}
void ViewerWidget::mousePressEvent(QMouseEvent *e) {
    if (suppressMouse(e)) {
        e->accept();
        return;
    }
    setFocus();
    if (m_transientTool == TransientTool::Eyedropper) {
        updateSample(e->position());
        if (e->button() == Qt::LeftButton && m_sampleColor.isValid()) {
            m_color = m_sampleColor;
            emit annotationColorPicked(m_color);
        } else if (e->button() == Qt::RightButton)
            setTransientTool(TransientTool::None);
        return;
    }
    if (m_transientTool == TransientTool::Laser) {
        if (e->button() == Qt::LeftButton && imageRect().contains(e->position())) {
            m_laserTimer->stop();
            m_laserPoints = {normalized(e->position())};
            m_drag = Drag::Laser;
            update();
        } else if (e->button() == Qt::RightButton)
            setTransientTool(TransientTool::None);
        return;
    }
    m_anchor = e->position();
    m_anchorFrame = m_session->currentFrame();
    const bool wipeGesture = m_session->project().viewer.mode == ViewerMode::Wipe &&
                             (!m_annotationsEnabled || (e->modifiers() & Qt::AltModifier));
    if (!wipeGesture)
        selectPaneAt(e->position());
    auto v = m_session->project().viewer;
    const int pane = m_dragPane = activeViewerPane(v);
    m_anchorPanZoom = v.panZoom;
    if (e->button() == Qt::RightButton) {
        emit contextMenuRequested(e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::MiddleButton && (e->modifiers() & Qt::ControlModifier) &&
        m_panZoomEnabled) {
        m_drag = (e->modifiers() & Qt::ShiftModifier) ? Drag::Zoom : Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (wipeGesture && !m_image.isNull()) {
        const auto pivot = canvasScreenPoint(wipePivot(v), v);
        const auto normal = wipeRotationHandle(v) - pivot;
        const auto delta = e->position() - pivot;
        const double dividerDistance = std::abs(QPointF::dotProduct(delta, normal) / 42);
        if (QLineF(e->position(), wipeRotationHandle(v)).length() < 10)
            m_drag = Drag::WipeRotate;
        else if (QLineF(e->position(), pivot).length() < 10)
            m_drag = Drag::WipePivot;
        else if (dividerDistance < 8)
            m_drag = Drag::Wipe;
        if (m_drag == Drag::Wipe || m_drag == Drag::WipeRotate || m_drag == Drag::WipePivot) {
            m_anchorViewer = v;
            m_anchorCanvas = viewerToCanvas(v, normalized(e->position()));
            m_previewToken = m_session->beginViewerPreview();
            return;
        }
    }
    if (m_annotationsEnabled && !m_image.isNull()) {
        if (!imageRect().contains(e->position()))
            return;
        if (!QRectF(0, 0, 1, 1).contains(viewerToCanvas(v, normalized(e->position()))))
            return;
        m_draftPaneRect = viewerPaneRect(v, pane);
        m_session->pause();
        m_draft = {};
        m_draft.id = newId();
        m_draft.timelineId = displayedAnnotationOwner(m_session->project());
        m_draft.frame = displayedAnnotationFrame(m_session->project(), m_anchorFrame);
        m_draft.endFrame = m_draft.frame;
        m_draft.color = m_color;
        m_draft.width = m_width;
        m_draft.opacity = m_opacity;
        m_draft.background = m_background;
        m_draft.layer = m_background ? "Background" : "Foreground";
        m_draft.hold = m_hold;
        m_draft.ghostFrames = m_ghost;
        m_draft.tool = m_tool;
        m_strokePressure = m_dispatchingTablet && m_tabletHasPressure && m_pressureEnabled &&
                           (m_tool == AnnotationTool::Pencil || m_tool == AnnotationTool::Eraser);
        m_smoothedPoint = annotationPoint(e->position());
        m_smoothedPressure = m_strokePressure ? m_tabletPressure : 1;
        m_draft.points.append(m_smoothedPoint);
        if (m_strokePressure)
            m_draft.widthScales.append(m_smoothedPressure);
        if (m_tool == AnnotationTool::Text) {
            m_draft.textPosition = annotationPoint(e->position());
            m_draft.fontSize = int(m_width * 7);
            if (m_textDialog) {
                QDialog dialog(this);
                dialog.setWindowTitle("Text Annotation");
                dialog.resize(std::max(240, int(width() * m_textDialogScale)),
                              std::max(160, int(height() * m_textDialogScale)));
                auto layout = new QVBoxLayout(&dialog);
                auto edit = new QPlainTextEdit(&dialog);
                layout->addWidget(edit);
                auto buttons =
                    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
                layout->addWidget(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                edit->setFocus();
                if (dialog.exec() == QDialog::Accepted && !edit->toPlainText().isEmpty()) {
                    m_draft.text = edit->toPlainText();
                    m_session->addAnnotation(m_draft);
                }
            } else {
                auto edit = new InlineAnnotationText(this);
                edit->setPlaceholderText("Type text · Ctrl+Enter to finish · Esc to cancel");
                int editWidth = std::min(360, width() - 20),
                    editHeight = std::min(160, height() - 20);
                edit->setGeometry(
                    std::clamp(int(e->position().x()), 0, std::max(0, width() - editWidth)),
                    std::clamp(int(e->position().y()), 0, std::max(0, height() - editHeight)),
                    editWidth, editHeight);
                auto annotation = m_draft;
                edit->completed = [this, annotation](bool commit, const QString &text) mutable {
                    if (commit && !text.isEmpty()) {
                        annotation.text = text;
                        m_session->addAnnotation(annotation);
                    }
                };
                edit->show();
                edit->setFocus();
            }
            return;
        }
        m_drag = Drag::Draw;
        m_previewToken = m_session->beginViewerPreview();
        if (!m_session->previewAnnotation(m_previewToken, m_draft))
            cancelInteraction();
        update();
        return;
    }
    if (m_pauseOnScrub)
        m_session->pause();
    m_drag = Drag::Scrub;
    m_scrubAccum = 0;
    m_lastScrubPosition = e->position();
    setCursor(Qt::SizeHorCursor);
}
void ViewerWidget::mouseMoveEvent(QMouseEvent *e) {
    if (suppressMouse(e)) {
        e->accept();
        return;
    }
    if (m_transientTool == TransientTool::Eyedropper) {
        updateSample(e->position());
        return;
    }
    if (m_transientTool == TransientTool::Laser) {
        if (m_drag == Drag::Laser) {
            if (m_laserPoints.size() >= 4096)
                m_laserPoints.remove(0, 1024);
            m_laserPoints.append(normalized(e->position()));
            update();
        }
        return;
    }
    auto delta = e->position() - m_anchor;
    auto v = m_session->project().viewer;
    if (m_drag == Drag::Scrub) {
        double sensitivity = (e->modifiers() & Qt::ShiftModifier) ? m_shiftScrubPixels
                             : (e->modifiers() & Qt::AltModifier) ? m_altScrubPixels
                                                                  : m_scrubPixels;
        m_scrubAccum += (e->position().x() - m_lastScrubPosition.x()) / std::max(.1, sensitivity);
        m_lastScrubPosition = e->position();
        m_session->navigate(m_anchorFrame + qint64(m_scrubAccum), m_audioOnScrub);
        if (m_endlessScrub) {
            auto bounds = screen()->geometry();
            auto global = e->globalPosition().toPoint();
            int wrapped = global.x();
            if (global.x() <= bounds.left() + 1)
                wrapped = bounds.right() - 2;
            else if (global.x() >= bounds.right() - 1)
                wrapped = bounds.left() + 2;
            if (wrapped != global.x()) {
                QPoint point(wrapped, global.y());
                QCursor::setPos(point);
                m_lastScrubPosition = mapFromGlobal(point);
            }
        }
    } else if (m_drag == Drag::Pan) {
        auto r = imageRect();
        v.panZoom.enabled = true;
        v.panZoom.pan =
            m_anchorPanZoom.pan + QPointF(delta.x() / r.width(), delta.y() / r.height());
        m_session->setViewerSettings(v);
        emit transformChanged();
    } else if (m_drag == Drag::Zoom) {
        v.panZoom.enabled = true;
        v.panZoom.zoom = std::clamp(m_anchorPanZoom.zoom * std::exp(delta.x() / 180), .01, 64.0);
        m_session->setViewerSettings(v);
        emit transformChanged();
    } else if (m_drag == Drag::Wipe || m_drag == Drag::WipePivot || m_drag == Drag::WipeRotate) {
        v = m_anchorViewer;
        const auto canvas = viewerToCanvas(v, normalized(e->position()));
        const bool snap = m_wipeSnap && !(e->modifiers() & Qt::ControlModifier);
        if (m_drag == Drag::WipeRotate) {
            const auto relative = canvas - wipePivot(v);
            double angle =
                std::atan2(relative.y() * m_image.height(), relative.x() * m_image.width()) * 180 /
                std::numbers::pi;
            if (angle < 0)
                angle += 360;
            if (snap && std::abs(angle - std::round(angle / 90) * 90) < 4)
                angle = std::round(angle / 90) * 90;
            v.wipeAngleDegrees = std::fmod(angle, 360.0);
        } else {
            auto shift = canvas - m_anchorCanvas;
            if (m_drag == Drag::Wipe) {
                const double angle = v.wipeAngleDegrees * std::numbers::pi / 180;
                const QPointF normal(std::cos(angle), std::sin(angle));
                const double projection = QPointF::dotProduct(
                    {shift.x() * m_image.width(), shift.y() * m_image.height()}, normal);
                shift = {normal.x() * projection / m_image.width(),
                         normal.y() * projection / m_image.height()};
            }
            v.wipe = std::clamp(v.wipe + shift.x(), 0.0, 1.0);
            v.wipePivotY = std::clamp(v.wipePivotY + shift.y(), 0.0, 1.0);
            const double zoom = v.panZoom.enabled ? v.panZoom.zoom : 1;
            if (snap && std::abs(v.wipe - .5) * imageRect().width() * zoom < 6)
                v.wipe = .5;
            if (snap && std::abs(v.wipePivotY - .5) * imageRect().height() * zoom < 6)
                v.wipePivotY = .5;
        }
        if (!m_session->previewViewerSettings(m_previewToken, v))
            cancelInteraction();
        update();
    } else if (m_drag == Drag::Draw) {
        auto pt = annotationPoint(e->position());
        if (m_draft.tool == AnnotationTool::Pencil || m_draft.tool == AnnotationTool::Eraser)
            appendStrokePoint(e->position());
        else {
            if (m_draft.points.size() == 1)
                m_draft.points.append(pt);
            else
                m_draft.points.last() = pt;
            if (!m_session->previewAnnotation(m_previewToken, m_draft))
                cancelInteraction();
        }
        update();
    }
}
void ViewerWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (suppressMouse(event)) {
        event->accept();
        return;
    }
    if (m_drag == Drag::Laser) {
        m_laserClock.restart();
        m_laserTimer->start();
    }
    if (m_drag == Drag::Draw) {
        if (m_draft.tool == AnnotationTool::Pencil || m_draft.tool == AnnotationTool::Eraser)
            appendStrokePoint(event->position(), true);
        if (m_drag == Drag::Draw)
            finishAnnotation();
    }
    if (m_drag == Drag::Wipe || m_drag == Drag::WipePivot || m_drag == Drag::WipeRotate) {
        const auto token = std::exchange(m_previewToken, 0);
        m_drag = Drag::None;
        const auto settings = m_session->displayedViewerSettings();
        if (settings.wipe != m_anchorViewer.wipe ||
            settings.wipePivotY != m_anchorViewer.wipePivotY ||
            settings.wipeAngleDegrees != m_anchorViewer.wipeAngleDegrees)
            m_session->commitViewerPreview(token, "Adjust wipe");
        else
            m_session->clearViewerPreview(token);
    }
    m_drag = Drag::None;
    setCursor(m_transientTool != TransientTool::None || m_annotationsEnabled ? Qt::CrossCursor
                                                                             : Qt::ArrowCursor);
    update();
}
void ViewerWidget::finishAnnotation() {
    bool changed = true;
    if (m_draft.tool == AnnotationTool::Eraser) {
        auto after = m_session->project();
        after.annotations.append(m_draft);
        changed = renderAnnotationLayer(after, m_draft.timelineId, m_draft.frame, m_image.size()) !=
                  renderAnnotationLayer(m_session->project(), m_draft.timelineId, m_draft.frame,
                                        m_image.size());
    }
    m_committingStroke = true;
    if (changed)
        m_session->addAnnotation(m_draft);
    m_committingStroke = false;
    if (const auto token = std::exchange(m_previewToken, 0))
        m_session->clearViewerPreview(token);
    m_draft = {};
}
void ViewerWidget::appendStrokePoint(QPointF position, bool finalPoint) {
    if (m_draft.points.size() >= 65536)
        return;
    const auto raw = annotationPoint(position);
    const double alpha = finalPoint ? 1.0 : 1.0 / (1.0 + m_smoothing * 8);
    m_smoothedPoint += (raw - m_smoothedPoint) * alpha;
    m_smoothedPressure += ((m_strokePressure ? m_tabletPressure : 1) - m_smoothedPressure) * alpha;
    m_draft.points.append(m_smoothedPoint);
    if (m_strokePressure)
        m_draft.widthScales.append(std::clamp(m_smoothedPressure, 0.0, 1.0));
    if (!m_session->previewAnnotation(m_previewToken, m_draft))
        cancelInteraction();
}
void ViewerWidget::clearCurrentAnnotations() {
    auto tl = displayedAnnotationOwner(m_session->project());
    auto f = displayedAnnotationFrame(m_session->project(), m_session->currentFrame());
    auto bg = m_background;
    m_session->mutateAnnotations("Clear annotations", tl, f, bg, [=](Project &p) {
        p.annotations.removeIf([=](const Annotation &a) {
            return a.timelineId == tl && (bg ? a.background : !a.background && a.frame == f);
        });
    });
}
void ViewerWidget::mouseDoubleClickEvent(QMouseEvent *e) {
    if (suppressMouse(e)) {
        e->accept();
        return;
    }
    if (m_transientTool != TransientTool::None)
        return;
    if (e->button() == Qt::LeftButton && !m_annotationsEnabled)
        resetTransform();
}
void ViewerWidget::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && (m_drag != Drag::None || m_previewToken)) {
        cancelInteraction();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_transientTool != TransientTool::None) {
        setTransientTool(TransientTool::None);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
void ViewerWidget::leaveEvent(QEvent *event) {
    if (m_transientTool == TransientTool::Eyedropper) {
        m_sampleColor = {};
        emit colorSampled({});
        update();
    }
    QWidget::leaveEvent(event);
}
void ViewerWidget::focusOutEvent(QFocusEvent *event) {
    if (m_drag != Drag::None || m_previewToken)
        cancelInteraction();
    QWidget::focusOutEvent(event);
}
void ViewerWidget::tabletEvent(QTabletEvent *event) {
    event->accept();
    m_tabletActivity.restart();
    if (!acceptsTabletDevice(event->pointingDevice(), m_tabletBackend))
        return;
    m_tabletHasPressure =
        event->pointingDevice()->capabilities().testFlag(QInputDevice::Capability::Pressure);
    if (event->type() != QEvent::TabletRelease)
        m_tabletPressure = m_tabletHasPressure && std::isfinite(event->pressure())
                               ? std::clamp(double(event->pressure()), 0.0, 1.0)
                               : 1.0;
    const bool eraser =
        event->pointingDevice()->pointerType() == QPointingDevice::PointerType::Eraser;
    emit tabletDiagnosticChanged(
        QString("%1 · %2 · pressure %3%4")
            .arg(event->pointingDevice()->name(), eraser ? "eraser" : "pen")
            .arg(m_tabletPressure, 0, 'f', 3)
            .arg(m_tabletHasPressure ? "" : " (unavailable; fixed width)"));
    QEvent::Type mouseType = QEvent::MouseMove;
    if (event->type() == QEvent::TabletPress)
        mouseType = QEvent::MouseButtonPress;
    if (event->type() == QEvent::TabletRelease)
        mouseType = QEvent::MouseButtonRelease;
    QMouseEvent mouse(mouseType, event->position(), event->globalPosition(), event->button(),
                      event->buttons(), event->modifiers());
    const auto selectedTool = m_tool;
    if (eraser)
        m_tool = AnnotationTool::Eraser;
    m_dispatchingTablet = true;
    if (event->type() == QEvent::TabletPress) {
        m_tabletContact = true;
        mousePressEvent(&mouse);
    } else if (event->type() == QEvent::TabletRelease) {
        mouseReleaseEvent(&mouse);
        m_tabletContact = false;
    } else if (m_tabletContact || m_transientTool == TransientTool::Eyedropper) {
        mouseMoveEvent(&mouse);
    }
    m_dispatchingTablet = false;
    m_tool = selectedTool;
}
void ViewerWidget::changeZoom(double value) {
    auto v = m_session->project().viewer;
    v.panZoom.enabled = true;
    v.panZoom.zoom = std::clamp(v.panZoom.zoom * value, .01, 64.0);
    m_session->setViewerSettings(v);
    emit transformChanged();
}
void ViewerWidget::resetTransform() {
    auto v = m_session->project().viewer;
    v.panZoom = {};
    m_session->setViewerSettings(v);
    emit transformChanged();
}
void ViewerWidget::wheelEvent(QWheelEvent *e) {
    if ((e->modifiers() & Qt::ControlModifier) && m_panZoomEnabled) {
        selectPaneAt(e->position());
        changeZoom(std::pow(1.15, e->angleDelta().y() / 120.0));
    } else
        m_session->step(e->angleDelta().y() > 0 ? -1 : 1);
    e->accept();
}
void ViewerWidget::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void ViewerWidget::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (auto &url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty())
        emit filesDropped(paths);
    e->acceptProposedAction();
}
void ViewerWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    update();
}
} // namespace openframe
