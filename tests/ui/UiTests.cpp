#include <QtTest>
#include <QAction>
#include <QDir>
#include <QImage>
#include <QFile>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QStatusBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMenu>
#include <QJsonDocument>
#include <QTableWidget>
#include <QCheckBox>
#include <QDialog>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabletEvent>
#include <QPointingDevice>
#include <QColorSpace>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <memory>
#include <cmath>
#include "core/ReviewSession.h"
#include "media/MediaDecoder.h"
#include "ui/MainWindow.h"
#include "ui/ViewerWidget.h"
#include "ui/TimelineWidget.h"
#include "ui/Theme.h"
#include "ui/LocalFileDialog.h"
#include "ui/LocalFileDialog_p.h"
#include "ui/NotesCsvDialog.h"
#include "ui/OverlayDialog.h"
#include "ui/BookmarkFilter.h"
#include "ui/TabletInput.h"
#include "core/ViewerGeometry.h"
#include "media/AnnotationRenderer.h"
#include "integration/ApiServer.h"
#include "integration/LocalPathPolicy.h"
using namespace openframe;
// Observe normal QWidget paints without forcing a grab/repaint. The queued callback
// runs after the paint event returns; it is not a display-compositor/vblank timestamp.
class ViewerPaintProbe final : public QObject {
  public:
    struct Sample {
        qint64 frame, readyNs, paintedNs;
    };
    QElapsedTimer clock;
    QVector<Sample> samples;
    int paintEvents = 0;
    ViewerPaintProbe(ViewerWidget *viewer, ReviewSession *session) : QObject(viewer) {
        clock.start();
        viewer->installEventFilter(this);
        connect(session, &ReviewSession::imageReady, this, [this, session] {
            m_frame = session->audioDiagnostics().value("renderedFrame").toInteger(-1);
            m_readyNs = clock.nsecsElapsed();
            ++m_serial;
        });
    }

  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            ++paintEvents;
            if (m_frame >= 0 && m_serial != m_recordedSerial) {
                m_recordedSerial = m_serial;
                const auto frame = m_frame, ready = m_readyNs;
                QTimer::singleShot(0, this, [this, frame, ready] {
                    samples.append({frame, ready, clock.nsecsElapsed()});
                });
            }
        }
        return false;
    }

  private:
    qint64 m_frame = -1, m_readyNs = 0;
    quint64 m_serial = 0, m_recordedSerial = 0;
};
class UiTests final : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir m_temp;
    std::unique_ptr<ReviewSession> m_session;
    std::unique_ptr<MainWindow> m_window;
    QAction *command(const QString &id) {
        for (auto a : m_window->commandActions())
            if (a->objectName() == id)
                return a;
        return nullptr;
    }
    void pointer(QEvent::Type type, QPointF point, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                 Qt::MouseEventSource source = Qt::MouseEventNotSynthesized) {
        auto viewer = m_window->viewer();
        QMouseEvent event(type, point, point, viewer->mapToGlobal(point.toPoint()),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          modifiers, source);
        QApplication::sendEvent(viewer, &event);
    }
    void tablet(QEvent::Type type, const QPointingDevice &device, QPointF point, double pressure) {
        auto viewer = m_window->viewer();
        QTabletEvent event(type, &device, point, viewer->mapToGlobal(point.toPoint()), pressure, 0,
                           0, 0, 0, 0, Qt::NoModifier,
                           type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                           type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        QApplication::sendEvent(viewer, &event);
        QVERIFY(event.isAccepted());
    }
  private slots:
    void initTestCase() {
        // Qt event-driven tests exercise the explicit fallback, including on the
        // Windows platform. They do not automate or validate Windows Shell UI.
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setOrganizationName("OpenFrameTests");
        QCoreApplication::setApplicationName("UiTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_temp.path());
        applyDarkTheme(*qobject_cast<QApplication *>(QCoreApplication::instance()));
    }
    void init() {
        QSettings().clear();
        m_session = std::make_unique<ReviewSession>();
        m_window = std::make_unique<MainWindow>(m_session.get());
        m_window->setSmokeMode(true);
        m_window->show();
        QImage image(160, 90, QImage::Format_RGB32);
        image.fill(QColor("#3e5363"));
        QString path = QDir(m_temp.path()).filePath("synthetic.png");
        QVERIFY(image.save(path));
        QStringList frames;
        for (int i = 0; i < 24; ++i)
            frames << path;
        m_session->importImageSequence(frames, {24, 1});
        QTRY_COMPARE_WITH_TIMEOUT(m_session->project().sources.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_session->displayImage().isNull(), 10000);
        QCOMPARE(m_session->durationFrames(), 24);
    }
    void cleanup() {
        m_session->pause();
        m_window.reset();
        m_session.reset();
        QCoreApplication::processEvents();
    }
    void layoutIconsMatchPaneArrangement() {
        auto horizontal = command("split");
        auto vertical = command("splitVertical");
        auto wipe = command("wipe");
        QVERIFY(horizontal && vertical && wipe);
        auto menuImage = [](QAction *action) {
            return action->icon().pixmap(QSize(16, 16), 1.0).toImage();
        };
        const auto horizontalIcon = menuImage(horizontal);
        const auto verticalIcon = menuImage(vertical);
        const auto wipeIcon = menuImage(wipe);
        QCOMPARE(horizontalIcon.size(), QSize(16, 16));
        QCOMPARE(verticalIcon.size(), horizontalIcon.size());
        QCOMPARE(wipeIcon.size(), horizontalIcon.size());
        // Sample the interior, away from the common outline, at actual menu size.
        QVERIFY(horizontalIcon.pixelColor(6, 8).alpha() > horizontalIcon.pixelColor(8, 6).alpha());
        QVERIFY(verticalIcon.pixelColor(8, 6).alpha() > verticalIcon.pixelColor(6, 8).alpha());
        QVERIFY(wipeIcon.pixelColor(9, 5).alpha() > wipeIcon.pixelColor(5, 5).alpha());
        QVERIFY(wipeIcon.pixelColor(6, 10).alpha() > wipeIcon.pixelColor(11, 10).alpha());

        horizontal->trigger();
        QCOMPARE(viewerPaneRect(m_session->project().viewer, 1), QRectF(.5, 0, .5, 1));
        vertical->trigger();
        QCOMPARE(viewerPaneRect(m_session->project().viewer, 1), QRectF(0, .5, 1, .5));
        wipe->trigger();
        QCOMPARE(m_session->project().viewer.mode, ViewerMode::Wipe);

        auto singleB = command("singleB");
        auto active = command("ab");
        QVERIFY(singleB && active);
        QCOMPARE(active->text(), QString("Active"));
        singleB->trigger();
        QCOMPARE(activeViewerPane(m_session->project().viewer), 1);
        command("rotateCW")->trigger();
        QCOMPARE(m_session->project().viewer.transforms[1].rotation, 90);
        QCOMPARE(m_session->project().viewer.transforms[0].rotation, 0);
        active->trigger();
        QCOMPARE(activeViewerPane(m_session->project().viewer), 0);
    }
    void transparencyUpdatesBeforeSliderRelease() {
        const QString sourceA = m_session->project().sources.first().id;
        const QString path = m_temp.filePath("white-comparison.bmp");
        QImage white(1920, 1080, QImage::Format_RGB32);
        white.fill(Qt::white);
        QVERIFY(white.save(path));
        m_session->importImageSequence({path}, {24, 1});
        QTRY_COMPARE_WITH_TIMEOUT(m_session->project().sources.size(), 2, 10000);
        const QString sourceB = m_session->project().sources.last().id;
        m_session->assignViewer(0, sourceA);
        m_session->assignViewer(1, sourceB);
        auto settings = m_session->project().viewer;
        settings.mode = ViewerMode::Blend;
        settings.opacity = 0;
        settings.wipe = .31;
        settings.showOverlay = false;
        m_session->setViewerSettings(settings);
        auto slider = m_window->findChild<QSlider *>("comparisonMix");
        QVERIFY(slider && slider->isEnabled());
        auto red = [&] { return m_session->displayImage().pixelColor(960, 540).red(); };
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(red() - 62) <= 1, 5000);
        const int history = m_session->undoStack()->index();
        slider->setSliderDown(true);
        slider->setValue(25);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(red() - 110) <= 2, 5000);
        slider->setValue(75);
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(red() - 207) <= 2, 5000);
        QVERIFY(slider->isSliderDown());
        QCOMPARE(m_session->project().viewer.opacity, 0.0);
        QCOMPARE(m_session->undoStack()->index(), history);
        slider->setSliderDown(false);
        QCOMPARE(m_session->undoStack()->index(), history + 1);
        QCOMPARE(m_session->project().viewer.opacity, .75);
        QCOMPARE(m_session->project().viewer.wipe, .31);
        m_session->undoStack()->undo();
        QCOMPARE(m_session->project().viewer.opacity, 0.0);
    }
    void commandsOperateOnSession() {
        QVERIFY(command("stepForward"));
        command("stepForward")->trigger();
        QCOMPARE(m_session->currentFrame(), 1);
        command("multiForward")->trigger();
        QCOMPARE(m_session->currentFrame(), 11);
        QCOMPARE(m_session->project().rangeOut, -1);
        QVERIFY(command("rangeStart")->isEnabled());
        command("rangeStart")->trigger();
        QCOMPARE(m_session->project().rangeIn, 11);
        QCOMPARE(m_session->project().rangeOut, 23);
        command("end")->trigger();
        QCOMPARE(m_session->currentFrame(), 23);
        command("rangeEnd")->trigger();
        QCOMPARE(m_session->project().rangeOut, 23);
        command("rangeReset")->trigger();
        QCOMPARE(m_session->project().rangeIn, 0);
        command("grid")->trigger();
        QCOMPARE(m_session->project().viewer.mode, ViewerMode::Grid);
        command("splitVertical")->trigger();
        QCOMPARE(viewerPaneRect(m_session->project().viewer, 1), QRectF(0, .5, 1, .5));
        command("overlayGrid")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::Grid);
        command("toggleOverlay")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::Off);
        command("rotateCW")->trigger();
        QCOMPARE(m_session->project().viewer.transforms[0].rotation, 90);
        command("resetFlip")->trigger();
        QCOMPARE(m_session->project().viewer.transforms[0].rotation, 0);
    }
    void scrubbingAvoidsDuplicateMovesAndKeepsReleaseTarget() {
        auto timeline = m_window->findChild<TimelineWidget *>("globalTimeControl");
        QVERIFY(timeline);
        timeline->setAudioOnScrub(false);
        auto send = [&](QEvent::Type type, double frame) {
            const QPointF point(10 + frame / 23 * (timeline->width() - 20), 8);
            QMouseEvent event(type, point, timeline->mapToGlobal(point.toPoint()),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(timeline, &event);
        };
        QSignalSpy changed(m_session.get(), &ReviewSession::frameChanged);
        send(QEvent::MouseButtonPress, 8);
        QCOMPARE(m_session->currentFrame(), 8);
        const int afterPress = changed.count();
        QVERIFY(afterPress > 0);
        for (int i = 0; i < 100; ++i)
            send(QEvent::MouseMove, 8 + .001 * i);
        QCOMPARE(changed.count(), afterPress);
        send(QEvent::MouseButtonRelease, 13);
        QCOMPARE(m_session->currentFrame(), 13);
        auto viewer = m_window->viewer();
        viewer->setAudioOnScrub(false);
        viewer->setScrubSensitivity(10);
        const auto start = viewer->imageRect().center();
        pointer(QEvent::MouseButtonPress, start);
        const int beforeMoves = changed.count();
        for (int i = 0; i < 8; ++i)
            pointer(QEvent::MouseMove, start + QPointF(i, 0));
        QCOMPARE(changed.count(), beforeMoves);
        pointer(QEvent::MouseButtonRelease, start + QPointF(30, 0));
        QCOMPARE(m_session->currentFrame(), 16);
        send(QEvent::MouseButtonPress, 8);
        send(QEvent::MouseButtonRelease, -5);
        QCOMPARE(m_session->currentFrame(), 0);
        send(QEvent::MouseButtonPress, 8);
        send(QEvent::MouseButtonRelease, 30);
        QCOMPARE(m_session->currentFrame(), 23);
        m_session->setRange(4, 12);
        m_session->setLockStepScrubToRange(true);
        send(QEvent::MouseButtonPress, 8);
        send(QEvent::MouseButtonRelease, 22);
        QCOMPARE(m_session->currentFrame(), 12);
    }
    void sliderDragReachesNormalViewerPaint() {
        const auto userProject = qEnvironmentVariable("OPENFRAME_UI_SCRUB_PROJECT");
        if (userProject.isEmpty()) {
            const auto imagePath = m_session->project().sources.first().sequencePaths.first();
            QStringList paths;
            for (int i = 0; i < 96; ++i)
                paths.append(imagePath);
            m_session->newProject();
            m_session->importImageSequence(paths, {24, 1});
            QTRY_COMPARE_WITH_TIMEOUT(m_session->durationFrames(), 96, 10000);
        } else {
            QVERIFY(m_session->openProject(userProject));
        }
        m_session->setAudioEnabled(false);
        m_session->seek(0, false);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->audioDiagnostics().value("renderedFrame").toInteger(),
                                  0, 10000);
        const qint64 lastFrame = m_session->durationFrames() - 1;
        QVERIFY(lastFrame >= 10);
        auto timeline = m_window->findChild<TimelineWidget *>("globalTimeControl");
        QVERIFY(timeline);
        timeline->setAudioOnScrub(false);
        m_session->setAudioEnabled(false);
        QCoreApplication::processEvents();
        ViewerPaintProbe probe(m_window->viewer(), m_session.get());
        QMap<qint64, qint64> requested;
        qint64 previousFrame = -1;
        QVector<double> handlers;
        auto send = [&](QEvent::Type type, double frame) {
            const QPointF point(10 + frame / lastFrame * (timeline->width() - 20), 8);
            const auto target = qint64(std::llround(frame));
            if (target != previousFrame) {
                requested[target] = probe.clock.nsecsElapsed();
                previousFrame = target;
            }
            QMouseEvent event(type, point, timeline->mapToGlobal(point.toPoint()),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QElapsedTimer handler;
            handler.start();
            QApplication::sendEvent(timeline, &event);
            handlers.append(handler.nsecsElapsed() / 1000000.0);
        };
        send(QEvent::MouseButtonPress, 0);
        for (int i = 1; i <= 94; ++i) {
            send(QEvent::MouseMove, double(lastFrame) * i / 95);
            QTest::qWait(4);
        }
        const auto releaseNs = probe.clock.nsecsElapsed();
        send(QEvent::MouseButtonRelease, lastFrame);
        QCOMPARE(m_session->currentFrame(), lastFrame);
        auto finalPainted = [&] {
            return std::any_of(probe.samples.cbegin(), probe.samples.cend(), [&](const auto &s) {
                return s.frame == lastFrame && s.paintedNs >= releaseNs;
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(finalPainted(), 5000);
        QVector<double> latencies, readyToPaint;
        QSet<qint64> paintedDuringDrag;
        double releaseMs = -1;
        for (const auto &sample : probe.samples) {
            if (requested.contains(sample.frame) && sample.paintedNs >= requested[sample.frame])
                latencies.append((sample.paintedNs - requested[sample.frame]) / 1000000.0);
            readyToPaint.append((sample.paintedNs - sample.readyNs) / 1000000.0);
            if (sample.paintedNs < releaseNs)
                paintedDuringDrag.insert(sample.frame);
            if (sample.frame == lastFrame && sample.paintedNs >= releaseNs && releaseMs < 0)
                releaseMs = (sample.paintedNs - releaseNs) / 1000000.0;
        }
        auto percentile = [](QVector<double> values, double fraction) {
            if (values.isEmpty())
                return -1.0;
            std::sort(values.begin(), values.end());
            return values[std::min<qsizetype>(values.size() - 1,
                                              qsizetype(std::ceil(values.size() * fraction)) - 1)];
        };
        QJsonObject report{
            {"fixture", userProject.isEmpty() ? "own 96-frame PNG sequence"
                                              : "authorized user project (read only)"},
            {"measurement",
             "slider QMouseEvent to queued completion after normal Viewer QEvent::Paint; no grab"},
            {"platform", QGuiApplication::platformName()},
            {"devicePixelRatio", m_window->devicePixelRatioF()},
            {"mouseMoves", 94},
            {"paintEvents", probe.paintEvents},
            {"distinctFramesPaintedDuringDrag", paintedDuringDrag.size()},
            {"requestToPaintP50Ms", percentile(latencies, .5)},
            {"requestToPaintP95Ms", percentile(latencies, .95)},
            {"imageReadyToPaintP95Ms", percentile(readyToPaint, .95)},
            {"mouseHandlerP95Ms", percentile(handlers, .95)},
            {"releaseToFinalPaintMs", releaseMs},
            {"finalPaintedFrame", lastFrame}};
        qInfo().noquote() << "SCRUB_UI " << QJsonDocument(report).toJson(QJsonDocument::Compact);
        QVERIFY2(paintedDuringDrag.size() >= 8, "Video only updated after the drag stopped");
        QVERIFY(releaseMs >= 0 && releaseMs < 1000);
    }
    void annotationRendererPreservesPrecisionAndPressure() {
        Project project;
        Annotation stroke;
        stroke.id = newId();
        stroke.timelineId = "owner";
        stroke.points = {{.1, .5}, {.9, .5}};
        stroke.width = 100;
        stroke.widthScales = {.2, 1};
        stroke.color = Qt::red;
        project.annotations = {stroke};
        const QSize size(384, 216);
        auto image = renderAnnotationLayer(project, "owner", 0, size);
        QCOMPARE(image.format(), QImage::Format_RGBA64_Premultiplied);
        QCOMPARE(image.colorSpace(), QColorSpace(QColorSpace::SRgb));
        auto thickness = [&](const QImage &value, int x) {
            int count = 0;
            for (int y = 0; y < value.height(); ++y)
                if (value.pixelColor(x, y).alphaF() > .5)
                    ++count;
            return count;
        };
        QVERIFY(thickness(image, 300) > thickness(image, 70) + 8);
        project.annotations[0].widthScales = {0, 0};
        auto empty = renderAnnotationLayer(project, "owner", 0, size);
        QCOMPARE(thickness(empty, 70), 0);
        QCOMPARE(thickness(empty, 300), 0);
        stroke.widthScales = {1, 1};
        stroke.opacity = .5;
        project.annotations = {stroke};
        const auto once = renderAnnotationLayer(project, "owner", 0, size);
        stroke.points.append(stroke.points.first());
        stroke.widthScales.append(1);
        project.annotations = {stroke};
        const auto retraced = renderAnnotationLayer(project, "owner", 0, size);
        QCOMPARE(once.pixelColor(192, 108), retraced.pixelColor(192, 108));
        QVERIFY(std::abs(once.pixelColor(192, 108).alphaF() - .5) < .001);
        project.annotations[0].points = {{.5, .5}};
        project.annotations[0].widthScales = {1};
        QVERIFY(renderAnnotationLayer(project, "owner", 0, size).pixelColor(192, 108).alpha() > 0);
    }
    void eraserIsIsolatedToOwnerLayerAndKeyframe() {
        Project project;
        Annotation bg;
        bg.id = newId();
        bg.timelineId = "owner";
        bg.background = true;
        bg.points = {{.1, .5}, {.9, .5}};
        bg.width = 140;
        bg.color = Qt::blue;
        Annotation fg = bg;
        fg.id = newId();
        fg.background = false;
        fg.width = 100;
        fg.color = Qt::red;
        Annotation erase = fg;
        erase.id = newId();
        erase.tool = AnnotationTool::Eraser;
        erase.points = {{.5, .1}, {.5, .9}};
        erase.width = 180;
        project.annotations = {bg, fg, erase};
        const QSize size(384, 216);
        auto image = renderAnnotationLayer(project, "owner", 0, size);
        QCOMPARE(image.pixelColor(192, 108), QColor(Qt::blue));
        QCOMPARE(image.pixelColor(90, 108), QColor(Qt::red));
        project.annotations[2].frame = project.annotations[2].endFrame = 1;
        QCOMPARE(renderAnnotationLayer(project, "owner", 0, size).pixelColor(192, 108),
                 QColor(Qt::red));
        project.annotations[2].frame = project.annotations[2].endFrame = 0;
        project.annotations[2].timelineId = "other";
        QCOMPARE(renderAnnotationLayer(project, "owner", 0, size).pixelColor(192, 108),
                 QColor(Qt::red));
        project.annotations[2].timelineId = "owner";
        project.annotations[2].layer = "Separate named layer";
        QCOMPARE(renderAnnotationLayer(project, "owner", 0, size).pixelColor(192, 108),
                 QColor(Qt::red));
        project.annotations[2].layer = fg.layer;
        project.viewer.holdAnnotations = true;
        QCOMPARE(renderAnnotationLayer(project, "owner", 5, size).pixelColor(192, 108),
                 QColor(Qt::blue));
        project.viewer.holdAnnotations = false;
        project.viewer.ghostEnabled = true;
        project.viewer.ghostPrevious = 3;
        QCOMPARE(renderAnnotationLayer(project, "owner", 1, size).pixelColor(192, 108),
                 QColor(Qt::blue));
        QVERIFY(renderAnnotationLayer(project, "owner", 1, size).pixelColor(90, 108) !=
                QColor(Qt::blue));
    }
    void tabletPressureEraserAndSyntheticMouseDeduplication() {
        auto viewer = m_window->viewer();
        viewer->setAnnotationsEnabled(true);
        viewer->setTabletOptions(true, 0, "windows_ink");
        viewer->setAnnotationWidth(150);
        const auto r = viewer->imageRect();
        const QPointF a(r.left() + r.width() * .2, r.center().y());
        const QPointF b(r.left() + r.width() * .8, r.center().y());
        QPointingDevice pen(
            "wmpointer", 801, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        const auto original = projectToJson(m_session->project());
        const QPoint sample(m_session->displayImage().width() / 2,
                            m_session->displayImage().height() / 2);
        const auto baseColor = m_session->displayImage().pixelColor(sample);
        tablet(QEvent::TabletPress, pen, a, .2);
        tablet(QEvent::TabletMove, pen, b, .9);
        QTRY_VERIFY_WITH_TIMEOUT(m_session->displayImage().pixelColor(sample) != baseColor, 3000);
        QCOMPARE(projectToJson(m_session->project()), original);
        pointer(QEvent::MouseButtonPress, a, Qt::NoModifier, Qt::MouseEventSynthesizedBySystem);
        pointer(QEvent::MouseButtonRelease, b, Qt::NoModifier, Qt::MouseEventSynthesizedBySystem);
        tablet(QEvent::TabletRelease, pen, b, 0);
        QCOMPARE(m_session->project().annotations.size(), 1);
        const auto drawn = m_session->project().annotations.first();
        QCOMPARE(drawn.widthScales.size(), drawn.points.size());
        QCOMPARE(drawn.widthScales.first(), .2);
        QCOMPARE(drawn.widthScales.last(), .9);
        pointer(QEvent::MouseButtonPress, a, Qt::NoModifier, Qt::MouseEventSynthesizedByQt);
        pointer(QEvent::MouseButtonRelease, b, Qt::NoModifier, Qt::MouseEventSynthesizedByQt);
        QCOMPARE(m_session->project().annotations.size(), 1);
        QPointingDevice eraser(
            "wmpointer", 802, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        tablet(QEvent::TabletPress, eraser, r.center() - QPointF(0, 40), 1);
        tablet(QEvent::TabletMove, eraser, r.center() + QPointF(0, 40), 1);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->displayImage().pixelColor(sample), baseColor, 3000);
        QCOMPARE(m_session->project().annotations.size(), 1);
        tablet(QEvent::TabletRelease, eraser, r.center() + QPointF(0, 40), 0);
        QCOMPARE(m_session->project().annotations.size(), 2);
        QCOMPARE(m_session->project().annotations.last().tool, AnnotationTool::Eraser);
        QCOMPARE(viewer->annotationTool(), AnnotationTool::Pencil);
        auto history = m_session->annotationUndoStack(m_session->activeOwnerId(), 0);
        history->undo();
        QCOMPARE(m_session->project().annotations.size(), 1);
        history->redo();
        QCOMPARE(m_session->project().annotations.size(), 2);
        auto path = QDir(m_temp.path()).filePath("pressure-eraser.oframe");
        QVERIFY(m_session->saveProject(path));
        QVERIFY(m_session->openProject(path));
        QCOMPARE(m_session->project().annotations.first().widthScales, drawn.widthScales);
        QCOMPARE(m_session->project().annotations.last().tool, AnnotationTool::Eraser);
    }
    void tabletFallbackSmoothingCancellationAndBackendRouting() {
        auto viewer = m_window->viewer();
        viewer->setAnnotationsEnabled(true);
        viewer->setTabletOptions(true, 1, "windows_ink");
        auto r = viewer->imageRect();
        const QPointF start = r.center() - QPointF(100, 0);
        QPointingDevice pen("synthetic-no-pressure", 803, QInputDevice::DeviceType::Stylus,
                            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position,
                            1, 3);
        tablet(QEvent::TabletPress, pen, start, 0);
        tablet(QEvent::TabletMove, pen, start + QPointF(20, 50), 0);
        tablet(QEvent::TabletMove, pen, start + QPointF(40, -50), 0);
        tablet(QEvent::TabletRelease, pen, start + QPointF(60, 0), 0);
        QCOMPARE(m_session->project().annotations.size(), 1);
        const auto baked = m_session->project().annotations.first();
        QVERIFY(baked.widthScales.isEmpty());
        QVERIFY(std::abs(baked.points[1].y() - baked.points[0].y()) < 10 / r.height());
        QVERIFY(std::abs(baked.points.last().x() - ((start.x() + 60 - r.left()) / r.width())) <
                .0001);
        viewer->setTabletOptions(false, 0, "windows_ink");
        QCOMPARE(m_session->project().annotations.first().points, baked.points);
        const auto history = m_session->annotationUndoStack(m_session->activeOwnerId(), 0)->index();
        tablet(QEvent::TabletPress, pen, start, .5);
        tablet(QEvent::TabletMove, pen, start + QPointF(40, 20), .8);
        QTest::keyClick(viewer, Qt::Key_Escape);
        tablet(QEvent::TabletRelease, pen, start + QPointF(40, 20), 0);
        QCOMPARE(m_session->project().annotations.size(), 1);
        QCOMPARE(m_session->annotationUndoStack(m_session->activeOwnerId(), 0)->index(), history);
        QPointingDevice foreign("wintab", 804, QInputDevice::DeviceType::Stylus,
                                QPointingDevice::PointerType::Pen,
                                QInputDevice::Capability::Position, 1, 3);
        tablet(QEvent::TabletPress, foreign, start, .5);
        tablet(QEvent::TabletMove, foreign, start + QPointF(20, 0), .8);
        tablet(QEvent::TabletRelease, foreign, start + QPointF(20, 0), 0);
        QCOMPARE(m_session->project().annotations.size(), 1);
        QString error;
        QVERIFY(!applyTabletBackend("invalid", &error));
        QVERIFY(!error.isEmpty());
        if (!tabletBackendSwitchSupported()) {
            QVERIFY(!applyTabletBackend("wintab", &error));
            QCOMPARE(activeTabletBackend(), QString("qt"));
        } else {
            const auto previous = activeTabletBackend();
            const bool enabled = applyTabletBackend("wintab", &error);
            if (enabled)
                QCOMPARE(activeTabletBackend(), QString("wintab"));
            else {
                QCOMPARE(activeTabletBackend(), previous);
                QVERIFY(!error.isEmpty());
            }
            QVERIFY(applyTabletBackend(previous, &error));
            qInfo() << "WinTab driver initialized:" << enabled << "(physical pen not exercised)";
        }
    }
    void wipeGeometryMatchesNonSquareCanvas() {
        ViewerSettings settings;
        settings.wipe = .23;
        settings.wipePivotY = .71;
        const QSizeF canvas(640, 360);
        for (double angle : {0., 37., 45., 90., 180., 270.}) {
            settings.wipeAngleDegrees = angle;
            auto line = wipeDivider(settings, canvas);
            QVERIFY(line.length() > 0);
            for (auto point :
                 {QPointF(.05, .05), QPointF(.9, .1), QPointF(.1, .9), QPointF(.9, .9)}) {
                const double radians = angle * std::acos(-1.) / 180;
                const double side = std::cos(radians) * (point.x() - .23) * 640 +
                                    std::sin(radians) * (point.y() - .71) * 360;
                const int expected = side <= 0 ? 0 : 1;
                QCOMPARE(wipePaneAt(settings, point, canvas), expected);
                QVERIFY(wipeClipPolygon(settings, canvas, expected)
                            .containsPoint({point.x() * 640, point.y() * 360}, Qt::OddEvenFill));
                QVERIFY(!wipeClipPolygon(settings, canvas, 1 - expected)
                             .containsPoint({point.x() * 640, point.y() * 360}, Qt::OddEvenFill));
            }
        }
    }
    void wipeGesturesPreviewCommitCancelAndExternalInvalidation() {
        auto viewer = m_window->viewer();
        auto settings = m_session->project().viewer;
        settings.mode = ViewerMode::Wipe;
        settings.panZoom = {};
        settings.wipe = .5;
        settings.wipePivotY = .5;
        settings.wipeAngleDegrees = 0;
        m_session->setViewerSettings(settings);
        QTest::qWait(30);
        const auto r = viewer->imageRect();
        const auto before = projectToJson(m_session->project());
        const auto history = m_session->undoStack()->index();
        pointer(QEvent::MouseButtonPress, r.center());
        pointer(QEvent::MouseButtonRelease, r.center());
        QCOMPARE(m_session->undoStack()->index(), history);
        pointer(QEvent::MouseButtonPress, r.center());
        pointer(QEvent::MouseMove, r.center() + QPointF(60, 30));
        QCOMPARE(projectToJson(m_session->project()), before);
        QVERIFY(m_session->displayedViewerSettings().wipe > .5);
        QTest::keyClick(viewer, Qt::Key_Escape);
        pointer(QEvent::MouseButtonRelease, r.center() + QPointF(60, 30));
        QCOMPARE(projectToJson(m_session->project()), before);
        QCOMPARE(m_session->undoStack()->index(), history);
        pointer(QEvent::MouseButtonPress, r.center() + QPointF(42, 0));
        pointer(QEvent::MouseMove, r.center() + QPointF(0, 55));
        QCOMPARE(m_session->project().viewer.wipeAngleDegrees, 0.);
        QCOMPARE(m_session->displayedViewerSettings().wipeAngleDegrees, 90.);
        pointer(QEvent::MouseButtonRelease, r.center() + QPointF(0, 55));
        QCOMPARE(m_session->project().viewer.wipeAngleDegrees, 90.);
        QCOMPARE(m_session->undoStack()->index(), history + 1);
        m_session->undoStack()->undo();
        QCOMPARE(m_session->project().viewer.wipeAngleDegrees, 0.);
        pointer(QEvent::MouseButtonPress, r.center());
        pointer(QEvent::MouseMove, r.center() + QPointF(35, 10));
        m_session->addNote("External mutation cancels the drag");
        const auto external = projectToJson(m_session->project());
        pointer(QEvent::MouseButtonRelease, r.center() + QPointF(90, 20));
        QCOMPARE(projectToJson(m_session->project()), external);
        QCOMPARE(m_session->project().viewer.wipe, .5);
        settings = m_session->project().viewer;
        settings.panZoom = {2, {.1, -.05}, true};
        m_session->setViewerSettings(settings);
        auto pivot = canvasToViewer(settings, {.5, .5});
        const QPointF transformedPivot(r.left() + pivot.x() * r.width(),
                                       r.top() + pivot.y() * r.height());
        pointer(QEvent::MouseButtonPress, transformedPivot);
        pointer(QEvent::MouseMove, transformedPivot + QPointF(40, 20));
        QVERIFY(std::abs(m_session->displayedViewerSettings().wipe - (.5 + 40 / (r.width() * 2))) <
                .00001);
        QVERIFY(std::abs(m_session->displayedViewerSettings().wipePivotY -
                         (.5 + 20 / (r.height() * 2))) < .00001);
        const auto beforeFocusLoss = projectToJson(m_session->project());
        QFocusEvent focusOut(QEvent::FocusOut);
        QApplication::sendEvent(viewer, &focusOut);
        pointer(QEvent::MouseButtonRelease, transformedPivot + QPointF(40, 20));
        QCOMPARE(projectToJson(m_session->project()), beforeFocusLoss);
    }
    void newDialogsCancelAndApplyActualSettings() {
        for (bool accept : {false, true}) {
            const auto original = projectToJson(m_session->project());
            bool observed = false;
            QTimer::singleShot(0, m_window.get(), [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto angle = dialog->findChild<QDoubleSpinBox *>("wipeAngle");
                auto y = dialog->findChild<QDoubleSpinBox *>("wipePivotY");
                if (angle && y) {
                    angle->setValue(37);
                    y->setValue(71);
                    observed = projectToJson(m_session->project()) == original &&
                               m_session->displayedViewerSettings().wipeAngleDegrees == 37;
                }
                accept ? dialog->accept() : dialog->reject();
            });
            command("wipeOptions")->trigger();
            QVERIFY(observed);
            if (accept) {
                QCOMPARE(m_session->project().viewer.wipeAngleDegrees, 37.);
                QCOMPARE(m_session->project().viewer.wipePivotY, .71);
            } else
                QCOMPARE(projectToJson(m_session->project()), original);
        }
        const auto originalColor = m_session->project().viewer.color;
        bool observed = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto peak = dialog->findChild<QDoubleSpinBox *>("sourcePeakNits");
            if (peak) {
                peak->setValue(2000);
                observed = m_session->project().viewer.color == originalColor &&
                           m_session->displayedViewerSettings().color.sourcePeakNits == 2000;
            }
            dialog->reject();
        });
        command("displayColor")->trigger();
        QVERIFY(observed);
        QVERIFY(m_session->project().viewer.color == originalColor);
        command("pictureType")->trigger();
        QVERIFY(m_session->project().viewer.showPictureType);
        const auto prefs = QSettings().value("annotation/smoothing");
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto smoothing = dialog->findChild<QSpinBox *>("annotation/smoothing"))
                smoothing->setValue(75);
            dialog->reject();
        });
        command("preferences")->trigger();
        QCOMPARE(QSettings().value("annotation/smoothing"), prefs);
    }
    void exportColorControlsAdvertiseSupportedPaths() {
        const auto original = projectToJson(m_session->project());
        bool checked = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto color = dialog->findChild<QComboBox *>("exportColorMode");
            auto depth = dialog->findChild<QComboBox *>("exportBitDepth");
            auto input = dialog->findChild<QComboBox *>("exportInput");
            auto codec = dialog->findChild<QComboBox *>("exportCodec");
            auto annotations = dialog->findChild<QCheckBox *>("exportIncludeAnnotations");
            auto kind = dialog->findChild<QComboBox *>("exportKind");
            auto box = dialog->findChild<QDialogButtonBox *>();
            if (color && depth && input && codec && kind && box && annotations) {
                color->setCurrentIndex(1);
                bool blockedViewer = !box->button(QDialogButtonBox::Save)->isEnabled();
                input->setCurrentIndex(1);
                codec->setCurrentIndex(codec->findData("ffv1"));
                depth->setCurrentIndex(depth->findData(10));
                bool allowedFfv1 = box->button(QDialogButtonBox::Save)->isEnabled() &&
                                   annotations->isEnabled() && annotations->isChecked();
                annotations->setChecked(false);
                bool explicitOff = !annotations->isChecked();
                input->setCurrentIndex(0);
                input->setCurrentIndex(1);
                explicitOff &= !annotations->isChecked();
                kind->setCurrentIndex(2);
                bool blockedPng10 = !box->button(QDialogButtonBox::Save)->isEnabled();
                depth->setCurrentIndex(depth->findData(16));
                checked = blockedViewer && allowedFfv1 && explicitOff && blockedPng10 &&
                          box->button(QDialogButtonBox::Save)->isEnabled();
            }
            dialog->reject();
        });
        command("export")->trigger();
        QVERIFY(checked);
        QCOMPARE(projectToJson(m_session->project()), original);
        QVERIFY(!m_session->exporting());
    }
    void mayaTimeoutPreferenceAppliesAndCancelPreservesIt() {
        ReviewSession session;
        ApiServer server(&session);
        QSettings().setValue("maya/timeoutSeconds", 2.5);
        MainWindow window(&session, &server);
        window.setSmokeMode(true);
        QCOMPARE(server.defaultMayaTimeout(), 2.5);
        QAction *preferences = nullptr;
        for (auto action : window.commandActions())
            if (action->objectName() == "preferences")
                preferences = action;
        QVERIFY(preferences);
        for (bool accept : {false, true}) {
            bool observed = false;
            QTimer::singleShot(0, &window, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto timeout = dialog->findChild<QDoubleSpinBox *>("maya/timeoutSeconds");
                if (timeout) {
                    timeout->setValue(4.25);
                    observed = true;
                }
                accept ? dialog->accept() : dialog->reject();
            });
            preferences->trigger();
            QVERIFY(observed);
            QCOMPARE(server.defaultMayaTimeout(), accept ? 4.25 : 2.5);
            QCOMPARE(QSettings().value("maya/timeoutSeconds").toDouble(), accept ? 4.25 : 2.5);
        }
        server.stop();
    }
    void keyboardAndBookmarks() {
        auto viewer = m_window->viewer();
        viewer->setFocus();
        QTest::keyClick(viewer, Qt::Key_Right);
        QCOMPARE(m_session->currentFrame(), 1);
        QTest::keyClick(viewer, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(m_session->currentFrame(), 11);
        QTest::keyClick(viewer, Qt::Key_B);
        QCOMPARE(m_session->project().bookmarks.size(), 1);
        QCOMPARE(m_session->project().bookmarks.first().frame, 11);
        QTest::keyClick(viewer, Qt::Key_B);
        QCOMPARE(m_session->project().bookmarks.size(), 0);
        command("play")->trigger();
        QVERIFY(m_session->playing());
        command("pause")->trigger();
        QVERIFY(!m_session->playing());
    }
    void annotationUsesSharedProjectAndUndo() {
        auto viewer = m_window->viewer();
        command("annotationsTools")->trigger();
        QVERIFY(viewer->annotationsEnabled());
        auto r = viewer->imageRect();
        QPoint start = r.center().toPoint(), end = start + QPoint(70, 35);
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(viewer, end, 10);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(m_session->project().annotations.size(), 1);
        auto annotation = m_session->project().annotations.first();
        QVERIFY(annotation.points.size() >= 2);
        QCOMPARE(annotation.timelineId, m_session->project().activeTimelineId);
        QVERIFY(annotation.points.first().x() >= 0 && annotation.points.first().x() <= 1);
        command("undo")->trigger();
        QCOMPARE(m_session->project().annotations.size(), 0);
        command("redo")->trigger();
        QCOMPARE(m_session->project().annotations.size(), 1);
        viewer->clearCurrentAnnotations();
        QCOMPARE(m_session->project().annotations.size(), 0);
    }
    void comparisonAnnotationsFollowPaneOwnerAndCoordinates() {
        MediaSource other = m_session->project().sources.first();
        other.id = newId();
        other.name = "Synthetic short B";
        other.sequencePaths = other.sequencePaths.mid(0, 3);
        other.info.framePts = other.info.framePts.mid(0, 3);
        other.info.frameDurations = other.info.frameDurations.mid(0, 3);
        other.info.durationUs = other.info.fps.frameToUs(3);
        other.outFrame = 2;
        m_session->mutateProject("Add synthetic B",
                                 [other](Project &p) { p.sources.append(other); });
        m_session->assignViewer(1, other.id);
        m_session->assignViewer(3, other.id);
        m_session->seek(10, false);
        command("annotationsTools")->trigger();
        auto viewer = m_window->viewer();
        struct Case {
            ViewerMode mode;
            bool vertical;
            int pane;
            QPointF position;
        };
        const QVector<Case> cases{{ViewerMode::AB, true, 1, {.5, .5}},
                                  {ViewerMode::Split, true, 1, {.75, .5}},
                                  {ViewerMode::Split, false, 1, {.5, .75}},
                                  {ViewerMode::Grid, true, 3, {.75, .75}}};
        for (const auto &test : cases) {
            auto settings = m_session->project().viewer;
            settings.mode = test.mode;
            settings.splitVertical = test.vertical;
            settings.activePane = test.mode == ViewerMode::AB ? 1 : 0;
            m_session->setViewerSettings(settings);
            const auto rect = viewer->imageRect();
            const QPoint start(qRound(rect.left() + rect.width() * test.position.x()),
                               qRound(rect.top() + rect.height() * test.position.y()));
            const QPoint end = start + QPoint(20, 10);
            viewer->setAnnotationTool(AnnotationTool::Pencil);
            QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(viewer, end, 10);
            QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, end);
            QCOMPARE(m_session->project().viewer.activePane, test.pane);
            QCOMPARE(m_session->project().annotations.size(), 1);
            const auto annotation = m_session->project().annotations.first();
            QCOMPARE(annotation.timelineId, other.id);
            QCOMPARE(annotation.frame, 2);
            QCOMPARE(m_session->currentFrame(), 10);
            QVERIFY(std::abs(annotation.points.first().x() - .5) < .01);
            QVERIFY(std::abs(annotation.points.first().y() - .5) < .01);
            command("undo")->trigger();
            QCOMPARE(m_session->project().annotations.size(), 0);
            command("redo")->trigger();
            QCOMPARE(m_session->project().annotations.size(), 1);
            viewer->clearCurrentAnnotations();
            QCOMPARE(m_session->project().annotations.size(), 0);
        }
    }
    void annotationHistoryIsScopedAndPreservesNotes() {
        command("annotationsTools")->trigger();
        auto owner = m_session->activeOwnerId();
        Annotation a;
        a.id = newId();
        a.timelineId = owner;
        a.frame = 0;
        a.endFrame = 0;
        a.points = {{.1, .1}, {.2, .2}};
        m_session->addAnnotation(a);
        m_session->seek(5, false);
        a.id = newId();
        a.frame = 5;
        a.endFrame = 5;
        m_session->addAnnotation(a);
        m_session->addNote("Keep this note");
        QCOMPARE(m_session->project().annotations.size(), 2);
        command("undo")->trigger();
        QCOMPARE(m_session->project().annotations.size(), 1);
        QCOMPARE(m_session->project().annotations.first().frame, 0);
        QCOMPARE(m_session->project().notes.size(), 1);
        command("redo")->trigger();
        QCOMPARE(m_session->project().annotations.size(), 2);
        command("annotationsTools")->trigger();
        command("undo")->trigger();
        QCOMPARE(m_session->project().notes.size(), 0);
        QCOMPARE(m_session->project().annotations.size(), 2);
    }
    void noteLabelsAppearInFrameCell() {
        Note note;
        note.id = newId();
        note.timelineId = m_session->activeOwnerId();
        note.frame = 3;
        note.text = "Colored review label";
        note.color = QColor("#9b5138");
        m_session->mutateProject("Add labeled note", [=](Project &p) { p.notes.append(note); });
        auto table = m_window->findChild<QTableWidget *>("notesTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->background().color(), note.color);
        QCOMPARE(table->item(0, 0)->text(), QString("4"));
    }
    void panZoomAndReset() {
        auto viewer = m_window->viewer();
        viewer->changeZoom(2);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 2.0);
        command("resetView")->trigger();
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.0);
        command("flipHorizontal")->trigger();
        QVERIFY(m_session->project().viewer.transforms[0].flipHorizontal);
        command("resetFlip")->trigger();
        QVERIFY(!m_session->project().viewer.transforms[0].flipHorizontal);
    }
    void globalPanZoomKeepsPaneCoordinatesAndOwnerState() {
        const auto owner = m_session->activeOwnerId();
        auto second = m_session->project().sources.first();
        second.id = newId();
        m_session->mutateProject("Second synthetic source",
                                 [=](Project &p) { p.sources.append(second); });
        m_session->assignViewer(1, second.id);
        auto settings = m_session->project().viewer;
        settings.mode = ViewerMode::Split;
        settings.splitVertical = true;
        settings.panZoom = {1.5, {-.1, .05}, true};
        m_session->setViewerSettings(settings);
        auto viewer = m_window->viewer();
        command("annotationsTools")->trigger();
        const auto transformed = canvasToViewer(settings, {.7, .4});
        const auto area = viewer->imageRect();
        const QPoint point(qRound(area.left() + transformed.x() * area.width()),
                           qRound(area.top() + transformed.y() * area.height()));
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(viewer, point + QPoint(8, 4));
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, point + QPoint(8, 4));
        QCOMPARE(m_session->project().viewer.activePane, 1);
        QCOMPARE(m_session->project().annotations.size(), 1);
        const auto annotation = m_session->project().annotations.first();
        QCOMPARE(annotation.timelineId, second.id);
        QVERIFY(std::abs(annotation.points.first().x() - .4) < .01);
        QVERIFY(std::abs(annotation.points.first().y() - .4) < .01);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.5);
        command("togglePan")->trigger();
        QVERIFY(!m_session->project().viewer.panZoom.enabled);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.5);
        command("togglePan")->trigger();
        QVERIFY(m_session->project().viewer.panZoom.enabled);
        m_session->assignViewer(0, second.id);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.0);
        m_session->assignViewer(0, owner);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.5);
        QCOMPARE(m_session->project().viewer.panZoom.pan, QPointF(-.1, .05));
    }
    void overlayEditorCommitsRendersAndCancels() {
        command("overlayPhiGrid")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::PhiGrid);
        command("overlayDynamicSymmetry")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::DynamicSymmetry);
        command("toggleOverlay")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::Off);
        command("toggleOverlay")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::DynamicSymmetry);
        auto before = QJsonDocument(projectToJson(m_session->project())).toJson();
        bool observed = false;
        QTimer::singleShot(0, [&] {
            auto dialog = dynamic_cast<OverlayDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QPushButton *>("overlayAddShape")->click();
            dialog->findChild<QComboBox *>("overlayShapeType")
                ->setCurrentIndex(int(OverlayShapeType::Rectangle));
            dialog->findChild<QPlainTextEdit *>("overlayShapePoints")
                ->setPlainText("0.2, 0.2\n0.8, 0.8");
            observed = true;
            dialog->reject();
        });
        command("overlayOptions")->trigger();
        QVERIFY(observed);
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), before);
        QImage image(40, 40, QImage::Format_ARGB32);
        image.fill(QColor(230, 30, 15));
        const auto path = QDir(m_temp.path()).filePath("overlay.png");
        QVERIFY(image.save(path));
        QTimer::singleShot(0, [&] {
            auto dialog = dynamic_cast<OverlayDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QComboBox *>("overlayType")->setCurrentIndex(int(OverlayType::Image));
            dialog->findChild<QLineEdit *>("overlayImagePath")->setText(path);
            dialog->findChild<QCheckBox *>("overlayImageKeepAspect")->setChecked(false);
            dialog->accept();
        });
        command("overlayOptions")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::Image);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_session->displayImage().pixelColor(m_session->displayImage().width() / 2,
                                                 m_session->displayImage().height() / 2),
            QColor(230, 30, 15), 10000);
        command("undo")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::DynamicSymmetry);
        command("redo")->trigger();
        QCOMPARE(m_session->project().viewer.overlay.type, OverlayType::Image);
    }
    void overlayEditorRejectsRemotePathsAndInvalidGeometry() {
        OverlaySettings value;
        value.type = OverlayType::Custom;
        OverlayDialog dialog(value, m_window.get());
        dialog.show();
        dialog.findChild<QPushButton *>("overlayAddShape")->click();
        auto points = dialog.findChild<QPlainTextEdit *>("overlayShapePoints");
        points->setPlainText("0.1, 0.2\n1.1, 0.8");
        dialog.accept();
        QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.findChild<QLabel *>("overlayValidation")->text().isEmpty());
        points->setPlainText("0.1, 0.2\n0.9, 0.8");
        auto type = dialog.findChild<QComboBox *>("overlayType");
        type->setCurrentIndex(int(OverlayType::Image));
        for (const auto &path : {QString("\\\\server\\share\\image.png"),
                                 QString("https://example.invalid/image.png")}) {
            dialog.findChild<QLineEdit *>("overlayImagePath")->setText(path);
            dialog.accept();
            QVERIFY(dialog.isVisible());
            QVERIFY(!dialog.findChild<QLabel *>("overlayValidation")->text().isEmpty());
        }
        type->setCurrentIndex(int(OverlayType::Custom));
        dialog.findChild<QLineEdit *>("overlayImagePath")->clear();
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.settings().shapes.first().points.last(), QPointF(.9, .8));
    }
    void loopDelayDialogAndSourceAudioSelection() {
        bool delayObserved = false;
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto seconds = dialog->findChild<QDoubleSpinBox *>("loopDelaySeconds");
            auto empty = dialog->findChild<QCheckBox *>("loopDelayEmpty");
            if (seconds && empty) {
                seconds->setValue(.375);
                empty->setChecked(true);
                delayObserved = true;
            }
            dialog->accept();
        });
        command("loopDelay")->trigger();
        QVERIFY(delayObserved);
        QCOMPARE(m_session->project().loopDelayMs, 375);
        QVERIFY(m_session->project().loopHoldEmpty);
        command("loopHoldEmpty")->trigger();
        QVERIFY(!m_session->project().loopHoldEmpty);
        m_session->setAudioEnabled(false);
        const auto sourceId = m_session->project().sources.first().id;
        m_session->mutateProject("Synthetic audio track descriptors", [&](Project &p) {
            auto &source = p.sources.first();
            AudioTrack english, french;
            english.streamIndex = 1;
            english.codec = "pcm_s16le";
            english.language = "eng";
            english.sampleRate = 48000;
            english.channels = 1;
            english.isDefault = true;
            english.durationUs = 1000000;
            french = english;
            french.streamIndex = 2;
            french.language = "fra";
            french.channels = 2;
            french.isDefault = false;
            source.info.hasAudio = true;
            source.info.audioTracks = {english, french};
        });
        m_session->assignViewer(0, sourceId);
        bool tracksObserved = false;
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto tracks = dialog->findChild<QComboBox *>("sourceAudioTrack");
            auto zoom = dialog->findChild<QDoubleSpinBox *>("ownerZoom");
            if (tracks && zoom) {
                tracksObserved = tracks->count() == 3 && tracks->itemText(2).contains("fra");
                tracks->setCurrentIndex(tracks->findData(2));
                zoom->setValue(1.75);
            }
            dialog->accept();
        });
        command("ownerProperties")->trigger();
        QVERIFY(tracksObserved);
        QCOMPARE(m_session->project().source(sourceId)->selectedAudioTrack, 2);
        QCOMPARE(m_session->project().source(sourceId)->info.channels, 2);
        QCOMPARE(m_session->project().viewer.panZoom.zoom, 1.75);
        auto menu = m_window->findChild<QMenu *>("sourceAudioTracks");
        QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
        QCOMPARE(menu->actions().size(), 3);
        menu->actions().first()->trigger();
        QCOMPARE(m_session->project().source(sourceId)->selectedAudioTrack, -1);
        QCOMPARE(m_session->project().source(sourceId)->info.channels, 1);
    }
    void lastOverlayPersistsAcrossWindowSessions() {
        command("overlayPhiGrid")->trigger();
        command("overlayOff")->trigger();
        m_window->setSmokeMode(false);
        QSettings().setValue("general/confirmClose", "disabled");
        QVERIFY(m_window->close());
        ReviewSession nextSession;
        MainWindow nextWindow(&nextSession);
        nextWindow.setSmokeMode(true);
        QCOMPARE(nextSession.project().viewer.overlay.type, OverlayType::Off);
        QVERIFY(!nextSession.dirty());
        for (auto action : nextWindow.commandActions())
            if (action->objectName() == "toggleOverlay")
                action->trigger();
        QCOMPARE(nextSession.project().viewer.overlay.type, OverlayType::PhiGrid);
    }
    void eyedropperSamplesComposedPixelsWithoutProjectMutation() {
        auto settings = m_session->project().viewer;
        settings.mode = ViewerMode::AB;
        settings.activePane = 1;
        settings.panZoom = {1.25, {.05, .05}, true};
        settings.overlay.type = OverlayType::Custom;
        OverlayShape rectangle;
        rectangle.id = newId();
        rectangle.type = OverlayShapeType::Rectangle;
        rectangle.points = {{.1, .1}, {.9, .9}};
        rectangle.color = rectangle.fill = QColor(230, 30, 15);
        settings.overlay.shapes = {rectangle};
        m_session->setViewerSettings(settings);
        QTRY_COMPARE(m_session->displayImage().pixelColor(480, 270), QColor(230, 30, 15));
        QVERIFY(m_session->saveProject(QDir(m_temp.path()).filePath("picker.oframe")));
        const auto project = QJsonDocument(projectToJson(m_session->project())).toJson();
        const auto history = m_session->undoStack()->index();
        auto viewer = m_window->viewer();
        command("eyedropper")->trigger();
        QCoreApplication::processEvents();
        const auto point = viewer->imageRect().center().toPoint();
        QCOMPARE(viewer->framebufferColorAt(point), QColor(230, 30, 15));
        QSignalSpy picked(viewer, &ViewerWidget::annotationColorPicked);
        QMouseEvent hover(QEvent::MouseMove, point, viewer->mapToGlobal(point), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer, &hover);
        QCOMPARE(viewer->sampledColor(), QColor(230, 30, 15));
        QVERIFY(m_window->findChild<QLabel *>("eyedropperReadout")->text().contains("#E61E0F"));
        QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(picked.count(), 1);
        QCOMPARE(viewer->annotationColor(), QColor(230, 30, 15));
        QCOMPARE(m_session->project().viewer.activePane, 1);
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), project);
        QCOMPARE(m_session->undoStack()->index(), history);
        QVERIFY(!m_session->dirty());
        QTest::keyClick(viewer, Qt::Key_Escape);
        QCOMPARE(viewer->transientTool(), ViewerWidget::TransientTool::None);
        QVERIFY(!command("eyedropper")->isChecked());
        command("eyedropper")->trigger();
        command("annotationsTools")->trigger();
        QCOMPARE(viewer->transientTool(), ViewerWidget::TransientTool::None);
        QImage transparent(4, 4, QImage::Format_ARGB32);
        transparent.fill(QColor(255, 0, 0, 128));
        viewer->setPreviewImage(transparent);
        QCOMPARE(viewer->framebufferColorAt(viewer->imageRect().center()), QColor(141, 14, 16));
        QImage gradient(2, 2, QImage::Format_ARGB32);
        gradient.setPixelColor(0, 0, Qt::red);
        gradient.setPixelColor(1, 0, Qt::green);
        gradient.setPixelColor(0, 1, Qt::blue);
        gradient.setPixelColor(1, 1, QColor(255, 255, 0, 128));
        viewer->setPreviewImage(gradient);
        const auto framebuffer = viewer->grab().toImage();
        const auto imageRect = viewer->imageRect();
        for (auto relative : {QPointF(.31, .37), QPointF(.48, .59), QPointF(.72, .64)}) {
            const QPointF sample(imageRect.left() + relative.x() * imageRect.width(),
                                 imageRect.top() + relative.y() * imageRect.height());
            const auto dpr = framebuffer.devicePixelRatio();
            const auto displayed =
                framebuffer.pixelColor(int(sample.x() * dpr), int(sample.y() * dpr));
            QCOMPARE(viewer->framebufferColorAt(sample), displayed);
            QVERIFY(displayed != gradient.pixelColor(int(relative.x() * gradient.width()),
                                                     int(relative.y() * gradient.height())));
        }
    }
    void laserIsTransientAndAbsentFromExport() {
        auto viewer = m_window->viewer();
        viewer->setLaserSettings(QColor(255, 30, 30), 5, 80);
        command("laserPointer")->trigger();
        QCoreApplication::processEvents();
        const auto project = QJsonDocument(projectToJson(m_session->project())).toJson();
        const auto history = m_session->undoStack()->index();
        const auto frame = m_session->displayImage();
        const auto baseline = viewer->grab().toImage();
        const auto center = viewer->imageRect().center().toPoint();
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, center - QPoint(40, 20));
        QTest::mouseMove(viewer, center, 5);
        QTest::mouseMove(viewer, center + QPoint(40, -20), 5);
        QVERIFY(viewer->laserStrokeVisible());
        QVERIFY(viewer->grab().toImage() != baseline);
        QCOMPARE(m_session->displayImage(), frame);
        ExportSettings settings;
        settings.kind = ExportKind::ImageSequence;
        settings.path = QDir(m_temp.path()).filePath("laser-export");
        settings.firstFrame = settings.lastFrame = m_session->currentFrame();
        settings.size = frame.size();
        settings.includeAudio = false;
        QSignalSpy exported(m_session.get(), &ReviewSession::exportFinished);
        m_session->exportAsync(settings);
        QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000);
        QVERIFY(exported.first().at(0).toBool());
        QCOMPARE(QImage(QDir(settings.path).filePath("frame_000001.png"))
                     .convertToFormat(frame.format()),
                 frame);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, center + QPoint(40, -20));
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->laserStrokeVisible(), 1000);
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), project);
        QCOMPARE(m_session->undoStack()->index(), history);
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, center);
        QVERIFY(viewer->laserStrokeVisible());
        command("cancelTransientTool")->trigger();
        QVERIFY(!viewer->laserStrokeVisible());
        QCOMPARE(viewer->transientTool(), ViewerWidget::TransientTool::None);
    }
    void bookmarkFiltersClassifyDataAndRemoveEmptyWithUndo() {
        const auto owner = m_session->activeOwnerId();
        const auto other = m_session->addTimeline("Other owner");
        m_session->mutateProject("Synthetic categorized bookmarks", [&](Project &p) {
            for (auto frame : {2, 5, 8, 9, 10, 17}) {
                Bookmark bookmark;
                bookmark.id = newId();
                bookmark.timelineId = owner;
                bookmark.frame = frame;
                bookmark.endFrame = frame == 10 ? 13 : frame == 17 ? 18 : frame;
                bookmark.name = "Named bookmark";
                p.bookmarks.append(bookmark);
            }
            Bookmark foreign;
            foreign.id = newId();
            foreign.timelineId = other;
            foreign.frame = 1;
            foreign.endFrame = 1;
            p.bookmarks.append(foreign);
            for (auto frame : {5, 11, 20}) {
                Note note;
                note.id = newId();
                note.timelineId = owner;
                note.frame = frame;
                note.text = "Review note";
                p.notes.append(note);
            }
            for (auto frame : {0, 8, 12}) {
                Annotation annotation;
                annotation.id = newId();
                annotation.timelineId = owner;
                annotation.frame = annotation.endFrame = frame;
                annotation.points = {{.1, .1}, {.2, .2}};
                annotation.background = frame == 0;
                annotation.hold = true;
                annotation.ghostFrames = 2;
                p.annotations.append(annotation);
            }
        });
        auto table = m_window->findChild<QTableWidget *>("notesTable");
        QCOMPARE(table->rowCount(), 9);
        const auto before = QJsonDocument(projectToJson(m_session->project())).toJson();
        command("bookmarkFilter1")->trigger();
        QCOMPARE(table->rowCount(), 3);
        m_session->seek(0, false);
        command("nextBookmark")->trigger();
        QCOMPARE(m_session->currentFrame(), 2);
        command("nextBookmark")->trigger();
        QCOMPARE(m_session->currentFrame(), 9);
        command("bookmarkFilter2")->trigger();
        QCOMPARE(table->rowCount(), 5);
        command("bookmarkFilter3")->trigger();
        QCOMPARE(table->rowCount(), 2);
        m_session->seek(0, false);
        command("nextBookmark")->trigger();
        QCOMPARE(m_session->currentFrame(), 8);
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), before);
        command("bookmarkFilter1")->trigger();
        command("removeEmptyBookmarks")->trigger();
        QCOMPARE(m_session->project().bookmarks.size(), 4);
        QCOMPARE(m_session->project().notes.size(), 3);
        QCOMPARE(m_session->project().annotations.size(), 3);
        QCOMPARE(table->rowCount(), 0);
        command("undo")->trigger();
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), before);
        QCOMPARE(table->rowCount(), 3);
        ReviewSession next;
        MainWindow nextWindow(&next);
        nextWindow.setSmokeMode(true);
        QCOMPARE(nextWindow.findChild<QComboBox *>("bookmarkFilter")->currentIndex(), 1);
    }
    void sourceAudioOffsetAndStretchControlsCommitAndCancel() {
        const auto id = m_session->project().sources.first().id;
        m_session->setAudioEnabled(false);
        m_session->mutateProject("Synthetic audio metadata",
                                 [](Project &p) { p.sources.first().info.hasAudio = true; });
        m_session->assignViewer(0, id);
        auto open = [&](bool accept) {
            QTimer::singleShot(0, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto offset = dialog->findChild<QDoubleSpinBox *>("sourceAudioOffsetMs");
                if (offset)
                    offset->setValue(accept ? -125.25 : 420.5);
                if (accept)
                    dialog->accept();
                else
                    dialog->reject();
            });
            command("ownerProperties")->trigger();
        };
        const auto before = QJsonDocument(projectToJson(m_session->project())).toJson();
        open(false);
        QCOMPARE(QJsonDocument(projectToJson(m_session->project())).toJson(), before);
        open(true);
        QCOMPARE(m_session->project().source(id)->audioOffsetUs, -125250);
        command("undo")->trigger();
        QCOMPARE(m_session->project().source(id)->audioOffsetUs, 0);
        QVERIFY(m_session->audioTimeStretchingEnabled());
        command("audioTimeStretching")->trigger();
        QVERIFY(!m_session->audioTimeStretchingEnabled());
        QVERIFY(!QSettings().value("audio/timeStretching").toBool());
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialog->findChild<QCheckBox *>("audio/timeStretching")->setChecked(true);
            dialog->findChild<QSpinBox *>("annotation/laserFadeMs")->setValue(900);
            dialog->reject();
        });
        command("preferences")->trigger();
        QVERIFY(!m_session->audioTimeStretchingEnabled());
        QCOMPARE(QSettings().value("annotation/laserFadeMs", 350).toInt(), 350);
        const qint64 largeOffset = MaximumAudioOffsetUs - 2;
        m_session->setSourceAudioOffset(id, largeOffset);
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog)
                dialog->accept();
        });
        command("ownerProperties")->trigger();
        QCOMPARE(m_session->project().source(id)->audioOffsetUs, largeOffset);
        const auto unchangedHistory = m_session->undoStack()->index();
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog)
                dialog->accept();
        });
        command("ownerProperties")->trigger();
        QCOMPARE(m_session->undoStack()->index(), unchangedHistory);
        int afterRemoval = -1;
        QTimer::singleShot(0, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            m_session->removeSource(id);
            afterRemoval = m_session->undoStack()->index();
            dialog->accept();
        });
        command("ownerProperties")->trigger();
        QVERIFY(afterRemoval >= 0);
        QCOMPARE(m_session->undoStack()->index(), afterRemoval);
        QVERIFY(!m_session->project().source(id));
    }
    void timelineSeekMapsToFrames() {
        auto timeline = m_window->findChild<TimelineWidget *>("globalTimeControl");
        QVERIFY(timeline);
        QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier,
                          QPoint(timeline->width() / 2, 8));
        QVERIFY(m_session->currentFrame() >= 11 && m_session->currentFrame() <= 12);
        command("start")->trigger();
        QCOMPARE(m_session->currentFrame(), 0);
    }
    void displayNumberingUsesOwnerOffset() {
        auto spin = m_window->findChild<QSpinBox *>("currentFrame");
        QVERIFY(spin);
        QCOMPARE(spin->value(), 1);
        auto id = m_session->activeOwnerId();
        m_session->mutateProject("Set frame numbering", [=](Project &p) {
            for (auto &t : p.timelines)
                if (t.id == id) {
                    t.timecodeOffsetEnabled = true;
                    t.timecodeOffset = 1001;
                }
        });
        QCOMPARE(spin->value(), 1001);
        spin->setValue(1005);
        QCOMPARE(m_session->currentFrame(), 4);
        QCOMPARE(displayFrameNumber(m_session->project(), id, 4), 1005);
    }
    void preferencesApplyToActualControls() {
        bool processed = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto reverse = dialog->findChild<QCheckBox *>("audio/reverse"),
                 undo = dialog->findChild<QCheckBox *>("project/undo"),
                 pan = dialog->findChild<QCheckBox *>("viewer/panZoom");
            auto offset = dialog->findChild<QSpinBox *>("project/displayOffset");
            if (reverse && undo && pan && offset) {
                reverse->setChecked(false);
                undo->setChecked(false);
                pan->setChecked(false);
                offset->setValue(1001);
                processed = true;
            }
            dialog->accept();
        });
        command("preferences")->trigger();
        QVERIFY(processed);
        QVERIFY(!m_session->reverseAudioEnabled());
        QVERIFY(!m_session->projectUndoEnabled());
        QVERIFY(!m_window->viewer()->panZoomEnabled());
        m_session->newProject();
        QCOMPARE(displayFrameNumber(m_session->project(), m_session->activeOwnerId(), 0), 1001);
    }
    void exportCancellationAndBlankPathLeaveStateUntouched() {
        const auto before = projectToJson(m_session->project());
        const int step = m_session->playbackFrameStep();
        QSignalSpy finished(m_session.get(), &ReviewSession::exportFinished);
        for (bool accept : {false, true}) {
            bool processed = false;
            QTimer::singleShot(0, m_window.get(), [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) {
                    QApplication::closeAllWindows();
                    return;
                }
                auto path = dialog->findChild<QLineEdit *>("exportPath");
                auto force = dialog->findChild<QCheckBox *>("exportForceOverwrite");
                auto frameStep = dialog->findChild<QSpinBox *>("exportFrameStep");
                if (path && force && frameStep) {
                    path->setText(accept ? QString()
                                         : QDir(m_temp.path()).filePath("cancelled.mp4"));
                    force->setChecked(true);
                    frameStep->setValue(7);
                    processed = true;
                }
                if (accept && processed)
                    dialog->accept();
                else
                    dialog->reject();
            });
            command("export")->trigger();
            QVERIFY(processed);
            QVERIFY(!m_session->exporting());
            QCOMPARE(finished.size(), 0);
            QCOMPARE(m_session->playbackFrameStep(), step);
            QCOMPARE(projectToJson(m_session->project()), before);
            QVERIFY(!QFileInfo::exists(QDir(m_temp.path()).filePath("cancelled.mp4")));
        }
    }
    void exportSequenceOverwritePreservesOtherFiles() {
        const QString directory = QDir(m_temp.path()).filePath("existing-sequence");
        QVERIFY(QDir().mkpath(directory));
        const QString firstPath = QDir(directory).filePath("frame_000001.png");
        QImage original(16, 16, QImage::Format_RGB32);
        original.fill(Qt::magenta);
        QVERIFY(original.save(firstPath));
        const QString unrelatedPath = QDir(directory).filePath("keep.txt");
        QFile unrelated(unrelatedPath);
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        QCOMPARE(unrelated.write("independent fixture"), qint64(19));
        unrelated.close();
        QSettings preferences;
        preferences.setValue("export/kind", 2);
        preferences.setValue("export/width", 160);
        preferences.setValue("export/height", 90);
        preferences.setValue("export/includeAudio", false);
        m_session->setRange(0, 1);
        const auto before = projectToJson(m_session->project());
        QSignalSpy finished(m_session.get(), &ReviewSession::exportFinished);
        bool processed = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto path = dialog->findChild<QLineEdit *>("exportPath");
            auto force = dialog->findChild<QCheckBox *>("exportForceOverwrite");
            auto kind = dialog->findChild<QComboBox *>("exportKind");
            if (path && force && kind) {
                kind->setCurrentIndex(2);
                path->setText(directory);
                force->setChecked(true);
                processed = force->isEnabled();
            }
            if (processed)
                dialog->accept();
            else
                dialog->reject();
        });
        command("export")->trigger();
        QVERIFY(processed);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(projectToJson(m_session->project()), before);
        QVERIFY(!m_session->exporting());
        QCOMPARE(finished.first().at(2).toString(), firstPath);
        QImage exported(firstPath);
        QCOMPARE(exported.size(), QSize(160, 90));
        QVERIFY(exported.pixelColor(80, 45) != QColor(Qt::magenta));
        QVERIFY(QFileInfo::exists(QDir(directory).filePath("frame_000002.png")));
        QVERIFY(unrelated.open(QIODevice::ReadOnly));
        QCOMPARE(unrelated.readAll(), QByteArray("independent fixture"));
    }
    void exportUsesRequestedRationalFrameRate() {
        QSettings preferences;
        preferences.setValue("export/kind", 0);
        preferences.setValue("export/videoCodec", "ffv1");
        preferences.setValue("export/width", 160);
        preferences.setValue("export/height", 90);
        preferences.setValue("export/includeAudio", false);
        const QString target = QDir(m_temp.path()).filePath("rational-output.mkv");
        const auto before = projectToJson(m_session->project());
        QSignalSpy finished(m_session.get(), &ReviewSession::exportFinished);
        bool processed = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto path = dialog->findChild<QLineEdit *>("exportPath");
            auto numerator = dialog->findChild<QSpinBox *>("exportFpsNumerator");
            auto denominator = dialog->findChild<QSpinBox *>("exportFpsDenominator");
            if (path && numerator && denominator) {
                processed = numerator->isEnabled() && denominator->isEnabled() &&
                            numerator->value() == 24 && denominator->value() == 1;
                path->setText(target);
                numerator->setValue(24000);
                denominator->setValue(2000);
            }
            if (processed)
                dialog->accept();
            else
                dialog->reject();
        });
        command("export")->trigger();
        QVERIFY(processed);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(projectToJson(m_session->project()), before);
        QCOMPARE(finished.first().at(2).toString(), target);
        const auto output = probeMedia(target);
        QCOMPARE(output.info.frameCount(), 12);
        QCOMPARE(output.info.fps.value(), 12.0);
        QVERIFY(std::abs(output.info.frameEndUs(11) - output.info.frameStartUs(0) - 1000000) <=
                2000);
    }
    void exportRejectsInvalidFrameRate() {
        const QString target = QDir(m_temp.path()).filePath("invalid-rate.mp4");
        const auto before = projectToJson(m_session->project());
        QSignalSpy finished(m_session.get(), &ReviewSession::exportFinished);
        bool processed = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto path = dialog->findChild<QLineEdit *>("exportPath");
            auto numerator = dialog->findChild<QSpinBox *>("exportFpsNumerator");
            auto denominator = dialog->findChild<QSpinBox *>("exportFpsDenominator");
            if (path && numerator && denominator) {
                path->setText(target);
                numerator->setValue(1000000);
                denominator->setValue(1);
                processed = true;
            }
            if (processed)
                dialog->accept();
            else
                dialog->reject();
        });
        command("export")->trigger();
        QVERIFY(processed);
        QVERIFY(!m_session->exporting());
        QCOMPARE(finished.size(), 0);
        QCOMPARE(projectToJson(m_session->project()), before);
        QVERIFY(!QFileInfo::exists(target));
    }
    void sparseImageExportDisablesContinuousAudio() {
        bool valid = false;
        const auto before = projectToJson(m_session->project());
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto kind = dialog->findChild<QComboBox *>("exportKind");
            auto step = dialog->findChild<QSpinBox *>("exportFrameStep");
            auto audio = dialog->findChild<QCheckBox *>("exportIncludeAudio");
            auto bookmarks = dialog->findChild<QCheckBox *>("exportBookmarksOnly");
            auto fps = dialog->findChild<QSpinBox *>("exportFpsNumerator");
            if (kind && step && audio && bookmarks && fps) {
                kind->setCurrentIndex(2);
                valid = audio->isEnabled() && !fps->isEnabled();
                step->setValue(2);
                valid &= !audio->isEnabled() && audio->text().contains("unavailable");
                step->setValue(1);
                valid &= audio->isEnabled();
                bookmarks->setChecked(true);
                valid &= !audio->isEnabled();
                kind->setCurrentIndex(0);
                valid &= audio->isEnabled() && fps->isEnabled();
            }
            dialog->reject();
        });
        command("export")->trigger();
        QVERIFY(valid);
        QCOMPARE(projectToJson(m_session->project()), before);
    }
    void statusMessageDoesNotOverlayFrameLabel() {
        auto label = m_window->findChild<QLabel *>("statusCurrentFrame");
        QVERIFY(label);
        m_window->statusBar()->showMessage("Synthetic status");
        QVERIFY(label->text().isEmpty());
        m_session->seek(3, false);
        QVERIFY(label->text().isEmpty());
        m_window->statusBar()->clearMessage();
        QCOMPARE(label->text(), QString("Frame 4"));
    }
    void fileDialogRoutingAndPathPolicy() {
        using namespace fileDialogDetail;
#ifdef Q_OS_WIN
        QVERIFY(useNativeWindowsDialogs("windows", false));
#else
        QVERIFY(!useNativeWindowsDialogs("windows", false));
#endif
        QVERIFY(!useNativeWindowsDialogs("windows", true));
        QVERIFY(!useNativeWindowsDialogs("offscreen", false));
        QVERIFY(!useNativeWindowsDialogs("minimal", false));
        const auto folder = QDir(m_temp.path()).filePath(QString::fromUtf8("Диалоги с пробелом"));
        QVERIFY(QDir().mkpath(folder));
        const auto input = QDir(folder).filePath(QString::fromUtf8("тест.txt"));
        QFile file(input);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("synthetic");
        file.close();
        const auto output = QDir(folder).filePath("new.txt");
        QCOMPARE(safeInitialPath(input), input);
        QCOMPARE(safeInitialPath(output), output);
        QCOMPARE(safeInitialPath(output, true), folder);
        QCOMPARE(safeInitialPath(folder), folder);
        for (const auto &invalid :
             {QString("//example.invalid/share/file.txt"),
              QString("https://example.invalid/file.txt"), folder + "/../file.txt"}) {
            const auto safe = safeInitialPath(invalid);
            QVERIFY(!safe.isEmpty());
            QCOMPARE(LocalPathPolicy::validateDirectory(safe), safe);
            QString error;
            for (auto mode : {Selection::OpenFile, Selection::OpenFiles, Selection::SaveFile,
                              Selection::Directory}) {
                QVERIFY(validateSelection({invalid}, mode, &error).isEmpty());
                QVERIFY(!error.isEmpty());
            }
            QVERIFY(validateSelection({input, invalid}, Selection::OpenFiles, &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        QString error;
        QCOMPARE(validateSelection({input}, Selection::OpenFile, &error), QStringList{input});
        QVERIFY(error.isEmpty());
        QCOMPARE(validateSelection({output}, Selection::SaveFile, &error), QStringList{output});
        QCOMPARE(validateSelection({folder}, Selection::Directory, &error), QStringList{folder});
        QVERIFY(validateSelection({output}, Selection::OpenFile, &error).isEmpty());
        QVERIFY(validateSelection({folder}, Selection::OpenFile, &error).isEmpty());
        QVERIFY(validateSelection({input}, Selection::Directory, &error).isEmpty());
        QVERIFY(validateSelection({input, input}, Selection::OpenFile, &error).isEmpty());
        QVERIFY(validateSelection({}, Selection::OpenFiles, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(validateSelection({QString()}, Selection::SaveFile, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(!QFileInfo::exists(output));
    }
    void fileDialogFallbackSelectionAndCancel() {
        const auto before = projectToJson(m_session->project());
        const auto folder = QDir(m_temp.path()).filePath("picker-fixture");
        QVERIFY(QDir().mkpath(folder));
        const QStringList inputs{QDir(folder).filePath("first.txt"),
                                 QDir(folder).filePath("second.txt")};
        for (const auto &path : inputs) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("synthetic");
        }
        const auto output = QDir(folder).filePath("new-file.txt");
        for (bool cancel : {false, true}) {
            for (int mode = 0; mode < 4; ++mode) {
                QObject timerScope;
                bool observed = false;
                QTimer::singleShot(0, &timerScope, [&] {
                    auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog)
                        return;
                    observed = dialog->objectName() == "localFileDialogFallback";
                    auto name = dialog->findChild<QLineEdit *>("localFileName");
                    auto entries = dialog->findChild<QTreeWidget *>("localEntries");
                    auto buttons = dialog->findChild<QDialogButtonBox *>();
                    if (cancel || !observed || !name || !entries || !buttons) {
                        dialog->reject();
                        return;
                    }
                    if (mode == 0)
                        name->setText("first.txt");
                    else if (mode == 1) {
                        for (int i = 0; i < entries->topLevelItemCount(); ++i)
                            entries->topLevelItem(i)->setSelected(true);
                    } else if (mode == 2)
                        name->setText("new-file");
                    buttons->button(QDialogButtonBox::Open)->click();
                });
                QTimer::singleShot(3000, &timerScope, [] {
                    if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                        dialog->reject();
                });
                QStringList selected;
                if (mode == 0)
                    selected << LocalFileDialog::getOpenFileName(m_window.get(), "Synthetic open",
                                                                 folder, "Text (*.txt)");
                else if (mode == 1)
                    selected = LocalFileDialog::getOpenFileNames(
                        m_window.get(), "Synthetic multi-open", folder, "Text (*.txt)");
                else if (mode == 2)
                    selected << LocalFileDialog::getSaveFileName(m_window.get(), "Synthetic save",
                                                                 folder, "Text (*.txt)");
                else
                    selected << LocalFileDialog::getExistingDirectory(
                        m_window.get(), "Synthetic directory", folder);
                QVERIFY(observed);
                selected.removeAll(QString());
                selected.sort();
                if (cancel)
                    QVERIFY(selected.isEmpty());
                else if (mode == 0)
                    QCOMPARE(selected, QStringList{inputs.first()});
                else if (mode == 1)
                    QCOMPARE(selected, inputs);
                else if (mode == 2)
                    QCOMPARE(selected, QStringList{output});
                else
                    QCOMPARE(selected, QStringList{folder});
            }
        }
        QVERIFY(!QFileInfo::exists(output));
        QCOMPARE(projectToJson(m_session->project()), before);
    }
    void localPickerDriveMatchesInitialDirectory() {
        bool matched = false;
        QTimer::singleShot(0, m_window.get(), [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                QApplication::closeAllWindows();
                return;
            }
            auto roots = dialog->findChild<QComboBox *>("localDrive");
            auto directory = dialog->findChild<QLineEdit *>("localDirectory");
            matched = roots && directory && !roots->currentText().isEmpty() &&
                      directory->text().startsWith(roots->currentText(), Qt::CaseInsensitive);
            dialog->reject();
        });
        const auto selected = LocalFileDialog::getExistingDirectory(
            m_window.get(), "Synthetic drive selection", QDir::currentPath());
        QVERIFY(selected.isEmpty());
        QVERIFY(matched);
    }
    void csvParsesQuotedMultilineNotes() {
        QVector<QStringList> rows;
        QString error;
        QVERIFY(parseNotesCsv(
            "frames,notes,author\r\n12,\"A comma, and a\nnew line with \"\"quotes\"\"\",Review\r\n",
            rows, &error));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[1][1], QString("A comma, and a\nnew line with \"quotes\""));
        QVERIFY(!parseNotesCsv("frame,note\n1,\"unterminated", rows, &error));
        QVERIFY(!error.isEmpty());
    }
    void localPathsRejectNetworkAndTraversal() {
        QString error;
        QVERIFY(createLocalDirectoryChain("//example.invalid/share/blocked", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(createLocalDirectoryChain("https://example.invalid/output", &error).isEmpty());
        QVERIFY(createLocalDirectoryChain(QDir(m_temp.path()).filePath("must_not_exist/../blocked"),
                                          &error)
                    .isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(m_temp.path()).filePath("must_not_exist")));
        auto local = QDir(m_temp.path()).filePath("nested/captures");
        QCOMPARE(createLocalDirectoryChain(local, &error), QDir::cleanPath(local));
        QVERIFY(QFileInfo::exists(local));
    }
    void captureAndProjectRoundTrip() {
        command("bookmark")->trigger();
        auto path = QDir(m_temp.path()).filePath("review.oframe");
        QVERIFY(m_session->saveProject(path));
        m_session->newProject();
        QCOMPARE(m_session->project().sources.size(), 0);
        QVERIFY(m_session->openProject(path));
        QCOMPARE(m_session->project().sources.size(), 1);
        QCOMPARE(m_session->project().bookmarks.size(), 1);
        QVERIFY(!m_window->grab().isNull());
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"
