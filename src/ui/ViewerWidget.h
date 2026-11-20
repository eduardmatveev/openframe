#pragma once
#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
#include "core/Project.h"
class QTimer;
class QPainter;
class QTabletEvent;
namespace openframe {
class ReviewSession;
class ViewerWidget final : public QWidget {
    Q_OBJECT
  public:
    enum class TransientTool { None, Eyedropper, Laser };
    explicit ViewerWidget(ReviewSession *session, QWidget *parent = nullptr);
    void setAnnotationTool(AnnotationTool tool);
    void setAnnotationsEnabled(bool enabled);
    bool annotationsEnabled() const {
        return m_annotationsEnabled;
    }
    void setAnnotationColor(QColor color) {
        m_color = color;
    }
    QColor annotationColor() const {
        return m_color;
    }
    AnnotationTool annotationTool() const {
        return m_tool;
    }
    void setTransientTool(TransientTool);
    TransientTool transientTool() const {
        return m_transientTool;
    }
    QColor sampledColor() const {
        return m_sampleColor;
    }
    QColor framebufferColorAt(QPointF position) const;
    void setLaserSettings(QColor color, double width, int fadeMilliseconds);
    void setTabletOptions(bool pressure, double smoothing, const QString &backend);
    void setWipeOptions(bool snapping, bool guides);
    void cancelInteraction();
    bool laserStrokeVisible() const {
        return !m_laserPoints.isEmpty();
    }
    void setAnnotationWidth(double width) {
        m_width = width;
    }
    void setAnnotationOpacity(double opacity) {
        m_opacity = opacity;
    }
    void setAnnotationBackground(bool background) {
        m_background = background;
        emit annotationContextChanged();
    }
    bool backgroundLayer() const {
        return m_background;
    }
    void setAnnotationHold(bool hold) {
        m_hold = hold;
    }
    void setAnnotationGhost(int frames) {
        m_ghost = frames;
    }
    void setScrubSensitivity(double pixels) {
        m_scrubPixels = pixels;
    }
    void setScrubModifiers(double shiftPixels, double altPixels) {
        m_shiftScrubPixels = shiftPixels;
        m_altScrubPixels = altPixels;
    }
    void setEndlessScrub(bool enabled) {
        m_endlessScrub = enabled;
    }
    void setAudioOnScrub(bool enabled) {
        m_audioOnScrub = enabled;
    }
    void setTextDialog(bool enabled, double scale) {
        m_textDialog = enabled;
        m_textDialogScale = scale;
    }
    void setPauseOnScrub(bool pause) {
        m_pauseOnScrub = pause;
    }
    void setPanZoomEnabled(bool enabled) {
        m_panZoomEnabled = enabled;
    }
    bool panZoomEnabled() const {
        return m_panZoomEnabled;
    }
    void setPreviewImage(const QImage &image);
    QRectF imageRect() const;
    void resetTransform();
    void changeZoom(double multiplier);
    void clearCurrentAnnotations();
  signals:
    void filesDropped(const QStringList &paths);
    void contextMenuRequested(QPoint globalPosition);
    void transformChanged();
    void annotationContextChanged();
    void transientToolChanged();
    void colorSampled(QColor color);
    void annotationColorPicked(QColor color);
    void tabletDiagnosticChanged(const QString &text);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
    void tabletEvent(QTabletEvent *) override;
    void focusOutEvent(QFocusEvent *) override;

  private:
    QPointF normalized(QPointF position) const;
    QPointF annotationPoint(QPointF position) const;
    void selectPaneAt(QPointF position);
    void finishAnnotation();
    void appendStrokePoint(QPointF position, bool finalPoint = false);
    bool suppressMouse(QMouseEvent *) const;
    QPointF canvasScreenPoint(QPointF normalized, const ViewerSettings &) const;
    QPointF wipeRotationHandle(const ViewerSettings &) const;
    void updateSample(QPointF);
    void paintFrame(QPainter &) const;
    void paintTransient(QPainter &);
    ReviewSession *m_session;
    QImage m_image;
    AnnotationTool m_tool = AnnotationTool::Pencil;
    QColor m_color{255, 204, 64};
    double m_width = 4.0, m_opacity = 1.0, m_scrubPixels = 8, m_shiftScrubPixels = 32,
           m_altScrubPixels = 2;
    double m_scrubAccum = 0, m_textDialogScale = .5;
    QPointF m_lastScrubPosition;
    bool m_endlessScrub = false, m_audioOnScrub = true, m_textDialog = true;
    bool m_annotationsEnabled = false, m_background = false, m_hold = false;
    bool m_pauseOnScrub = true, m_panZoomEnabled = true;
    int m_ghost = 0;
    enum class Drag { None, Scrub, Pan, Zoom, Wipe, WipePivot, WipeRotate, Draw, Laser };
    Drag m_drag = Drag::None;
    QPointF m_anchor;
    qint64 m_anchorFrame = 0;
    PanZoom m_anchorPanZoom;
    Annotation m_draft;
    int m_dragPane = 0;
    QRectF m_draftPaneRect{0, 0, 1, 1};
    TransientTool m_transientTool = TransientTool::None;
    QColor m_sampleColor, m_laserColor{255, 60, 60};
    QPointF m_samplePosition;
    QVector<QPointF> m_laserPoints;
    double m_laserWidth = 3;
    int m_laserFadeMs = 350;
    QElapsedTimer m_laserClock;
    QTimer *m_laserTimer;
    bool m_pressureEnabled = true, m_strokePressure = false, m_tabletContact = false;
    bool m_tabletHasPressure = false;
    bool m_dispatchingTablet = false, m_committingStroke = false;
    double m_smoothing = 0, m_tabletPressure = 1, m_smoothedPressure = 1;
    QPointF m_smoothedPoint;
    QString m_tabletBackend = "windows_ink";
    QElapsedTimer m_tabletActivity;
    bool m_wipeSnap = true, m_wipeGuides = true;
    quint64 m_previewToken = 0;
    ViewerSettings m_anchorViewer;
    QPointF m_anchorCanvas;
};
} // namespace openframe
