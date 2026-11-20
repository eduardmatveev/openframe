#include "MainWindow.h"
#include "ViewerWidget.h"
#include "TimelineWidget.h"
#include "Theme.h"
#include "core/ReviewSession.h"
#include "integration/ApiServer.h"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include "LocalFileDialog.h"
#include "NotesCsvDialog.h"
#include "OverlayDialog.h"
#include "TabletInput.h"
#include "ViewerDialogs.h"
#include "integration/LocalPathPolicy.h"
#include <QFileInfo>
#include <QFormLayout>
#include <QEventLoop>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <limits>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <shlobj.h>
#else
#include <QDesktopServices>
#include <QUrl>
#endif
namespace openframe {
namespace {
constexpr int IdRole = Qt::UserRole + 1, KindRole = Qt::UserRole + 2;
int displayBounded(qint64 value) {
    return int(std::clamp<qint64>(value, std::numeric_limits<int>::min(),
                                  std::numeric_limits<int>::max()));
}
int bounded(qint64 value) {
    return int(std::clamp<qint64>(value, 0, std::numeric_limits<int>::max()));
}
QToolButton *iconButton(const QString &icon, const QString &tip, QWidget *parent) {
    auto b = new QToolButton(parent);
    b->setIcon(uiIcon(icon));
    b->setToolTip(tip);
    return b;
}
QWidget *row(QWidget *parent, const QList<QWidget *> &widgets) {
    auto w = new QWidget(parent);
    auto l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(5);
    for (auto item : widgets)
        l->addWidget(item);
    return w;
}
QString durationString(qint64 frames, double fps) {
    double secs = frames / std::max(.001, fps);
    int ms = int(std::round((secs - std::floor(secs)) * 1000));
    return QString("%1:%2.%3")
        .arg(int(secs) / 60, 2, 10, QChar('0'))
        .arg(int(secs) % 60, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}
} // namespace
MainWindow::MainWindow(ReviewSession *session, ApiServer *server, QWidget *parent)
    : QMainWindow(parent), m_session(session), m_server(server) {
    setObjectName("OpenFrameMainWindow");
    setWindowTitle("OpenFrame");
    setMinimumSize(860, 560);
    resize(1360, 840);
    setDockNestingEnabled(true);
    setAcceptDrops(true);
    auto center = new QWidget(this);
    auto layout = new QVBoxLayout(center);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_viewer = new ViewerWidget(session, center);
    connect(m_viewer, &ViewerWidget::tabletDiagnosticChanged, this,
            [this](const QString &text) { m_lastTabletDiagnostic = text; });
    m_timeline = new TimelineWidget(session, center);
    m_rangeTimeline = new TimelineWidget(session, center);
    m_rangeTimeline->setObjectName("rangeTimeControl");
    m_rangeTimeline->setRangeOnly(true);
    layout->addWidget(m_viewer, 1);
    layout->addWidget(m_timeline);
    layout->addWidget(m_rangeTimeline);
    setCentralWidget(center);
    m_bookmarkTimer = new QTimer(this);
    connect(m_bookmarkTimer, &QTimer::timeout, this, [this] {
        auto before = m_session->currentFrame();
        jumpBookmark(1);
        if (before == m_session->currentFrame())
            m_bookmarkTimer->stop();
    });
    createDocks();
    createMenus();
    createToolbars();
    m_frameLabel = new QLabel("0", this);
    m_frameLabel->setObjectName("statusCurrentFrame");
    m_durationLabel = new QLabel(this);
    m_zoomLabel = new QLabel("100%", this);
    m_networkLabel = new QLabel(this);
    statusBar()->addWidget(m_frameLabel);
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString &message) {
        m_frameLabel->setText(
            message.isEmpty()
                ? QString("Frame %1")
                      .arg(displayFrameNumber(m_session->project(), m_session->activeOwnerId(),
                                              m_session->currentFrame()))
                : QString());
    });
    statusBar()->addPermanentWidget(m_durationLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
    statusBar()->addPermanentWidget(m_networkLabel);
    connect(session, &ReviewSession::projectChanged, this, &MainWindow::refreshProject);
    connect(session, &ReviewSession::transportSettingsChanged, this,
            &MainWindow::refreshTransportState);
    connect(session, &ReviewSession::frameChanged, this, &MainWindow::refreshFrame);
    connect(session, &ReviewSession::playbackChanged, this, [this](bool playing) {
        m_actions["play"]->setIcon(uiIcon(playing ? "pause" : "play"));
        m_actions["play"]->setText(playing ? "Pause" : "Play / Pause");
        refreshTransportState();
    });
    connect(session, &ReviewSession::imageReady, this, [this](const QImage &image) {
        if (!image.isNull())
            m_thumbnail->setPixmap(QPixmap::fromImage(image).scaled(
                m_thumbnail->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
    connect(session, &ReviewSession::error, this, &MainWindow::showError);
    connect(session, &ReviewSession::status, this, [this](const QString &text) {
        statusBar()->showMessage(text, 6000);
        log(text);
    });
    connect(session, &ReviewSession::busyChanged, this, [this](bool busy) {
        statusBar()->showMessage(busy ? "Reading local media…" : "Ready", 3000);
        if (!busy && QSettings().value("general/resizeOnLoad", false).toBool() &&
            m_session->durationFrames() > 0)
            m_actions["size1"]->trigger();
    });
    connect(session, &ReviewSession::exportProgress, this, [this](int value) {
        if (m_exportProgress)
            m_exportProgress->setValue(value);
    });
    connect(session, &ReviewSession::exportFinished, this,
            [this](bool success, const QString &message, const QString &publishedFile) {
                const bool fromExportDialog = !m_exportProgress.isNull();
                if (m_exportProgress) {
                    m_exportProgress->close();
                    m_exportProgress->deleteLater();
                    m_exportProgress = nullptr;
                }
                if (success) {
                    statusBar()->showMessage(message, 15000);
                    log(message);
                    if (fromExportDialog)
                        revealExportedFile(publishedFile);
                } else
                    showError(message);
            });
    connect(m_viewer, &ViewerWidget::annotationContextChanged, this,
            &MainWindow::refreshUndoActions);
    connect(m_viewer, &ViewerWidget::filesDropped, this, &MainWindow::importMediaPaths);
    connect(m_viewer, &ViewerWidget::transformChanged, this, [this] {
        auto &v = m_session->project().viewer;
        m_zoomLabel->setText(v.panZoom.enabled ? QString::number(v.panZoom.zoom * 100, 'f', 0) + "%"
                                               : "Pan/zoom off");
    });
    connect(m_viewer, &ViewerWidget::contextMenuRequested, this, [this](QPoint pos) {
        QMenu menu(this);
        menu.addAction(m_actions["play"]);
        menu.addAction(m_actions["bookmark"]);
        menu.addAction(m_actions["note"]);
        menu.addSeparator();
        menu.addAction(m_actions["annotationsTools"]);
        menu.addAction(m_actions["resetView"]);
        menu.addAction(m_actions["captureClipboard"]);
        menu.exec(pos);
    });
    connect(m_timeline, &TimelineWidget::noteRequested, this, [this](qint64 frame) {
        m_session->seek(frame);
        editNote();
    });
    connect(m_rangeTimeline, &TimelineWidget::noteRequested, this, [this](qint64 frame) {
        m_session->seek(frame);
        editNote();
    });
    if (m_server) {
        connect(m_server, &ApiServer::activateRequested, this, [this] {
            auto state = windowState();
            state.setFlag(Qt::WindowMinimized, false);
            setWindowState(state);
            show();
            raise();
            activateWindow();
        });
        auto updateServer = [this] {
            m_networkLabel->setText(m_server->isListening()
                                        ? QString("API 127.0.0.1:%1").arg(m_server->listeningPort())
                                        : "API off");
        };
        connect(m_server, &ApiServer::started, this, updateServer);
        connect(m_server, &ApiServer::stopped, this, updateServer);
        connect(m_server, &ApiServer::error, this, &MainWindow::log);
        updateServer();
    }
    m_session->setHideAnnotationsDuringPlayback(true);
    m_notesDock->hide();
    m_consoleDock->hide();
    m_annotationBar->hide();
    m_annotationOptions->hide();
    m_rangeTimeline->hide();
    m_defaultLayout = saveState();
    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());
    applyPreferences();
    if (m_session->project().sources.isEmpty() && m_session->projectPath().isEmpty() &&
        !m_session->dirty()) {
        m_session->newProject();
        m_lastOverlayType = OverlayType(std::clamp(
            settings.value("viewer/lastOverlayType", int(OverlayType::Grid)).toInt(), 1, 7));
        Project overlayDefaults;
        QString error;
        const auto stored =
            QJsonDocument::fromJson(settings.value("viewer/overlayDefaults").toByteArray());
        if (stored.isObject() && projectFromJson(stored.object(), overlayDefaults, &error)) {
            auto viewer = m_session->project().viewer;
            viewer.overlay = overlayDefaults.viewer.overlay;
            m_session->setViewerSettings(viewer);
            m_session->undoStack()->clear();
        }
    }
    refreshProject();
}
QAction *MainWindow::action(const QString &id, const QString &text, const QString &icon,
                            const QList<QKeySequence> &shortcuts,
                            const std::function<void()> &callback, bool checkable) {
    auto a = new QAction(text, this);
    a->setObjectName(id);
    if (!icon.isEmpty())
        a->setIcon(uiIcon(icon));
    a->setShortcuts(shortcuts);
    a->setShortcutContext(Qt::WindowShortcut);
    a->setCheckable(checkable);
    addAction(a);
    connect(a, &QAction::triggered, this, [callback] { callback(); });
    m_actions.insert(id, a);
    m_defaultShortcuts.insert(id, shortcuts);
    return a;
}
QList<QAction *> MainWindow::commandActions() const {
    return m_actions.values();
}
QUndoStack *MainWindow::activeUndoStack() {
    return m_viewer->annotationsEnabled()
               ? m_session->annotationUndoStack(
                     displayedAnnotationOwner(m_session->project()),
                     displayedAnnotationFrame(m_session->project(), m_session->currentFrame()),
                     m_viewer->backgroundLayer())
               : m_session->undoStack();
}
void MainWindow::refreshUndoActions() {
    if (!m_actions.contains("undo"))
        return;
    auto stack = activeUndoStack();
    connect(stack, &QUndoStack::indexChanged, this, &MainWindow::refreshUndoActions,
            Qt::UniqueConnection);
    bool historyEnabled = m_viewer->annotationsEnabled() || m_session->projectUndoEnabled();
    m_actions["undo"]->setEnabled(historyEnabled && stack->canUndo());
    m_actions["redo"]->setEnabled(historyEnabled && stack->canRedo());
    m_actions["undo"]->setText(stack->canUndo() ? "Undo " + stack->undoText() : "Undo");
    m_actions["redo"]->setText(stack->canRedo() ? "Redo " + stack->redoText() : "Redo");
    updateTitle();
}
void MainWindow::ghostingDialog() {
    auto settings = m_session->project().viewer;
    QDialog dialog(this);
    dialog.setWindowTitle("Ghosting Options");
    auto form = new QFormLayout(&dialog);
    auto enabled = new QCheckBox("Show annotation ghosts", &dialog);
    enabled->setChecked(settings.ghostEnabled);
    form->addRow(enabled);
    auto unit = new QComboBox(&dialog);
    unit->addItems({"Frames", "Annotation keyframes"});
    unit->setCurrentIndex(settings.ghostByAnnotations ? 1 : 0);
    form->addRow("Count units", unit);
    auto previous = new QSpinBox(&dialog), next = new QSpinBox(&dialog);
    previous->setRange(0, 1000);
    next->setRange(0, 1000);
    previous->setValue(settings.ghostPrevious);
    next->setValue(settings.ghostNext);
    form->addRow("Previous", previous);
    form->addRow("Next", next);
    QColor previousColor = settings.ghostPreviousTint, nextColor = settings.ghostNextTint;
    auto previousButton = new QPushButton(previousColor.name(), &dialog),
         nextButton = new QPushButton(nextColor.name(), &dialog);
    form->addRow("Previous tint", previousButton);
    form->addRow("Next tint", nextButton);
    connect(previousButton, &QPushButton::clicked, &dialog, [&] {
        auto color = QColorDialog::getColor(previousColor, &dialog, "Previous annotation tint");
        if (color.isValid()) {
            previousColor = color;
            previousButton->setText(color.name());
        }
    });
    connect(nextButton, &QPushButton::clicked, &dialog, [&] {
        auto color = QColorDialog::getColor(nextColor, &dialog, "Next annotation tint");
        if (color.isValid()) {
            nextColor = color;
            nextButton->setText(color.name());
        }
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    settings.ghostEnabled = enabled->isChecked();
    settings.ghostByAnnotations = unit->currentIndex() == 1;
    settings.ghostPrevious = previous->value();
    settings.ghostNext = next->value();
    settings.ghostPreviousTint = previousColor;
    settings.ghostNextTint = nextColor;
    m_session->setViewerSettings(settings);
}
void MainWindow::createMenus() {
    auto a = [this](QString id, QString label, QString icon, QList<QKeySequence> keys,
                    std::function<void()> f,
                    bool check = false) { return action(id, label, icon, keys, f, check); };
    auto file = menuBar()->addMenu("&File");
    file->addAction(a("import", "Import File(s)…", "import", {QKeySequence("Ctrl+I")},
                      [this] { importFiles(); }));
    file->addAction(a("importFolder", "Import Folder…", "folder", {}, [this] { importFolder(); }));
    file->addAction(
        a("importSequence", "Import Image Sequence…", "media", {}, [this] { importSequence(); }));
    file->addSeparator();
    file->addAction(a("new", "New Project", "", {QKeySequence::New}, [this] {
        if (confirmClose())
            m_session->newProject();
    }));
    file->addAction(
        a("open", "Open Project…", "folder", {QKeySequence::Open}, [this] { openProject(); }));
    file->addAction(a("importProject", "Import Project…", "", {}, [this] {
        QString path = LocalFileDialog::getOpenFileName(this, "Import OpenFrame project", {},
                                                        "OpenFrame project (*.oframe *.json)");
        if (!path.isEmpty())
            m_session->importProject(path);
    }));
    file->addAction(a("save", "Save Project", "save", {QKeySequence::Save}, [this] {
        if (m_session->projectPath().isEmpty())
            saveAs();
        else
            m_session->saveProject();
    }));
    file->addAction(
        a("saveAs", "Save Project As…", "", {QKeySequence::SaveAs}, [this] { saveAs(); }));
    file->addSeparator();
    file->addAction(
        a("export", "Export to File…", "", {QKeySequence("Ctrl+E")}, [this] { exportDialog(); }));
    auto capture = file->addMenu("Capture Image");
    capture->addAction(
        a("captureClipboard", "To Clipboard", "", {QKeySequence("Ctrl+Shift+C")}, [this] {
            auto im = m_session->displayImage();
            if (!im.isNull()) {
                QApplication::clipboard()->setImage(im);
                statusBar()->showMessage("Viewer image copied", 3000);
            }
        }));
    capture->addAction(a("captureFile", "To File…", "", {}, [this] {
        auto im = m_session->displayImage();
        if (im.isNull())
            return;
        auto path = LocalFileDialog::getSaveFileName(
            this, "Capture viewer",
            QString("frame_%1.png").arg(m_session->currentFrame(), 5, 10, QChar('0')),
            "PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif)");
        if (!path.isEmpty() && !im.save(path))
            showError("Could not save viewer image.");
    }));
    file->addAction(a("newInstance", "Open New Instance", "", {}, [] {
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    }));
    file->addSeparator();
    m_recentFilesMenu = file->addMenu("Recent Files");
    m_recentProjectsMenu = file->addMenu("Recent Projects");
    rebuildRecentMenus();
    file->addSeparator();
    file->addAction(a("exit", "Exit", "", {QKeySequence("Alt+F4")}, [this] { close(); }));
    auto edit = menuBar()->addMenu("&Edit");
    edit->addAction(a("undo", "Undo", "undo", {QKeySequence::Undo}, [this] {
        activeUndoStack()->undo();
        refreshUndoActions();
    }));
    edit->addAction(
        a("redo", "Redo", "redo", {QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")}, [this] {
            activeUndoStack()->redo();
            refreshUndoActions();
        }));
    edit->addSeparator();
    edit->addAction(a("rangeStart", "Range Start to Current", "", {QKeySequence("I")}, [this] {
        auto out = m_session->project().rangeOut < 0 ? m_session->durationFrames() - 1
                                                     : m_session->project().rangeOut;
        m_session->setRange(m_session->currentFrame(), std::max(m_session->currentFrame(), out));
    }));
    edit->addAction(a("rangeEnd", "Range End to Current", "", {QKeySequence("O")}, [this] {
        m_session->setRange(std::min(m_session->currentFrame(), m_session->project().rangeIn),
                            m_session->currentFrame());
    }));
    edit->addAction(a("rangeReset", "Reset Range", "", {QKeySequence("Ctrl+R")},
                      [this] { m_session->setRange(0, m_session->durationFrames() - 1); }));
    edit->addSeparator();
    edit->addAction(a(
        "alwaysOnTop", "Always on Top", "", {},
        [this] {
            bool checked = m_actions["alwaysOnTop"]->isChecked();
            setWindowFlag(Qt::WindowStaysOnTopHint, checked);
            show();
        },
        true));
    edit->addAction(a("ownerProperties", "Source / Timeline Properties…", "", {},
                      [this] { editSelectedProperties(); }));
    edit->addAction(
        a("projectSettings", "Project Settings…", "", {}, [this] { projectSettingsDialog(); }));
    edit->addAction(a("preferences", "Preferences…", "settings", {QKeySequence("Ctrl+,")},
                      [this] { preferencesDialog(); }));
    auto playback = menuBar()->addMenu("&Playback");
    auto speedMenu = playback->addMenu("Speed");
    for (double rate : {.0625, .125, .25, .5, 1., 1.5, 2., 4.})
        speedMenu->addAction(
            a(QString("speed%1").arg(rate), QString("%1×").arg(rate), "", {}, [this, rate] {
                m_session->setPlaybackRate(rate);
                m_speed->setCurrentText(QString::number(rate) + "×");
            }));
    playback->addSeparator();
    playback->addAction(a("play", "Play / Pause", "play", {QKeySequence("Space")}, [this] {
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            m_actions["playBookmarks"]->trigger();
            return;
        }
        m_session->togglePlayback();
    }));
    playback->addAction(a("playBookmarks", "Play / Pause (Bookmarks Only)", "",
                          {QKeySequence("Ctrl+Space"), QKeySequence("Shift+Space")}, [this] {
                              if (m_session->playing() && m_session->bookmarkPlayback())
                                  m_session->pause();
                              else
                                  m_session->playBookmarks();
                          }));
    playback->addAction(a("forward", "Play Forwards", "play", {QKeySequence("L")}, [this] {
        m_session->setPlaybackRate(std::abs(m_session->playbackRate()));
        m_session->play();
    }));
    playback->addAction(a("backward", "Play Backwards", "reverse", {QKeySequence("J")}, [this] {
        m_session->setPlaybackRate(-std::abs(m_session->playbackRate()));
        m_session->play();
    }));
    playback->addAction(a("pause", "Pause", "pause", {QKeySequence("K")}, [this] {
        m_bookmarkTimer->stop();
        m_session->pause();
    }));
    playback->addAction(a("stepSize", "Set Playback Frame Step…", "", {}, [this] {
        bool ok = false;
        int step = QInputDialog::getInt(this, "Playback Frame Step", "Frames per playback update:",
                                        m_session->playbackFrameStep(), 1, 10000, 1, &ok);
        if (ok) {
            m_session->setPlaybackFrameStep(step);
            QSettings().setValue("playback/frameStep", step);
        }
    }));
    auto step = playback->addMenu("Frame Step");
    step->addAction(a("stepForward", "Step Forwards", "step", {QKeySequence("Right")},
                      [this] { m_session->step(1); }));
    step->addAction(a("stepBack", "Step Backwards", "back", {QKeySequence("Left")},
                      [this] { m_session->step(-1); }));
    step->addAction(a("multiForward", "Multi-Step Forwards", "",
                      {QKeySequence("Alt+Right"), QKeySequence("Shift+Right")},
                      [this] { m_session->step(10); }));
    step->addAction(a("multiBack", "Multi-Step Backwards", "",
                      {QKeySequence("Alt+Left"), QKeySequence("Shift+Left")},
                      [this] { m_session->step(-10); }));
    playback->addSeparator();
    playback->addAction(
        a("start", "Go to Start", "start", {QKeySequence("Home")}, [this] { m_session->seek(0); }));
    playback->addAction(a("end", "Go to End", "end", {QKeySequence("End")},
                          [this] { m_session->seek(m_session->durationFrames() - 1); }));
    playback->addAction(a("rangeGoStart", "Go to Range Start", "", {QKeySequence("Ctrl+Home")},
                          [this] { m_session->seek(m_session->project().rangeIn); }));
    playback->addAction(a("rangeGoEnd", "Go to Range End", "", {QKeySequence("Ctrl+End")},
                          [this] { m_session->seek(m_session->project().rangeOut); }));
    playback->addAction(a("previousEdit", "Go to Previous Edit", "", {QKeySequence("Ctrl+Left")},
                          [this] { jumpEdit(-1); }));
    playback->addAction(a("nextEdit", "Go to Next Edit", "", {QKeySequence("Ctrl+Right")},
                          [this] { jumpEdit(1); }));
    auto looping = playback->addMenu("Looping");
    auto group = new QActionGroup(this);
    int loopIndex = 0;
    for (auto mode : {LoopMode::Once, LoopMode::Loop, LoopMode::PingPong}) {
        QString label = QStringList{"Play Once", "Loop", "Ping-Pong"}[loopIndex++];
        auto ac = a(
            "loop" + QString::number(int(mode)), label, "", {},
            [this, mode] { m_session->setLoopMode(mode); }, true);
        group->addAction(ac);
        looping->addAction(ac);
    }
    looping->addAction(a("cycleLoop", "Cycle Loop Mode", "loop", {QKeySequence("Ctrl+L")}, [this] {
        m_session->setLoopMode(LoopMode((int(m_session->project().loopMode) + 1) % 3));
    }));
    looping->addSeparator();
    looping->addAction(a("loopDelay", "Loop Delay…", "", {}, [this] { loopDelayDialog(); }));
    looping->addAction(a(
        "loopHoldEmpty", "Hold Empty Frame During Delay", "", {},
        [this] {
            m_session->setLoopDelay(m_session->project().loopDelayMs,
                                    m_actions["loopHoldEmpty"]->isChecked());
        },
        true));
    for (auto item : {std::pair{QString("loopStep"), QString("Loop on Step / Scrub")},
                      std::pair{QString("loopRange"), QString("Loop Step / Scrub to Range")},
                      std::pair{QString("lockRange"), QString("Lock Step / Scrub to Range")},
                      std::pair{QString("autoplay"), QString("Autoplay")}}) {
        auto key = item.first;
        playback->addAction(a(
            key, item.second, "", {},
            [this, key] {
                bool enabled = m_actions[key]->isChecked();
                if (key == "loopStep")
                    m_session->setLoopOnStepScrub(enabled);
                if (key == "loopRange")
                    m_session->setLoopStepScrubToRange(enabled);
                if (key == "lockRange")
                    m_session->setLockStepScrubToRange(enabled);
                if (key == "autoplay")
                    m_session->setAutoplay(enabled);
                QSettings().setValue("playback/" + key, enabled);
            },
            true));
    }
    auto audio = menuBar()->addMenu("&Audio");
    audio->addAction(a("volumeUp", "Increase Volume", "", {QKeySequence("Up")},
                       [this] { m_volume->setValue(std::min(100, m_volume->value() + 5)); }));
    audio->addAction(a("volumeDown", "Decrease Volume", "", {QKeySequence("Down")},
                       [this] { m_volume->setValue(std::max(0, m_volume->value() - 5)); }));
    audio->addAction(a(
        "mute", "Mute", "sound", {QKeySequence("M")},
        [this] { m_session->setAudioEnabled(!m_actions["mute"]->isChecked()); }, true));
    audio->addAction(a(
        "audioScrub", "Audio Scrub", "", {QKeySequence("Shift+S")},
        [this] { m_session->setAudioScrubEnabled(m_actions["audioScrub"]->isChecked()); }, true));
    m_actions["audioScrub"]->setChecked(true);
    audio->addAction(a(
        "audioTimeStretching", "Preserve Pitch When Changing Speed", "", {},
        [this] {
            const bool enabled = m_actions["audioTimeStretching"]->isChecked();
            m_session->setAudioTimeStretchingEnabled(enabled);
            QSettings().setValue("audio/timeStretching", enabled);
        },
        true));
    auto tracks = audio->addMenu("Source Audio Track");
    tracks->setObjectName("sourceAudioTracks");
    connect(tracks, &QMenu::aboutToShow, this, [this, tracks] {
        tracks->clear();
        for (auto group :
             tracks->findChildren<QActionGroup *>(QString(), Qt::FindDirectChildrenOnly))
            delete group;
        QString id = selectedSourceId();
        const auto &project = m_session->project();
        if (id.isEmpty())
            id = m_session->activeOwnerId();
        if (!project.source(id)) {
            const auto evaluated = evaluateAssignment(project, id, m_session->currentFrame());
            if (evaluated)
                id = evaluated->audioSourceId.isEmpty() ? evaluated->sourceId
                                                        : evaluated->audioSourceId;
        }
        const auto source = project.source(id);
        if (!source || source->info.audioTracks.isEmpty()) {
            tracks->addAction("No audio tracks in this source")->setEnabled(false);
            return;
        }
        auto group = new QActionGroup(tracks);
        auto addTrack = [&](const QString &label, int stream) {
            auto entry = tracks->addAction(label);
            entry->setCheckable(true);
            entry->setChecked(source->selectedAudioTrack == stream);
            entry->setData(stream);
            group->addAction(entry);
            connect(entry, &QAction::triggered, this,
                    [this, id, stream] { m_session->setSourceAudioTrack(id, stream); });
        };
        addTrack("Default track", -1);
        for (const auto &track : source->info.audioTracks)
            addTrack(QString("Track %1 · %2 · %3 Hz · %4 ch%5%6")
                         .arg(track.streamIndex)
                         .arg(track.codec)
                         .arg(track.sampleRate)
                         .arg(track.channels)
                         .arg(track.language.isEmpty() ? QString() : " · " + track.language)
                         .arg(track.title.isEmpty() ? QString() : " · " + track.title),
                     track.streamIndex);
    });
    auto video = menuBar()->addMenu("&Video");
    auto modes = video->addMenu("Layout");
    struct ModeItem {
        const char *id;
        const char *label;
        ViewerMode mode;
        bool sideBySide = false;
    };
    for (auto item : {ModeItem{"single", "Single A", ViewerMode::Single},
                      {"singleB", "Single B", ViewerMode::SingleB},
                      {"ab", "Active", ViewerMode::AB},
                      {"wipe", "Wipe", ViewerMode::Wipe},
                      {"blend", "Transparency", ViewerMode::Blend},
                      {"split", "Horizontal", ViewerMode::Split, true},
                      {"splitVertical", "Vertical", ViewerMode::Split},
                      {"grid", "Grid", ViewerMode::Grid}})
        modes->addAction(a(item.id, item.label, item.id, {}, [this, item] {
            changeViewer([=](ViewerSettings &v) {
                v.mode = item.mode;
                // The saved flag names the divider, which is vertical for side-by-side panes.
                v.splitVertical = item.sideBySide;
            });
        }));
    modes->addAction(
        a("swapViewers", "Swap Timelines A/B", "swap", {QKeySequence("Ctrl+Tab")}, [this] {
            changeViewer([](ViewerSettings &v) {
                while (v.assignments.size() < 2)
                    v.assignments.append(QString{});
                v.assignments.swapItemsAt(0, 1);
            });
        }));
    auto wipe = video->addMenu("Wipe");
    wipe->addAction(
        a("wipeOptions", "Wipe Options…", "", {}, [this] { editWipeSettings(m_session, this); }));
    wipe->addAction(a("resetWipe", "Reset Wipe", "", {}, [this] {
        changeViewer([](ViewerSettings &v) {
            v.wipe = .5;
            v.wipePivotY = .5;
            v.wipeAngleDegrees = 0;
        });
    }));
    wipe->addAction(a("centerWipeHorizontal", "Center Horizontally", "", {},
                      [this] { changeViewer([](ViewerSettings &v) { v.wipe = .5; }); }));
    wipe->addAction(a("centerWipeVertical", "Center Vertically", "", {},
                      [this] { changeViewer([](ViewerSettings &v) { v.wipePivotY = .5; }); }));
    for (int angle : {0, 90, 180, 270})
        wipe->addAction(a("wipeAngle" + QString::number(angle), QString("Rotate to %1°").arg(angle),
                          "", {}, [this, angle] {
                              changeViewer([=](ViewerSettings &v) { v.wipeAngleDegrees = angle; });
                          }));
    wipe->addSeparator();
    for (auto item : {std::pair{QString("wipeSnap"), QString("Snap to Center / Right Angles")},
                      std::pair{QString("wipeGuides"), QString("Show Wipe Guides")}})
        wipe->addAction(a(
            item.first, item.second, "", {},
            [this, item] {
                QSettings().setValue("viewer/" + item.first, m_actions[item.first]->isChecked());
                m_viewer->setWipeOptions(m_actions["wipeSnap"]->isChecked(),
                                         m_actions["wipeGuides"]->isChecked());
            },
            true));
    video->addAction(a(
        "pictureType", "Picture Type (I / P / B)", "", {},
        [this] {
            changeViewer([this](ViewerSettings &v) {
                v.showPictureType = m_actions["pictureType"]->isChecked();
            });
        },
        true));
    video->addAction(a("displayColor", "HDR / Display Color…", "", {},
                       [this] { editColorSettings(m_session, this); }));
    auto ann = video->addMenu("Annotations");
    auto visibility = new QActionGroup(this);
    for (int i = 0; i < 3; ++i) {
        auto ac = a(
            "annotationsVisibility" + QString::number(i),
            QStringList{"On", "Off During Playback", "Off"}[i], "", {},
            [this, i] {
                m_hideAnnotationsWhilePlaying = i == 1;
                m_session->setHideAnnotationsDuringPlayback(i == 1);
                changeViewer([=](ViewerSettings &v) { v.showAnnotations = i != 2; });
            },
            true);
        ann->addAction(ac);
        visibility->addAction(ac);
        if (i == 1)
            ac->setChecked(true);
    }
    auto overlays = video->addMenu("Overlays");
    auto overlayGroup = new QActionGroup(this);
    const QStringList overlayKeys{"Off",     "Safe",   "Grid", "Center", "DynamicSymmetry",
                                  "PhiGrid", "Custom", "Image"};
    const QStringList overlayNames{"Off",      "Safe Area", "Grid", "Center", "Dynamic Symmetry",
                                   "Phi Grid", "Custom",    "Image"};
    for (int i = 0; i < overlayKeys.size(); ++i) {
        auto item = a(
            "overlay" + overlayKeys[i], overlayNames[i], "", {},
            [this, i] { setOverlayType(OverlayType(i)); }, true);
        overlays->addAction(item);
        overlayGroup->addAction(item);
    }
    overlays->addSeparator();
    overlays->addAction(a("toggleOverlay", "Toggle Last Overlay", "", {}, [this] {
        setOverlayType(m_session->project().viewer.overlay.type == OverlayType::Off
                           ? m_lastOverlayType
                           : OverlayType::Off);
    }));
    overlays->addAction(
        a("overlayOptions", "Overlay Options…", "", {}, [this] { overlayDialog(); }));
    auto sizes = video->addMenu("Size");
    for (auto item :
         {std::pair{QString("Original Size"), 1.0}, std::pair{QString("Half Size"), .5},
          std::pair{QString("Quarter Size"), .25}, std::pair{QString("Double Size"), 2.0}})
        sizes->addAction(a("size" + QString::number(item.second), item.first, "", {}, [this, item] {
            QSize size = m_session->project().viewer.outputSize;
            QString id = m_session->activeOwnerId();
            if (auto source = m_session->project().source(id); source && source->info.width > 0)
                size = QSize(source->info.width, source->info.height);
            size = QSize(int(size.width() * item.second), int(size.height() * item.second));
            auto available = screen()->availableGeometry().size();
            resize(std::min(available.width(), width() - m_viewer->width() + size.width()),
                   std::min(available.height(), height() - m_viewer->height() + size.height()));
        }));
    video->addAction(a("fullscreen", "Fullscreen", "fit", {QKeySequence("F11")},
                       [this] { isFullScreen() ? showNormal() : showFullScreen(); }));
    auto pan = video->addMenu("Pan / Zoom");
    pan->addAction(a("zoomIn", "Zoom In", "zoom", {QKeySequence("Ctrl++"), QKeySequence("Ctrl+=")},
                     [this] { m_viewer->changeZoom(1.2); }));
    pan->addAction(a("zoomOut", "Zoom Out", "", {QKeySequence("Ctrl+-")},
                     [this] { m_viewer->changeZoom(1 / 1.2); }));
    pan->addAction(a(
        "togglePan", "Toggle Pan / Zoom", "", {QKeySequence("/")},
        [this] { changeViewer([](ViewerSettings &v) { v.panZoom.enabled = !v.panZoom.enabled; }); },
        true));
    pan->addAction(a("resetView", "Reset Pan / Zoom", "fit", {QKeySequence("Ctrl+/")},
                     [this] { m_viewer->resetTransform(); }));
    for (auto item : {std::pair{QString("Left"), QPointF(-.05, 0)},
                      {QString("Right"), QPointF(.05, 0)},
                      {QString("Up"), QPointF(0, -.05)},
                      {QString("Down"), QPointF(0, .05)}})
        pan->addAction(a("pan" + item.first, "Pan " + item.first, "", {}, [this, item] {
            changeViewer([&](ViewerSettings &v) {
                v.panZoom.enabled = true;
                v.panZoom.pan += item.second;
            });
        }));
    video->addAction(a("rotateCW", "Rotate CW", "rotate", {}, [this] {
        changeViewer([](ViewerSettings &v) {
            auto &rotation = v.transforms[activeViewerPane(v)].rotation;
            rotation = (rotation + 90) % 360;
        });
    }));
    video->addAction(a("rotateCCW", "Rotate CCW", "", {}, [this] {
        changeViewer([](ViewerSettings &v) {
            auto &rotation = v.transforms[activeViewerPane(v)].rotation;
            rotation = (rotation + 270) % 360;
        });
    }));
    for (auto key : {QString("Horizontal"), QString("Vertical")})
        video->addAction(a("flip" + key, "Flip " + key, "", {}, [this, key] {
            changeViewer([=](ViewerSettings &v) {
                auto &t = v.transforms[activeViewerPane(v)];
                if (key == "Horizontal")
                    t.flipHorizontal = !t.flipHorizontal;
                else
                    t.flipVertical = !t.flipVertical;
            });
        }));
    video->addAction(a("resetFlip", "Reset Flip / Rotate", "", {}, [this] {
        changeViewer([](ViewerSettings &v) {
            for (auto &t : v.transforms) {
                t.rotation = 0;
                t.flipHorizontal = false;
                t.flipVertical = false;
            }
        });
    }));
    auto bookmarks = menuBar()->addMenu("&Bookmarks");
    bookmarks->addAction(a("bookmark", "Toggle Bookmark", "bookmark", {QKeySequence("B")},
                           [this] { toggleBookmark(false); }));
    bookmarks->addAction(a("rangeBookmark", "Toggle Range Bookmark", "", {QKeySequence("Shift+B")},
                           [this] { toggleBookmark(true); }));
    bookmarks->addSeparator();
    bookmarks->addAction(a("cutBookmark", "Cut Bookmark", "", {}, [this] { copyBookmark(true); }));
    bookmarks->addAction(
        a("copyBookmark", "Copy Bookmark", "", {}, [this] { copyBookmark(false); }));
    bookmarks->addAction(a("pasteBookmark", "Paste Bookmark", "", {}, [this] { pasteBookmark(); }));
    bookmarks->addSeparator();
    bookmarks->addAction(a("previousBookmark", "Previous Bookmark", "", {QKeySequence(",")},
                           [this] { jumpBookmark(-1); }));
    bookmarks->addAction(
        a("nextBookmark", "Next Bookmark", "", {QKeySequence(".")}, [this] { jumpBookmark(1); }));
    bookmarks->addAction(a("previousRange", "Previous Range Bookmark", "",
                           {QKeySequence("Shift+,")}, [this] { jumpBookmark(-1, true); }));
    bookmarks->addAction(a("nextRange", "Next Range Bookmark", "", {QKeySequence("Shift+.")},
                           [this] { jumpBookmark(1, true); }));
    auto bookmarkFilters = bookmarks->addMenu("Filter");
    auto bookmarkFilterGroup = new QActionGroup(this);
    const QStringList filterNames{"All", "Empty", "With Notes", "With Annotations"};
    for (int i = 0; i < filterNames.size(); ++i) {
        auto filter = a(
            "bookmarkFilter" + QString::number(i), filterNames[i], "", {},
            [this, i] { setBookmarkFilter(BookmarkFilter(i)); }, true);
        bookmarkFilterGroup->addAction(filter);
        bookmarkFilters->addAction(filter);
    }
    bookmarks->addAction(a("removeEmptyBookmarks", "Remove Empty Bookmarks", "delete", {},
                           [this] { removeEmptyBookmarks(); }));
    auto notes = bookmarks->addMenu("Notes");
    notes->addAction(
        a("note", "Add / Edit Note…", "note", {QKeySequence("Ctrl+W")}, [this] { editNote(); }));
    notes->addAction(a("importNotes", "Import CSV…", "", {}, [this] {
        auto path = LocalFileDialog::getOpenFileName(this, "Import notes", {}, "CSV (*.csv)");
        if (!path.isEmpty() && importNotesCsvDialog(m_session, this, path))
            m_notesDock->show();
    }));
    notes->addAction(a("exportNotes", "Export CSV…", "", {}, [this] {
        auto path =
            LocalFileDialog::getSaveFileName(this, "Export notes", "notes.csv", "CSV (*.csv)");
        if (!path.isEmpty() && m_session->saveNotesCsv(path))
            revealExportedFile(path);
    }));
    auto tools = menuBar()->addMenu("&Tools");
    for (auto dock : {m_projectDock, m_detailsDock, m_notesDock, m_consoleDock}) {
        auto toggle = dock->toggleViewAction();
        QString id = "toggle" + dock->objectName();
        toggle->setObjectName(id);
        if (dock == m_notesDock)
            toggle->setShortcut(QKeySequence("Ctrl+Shift+B"));
        m_actions[id] = toggle;
        m_defaultShortcuts[id] = toggle->shortcuts();
        addAction(toggle);
        tools->addAction(toggle);
    }
    tools->addAction(a(
        "annotationsTools", "Annotation Tools", "pencil", {QKeySequence("F8")},
        [this] {
            bool on = !m_annotationBar->isVisible();
            m_annotationBar->setVisible(on);
            m_annotationOptions->setVisible(on);
            m_viewer->setAnnotationsEnabled(on);
            m_actions["annotationsTools"]->setChecked(on);
        },
        true));
    for (auto entry : {std::pair{QString("eyedropper"), ViewerWidget::TransientTool::Eyedropper},
                       {QString("laserPointer"), ViewerWidget::TransientTool::Laser}}) {
        const bool eyedropper = entry.second == ViewerWidget::TransientTool::Eyedropper;
        tools->addAction(a(
            entry.first, eyedropper ? "Eyedropper" : "Laser Pointer",
            eyedropper ? "eyedropper" : "laser", {QKeySequence(eyedropper ? "Shift+I" : "Shift+L")},
            [this, entry] {
                m_annotationBar->show();
                m_annotationOptions->show();
                m_viewer->setAnnotationsEnabled(true);
                m_viewer->setTransientTool(m_viewer->transientTool() == entry.second
                                               ? ViewerWidget::TransientTool::None
                                               : entry.second);
                m_viewer->setFocus();
            },
            true));
    }
    tools->addAction(a("cancelTransientTool", "Cancel Eyedropper / Laser", "",
                       {QKeySequence("Esc")},
                       [this] { m_viewer->setTransientTool(ViewerWidget::TransientTool::None); }));
    tools->addAction(a("toggleGlobalTime", "Global Time Control", "", {},
                       [this] { m_timeline->setVisible(!m_timeline->isVisible()); }));
    tools->addAction(a("toggleRangeTime", "Range Time Control", "", {},
                       [this] { m_rangeTimeline->setVisible(!m_rangeTimeline->isVisible()); }));
    tools->addAction(a("swapTime", "Swap Time Controls", "", {QKeySequence("T")}, [this] {
        bool old = m_timeline->isVisible();
        m_timeline->setVisible(!old);
        m_rangeTimeline->setVisible(old);
    }));
    m_actions["cancelTransientTool"]->setEnabled(false);
    tools->addAction(a("togglePlaybackBar", "Playback Controls", "", {},
                       [this] { m_playbackBar->setVisible(!m_playbackBar->isVisible()); }));
    tools->addAction(a("toggleViewerBar", "Viewer Controls", "", {},
                       [this] { m_viewerBar->setVisible(!m_viewerBar->isVisible()); }));
    tools->addAction(a("toggleMenu", "Menu Bar", "", {QKeySequence("Ctrl+Shift+M")},
                       [this] { menuBar()->setVisible(!menuBar()->isVisible()); }));
    tools->addAction(a("moveSidebar", "Move Side Bar Right / Left", "", {}, [this] {
        m_sideRight = !m_sideRight;
        addDockWidget(m_sideRight ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea,
                      m_projectDock);
        splitDockWidget(m_projectDock, m_detailsDock, Qt::Vertical);
    }));
    tools->addAction(a(
        "mediaInfo", "Media Info", "", {},
        [this] {
            changeViewer(
                [this](ViewerSettings &v) { v.showOverlay = m_actions["mediaInfo"]->isChecked(); });
        },
        true));
    m_actions["mediaInfo"]->setChecked(true);
    tools->addAction(a("resetLayout", "Reset Layout", "", {}, [this] { resetLayout(); }));
    tools->addSeparator();
    tools->addAction(a("maya", "Maya Tools…", "", {}, [this] { mayaDialog(); }));
    auto help = menuBar()->addMenu("&Help");
    help->addAction(a("documentation", "Documentation…", "", {QKeySequence::HelpContents},
                      [this] { documentationDialog(); }));
    help->addAction(a("about", "About OpenFrame…", "", {}, [this] {
        QMessageBox::about(
            this, "About OpenFrame",
            QString("<h3>OpenFrame %1</h3><p>Independent, open-source animation "
                    "review.</p><p>Qt 6 / C++20 · FFmpeg local media engine</p><p>No "
                    "activation. Local API binds to 127.0.0.1 only.</p><p>Full feature and "
                    "visual parity with any reference product has not been verified.</p>")
                .arg(QCoreApplication::applicationVersion()));
    }));
}
class ProjectTree final : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;
    std::function<void(QString, QString, QStringList)> itemsDropped;
    std::function<void(QStringList)> filesDropped;

  protected:
    void dragEnterEvent(QDragEnterEvent *e) override {
        if (e->mimeData()->hasUrls())
            e->acceptProposedAction();
        else
            QTreeWidget::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent *e) override {
        e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override {
        if (e->mimeData()->hasUrls()) {
            QStringList paths;
            for (auto url : e->mimeData()->urls())
                if (url.isLocalFile())
                    paths << url.toLocalFile();
            if (filesDropped)
                filesDropped(paths);
            e->acceptProposedAction();
            return;
        }
        auto target = itemAt(e->position().toPoint());
        QStringList ids;
        for (auto item : selectedItems())
            ids << item->data(0, IdRole).toString();
        if (target && itemsDropped)
            itemsDropped(target->data(0, KindRole).toString(), target->data(0, IdRole).toString(),
                         ids);
        e->acceptProposedAction();
    }
};
void MainWindow::createDocks() {
    m_projectDock = new QDockWidget("Project", this);
    m_projectDock->setObjectName("projectDock");
    m_projectDock->setMinimumWidth(210);
    auto projectWidget = new QWidget(m_projectDock);
    auto pl = new QVBoxLayout(projectWidget);
    pl->setContentsMargins(4, 3, 4, 4);
    pl->setSpacing(4);
    auto filter = new QLineEdit(projectWidget);
    filter->setPlaceholderText("Filter sources and timelines…");
    filter->setClearButtonEnabled(true);
    pl->addWidget(filter);
    auto tree = new ProjectTree(projectWidget);
    m_projectTree = tree;
    m_projectTree->setObjectName("projectTree");
    m_projectTree->setHeaderHidden(true);
    m_projectTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_projectTree->setDragEnabled(true);
    m_projectTree->setAcceptDrops(true);
    m_projectTree->setDragDropMode(QAbstractItemView::DragDrop);
    m_projectTree->setContextMenuPolicy(Qt::CustomContextMenu);
    pl->addWidget(m_projectTree, 1);
    auto newFolder = iconButton("folder", "New folder", projectWidget);
    auto newTimeline = iconButton("timeline", "New timeline", projectWidget);
    auto import = iconButton("import", "Import media", projectWidget);
    auto del = iconButton("delete", "Remove selected", projectWidget);
    auto buttons = row(projectWidget, {newFolder, newTimeline, import, del});
    pl->addWidget(buttons);
    m_projectDock->setWidget(projectWidget);
    addDockWidget(Qt::LeftDockWidgetArea, m_projectDock);
    connect(newFolder, &QToolButton::clicked, this, &MainWindow::addFolder);
    connect(newTimeline, &QToolButton::clicked, this, &MainWindow::addTimeline);
    connect(import, &QToolButton::clicked, this, &MainWindow::importFiles);
    connect(del, &QToolButton::clicked, this, &MainWindow::deleteSelected);
    connect(m_projectTree, &QTreeWidget::customContextMenuRequested, this,
            &MainWindow::projectContextMenu);
    connect(m_projectTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::refreshDetails);
    connect(m_projectTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        auto id = item->data(0, IdRole).toString(), kind = item->data(0, KindRole).toString();
        if (kind == "timeline") {
            m_session->setActiveTimeline(id);
            m_session->assignViewer(0, id);
        } else if (kind == "source")
            m_session->assignViewer(0, id);
    });
    connect(filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        QTreeWidgetItemIterator it(m_projectTree);
        while (*it) {
            auto item = *it;
            auto kind = item->data(0, KindRole).toString();
            item->setHidden((kind == "source" || kind == "timeline") &&
                            !item->text(0).contains(text, Qt::CaseInsensitive));
            ++it;
        }
    });
    tree->filesDropped = [this](QStringList paths) { importMediaPaths(paths); };
    tree->itemsDropped = [this](QString kind, QString target, QStringList ids) {
        if (kind == "timeline") {
            for (const auto &id : ids)
                if (m_session->project().source(id))
                    m_session->appendSource(id, target);
        } else if (kind == "folder" || kind == "sources" || kind == "timelines") {
            m_session->mutateProject("Move sources to folder", [=](Project &p) {
                for (auto &s : p.sources)
                    if (ids.contains(s.id))
                        s.folderId = kind == "folder" ? target : QString();
                for (auto &t : p.timelines)
                    if (ids.contains(t.id))
                        t.folderId = kind == "folder" ? target : QString();
                for (auto &f : p.folders)
                    if (ids.contains(f.id) && f.id != target) {
                        QString cursor = target;
                        bool cycle = false;
                        for (int limit = 0; limit < p.folders.size() + 1 && !cursor.isEmpty();
                             ++limit) {
                            if (cursor == f.id) {
                                cycle = true;
                                break;
                            }
                            QString parent;
                            for (const auto &pf : p.folders)
                                if (pf.id == cursor) {
                                    parent = pf.parentId;
                                    break;
                                }
                            cursor = parent;
                        }
                        if (!cycle)
                            f.parentId = kind == "folder" ? target : QString();
                    }
            });
        }
    };
    m_detailsDock = new QDockWidget("Details", this);
    m_detailsDock->setObjectName("detailsDock");
    auto tabs = new QTabWidget(m_detailsDock);
    auto sourcePane = new QWidget(tabs);
    auto sl = new QVBoxLayout(sourcePane);
    sl->setContentsMargins(8, 8, 8, 8);
    m_selectionLabel = new QLabel("No source selected", sourcePane);
    m_selectionLabel->setWordWrap(true);
    m_selectionLabel->setTextFormat(Qt::PlainText);
    m_thumbnail = new QLabel(sourcePane);
    m_thumbnail->setFixedHeight(112);
    m_thumbnail->setMinimumWidth(180);
    m_thumbnail->setAlignment(Qt::AlignCenter);
    m_thumbnail->setText("Viewer preview");
    m_thumbnail->setStyleSheet("background:#1b1d20;color:#6c737b;border:1px solid #41464d;");
    m_mediaInfo = new QLabel(sourcePane);
    m_mediaInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_mediaInfo->setWordWrap(true);
    m_mediaInfo->setTextFormat(Qt::PlainText);
    sl->addWidget(m_selectionLabel);
    sl->addWidget(m_thumbnail);
    sl->addWidget(m_mediaInfo);
    sl->addStretch();
    auto sourceScroll = new QScrollArea(tabs);
    sourceScroll->setWidgetResizable(true);
    sourceScroll->setFrameShape(QFrame::NoFrame);
    sourceScroll->setWidget(sourcePane);
    tabs->addTab(sourceScroll, "Properties");
    auto timelinePane = new QWidget(tabs);
    auto tl = new QVBoxLayout(timelinePane);
    tl->setContentsMargins(4, 4, 4, 4);
    m_clipTable = new QTableWidget(0, 3, timelinePane);
    m_clipTable->setObjectName("clipTable");
    m_clipTable->setHorizontalHeaderLabels({"Source", "In", "Out"});
    m_clipTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_clipTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_clipTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_clipTable->verticalHeader()->hide();
    m_clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_clipTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_clipTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_clipTable->setContextMenuPolicy(Qt::CustomContextMenu);
    tl->addWidget(m_clipTable, 1);
    auto addClip = iconButton("plus", "Append selected source", timelinePane);
    auto up = iconButton("up", "Move clip earlier", timelinePane);
    auto down = iconButton("down", "Move clip later", timelinePane);
    auto remove = iconButton("delete", "Remove clip", timelinePane);
    tl->addWidget(row(timelinePane, {addClip, up, down, remove}));
    tabs->addTab(timelinePane, "Timeline");
    m_detailsDock->setWidget(tabs);
    addDockWidget(Qt::LeftDockWidgetArea, m_detailsDock);
    splitDockWidget(m_projectDock, m_detailsDock, Qt::Vertical);
    connect(addClip, &QToolButton::clicked, this, &MainWindow::appendSelected);
    connect(up, &QToolButton::clicked, this, [this] { moveClip(-1); });
    connect(down, &QToolButton::clicked, this, [this] { moveClip(1); });
    connect(remove, &QToolButton::clicked, this, &MainWindow::removeClip);
    connect(m_clipTable, &QTableWidget::cellDoubleClicked, this, [this] { editClip(); });
    connect(m_clipTable, &QTableWidget::customContextMenuRequested, this,
            &MainWindow::clipContextMenu);
    m_notesDock = new QDockWidget("Notes & Bookmarks", this);
    m_notesDock->setObjectName("notesDock");
    auto notesWidget = new QWidget(m_notesDock);
    auto nl = new QVBoxLayout(notesWidget);
    nl->setContentsMargins(5, 5, 5, 5);
    auto addNote = iconButton("note", "Add / edit note (Ctrl+W)", notesWidget);
    auto addMark = iconButton("bookmark", "Toggle bookmark (B)", notesWidget);
    auto exportNotes = iconButton("save", "Export notes to CSV", notesWidget);
    nl->addWidget(row(notesWidget, {addNote, addMark, exportNotes}));
    m_bookmarkFilterCombo = new QComboBox(notesWidget);
    m_bookmarkFilterCombo->setObjectName("bookmarkFilter");
    m_bookmarkFilterCombo->addItems(
        {"All notes and bookmarks", "Empty bookmarks", "With notes", "With annotations"});
    m_bookmarkFilterCombo->setToolTip(
        "Filters the list, timeline markers and previous/next bookmark navigation.");
    nl->addWidget(m_bookmarkFilterCombo);
    connect(m_bookmarkFilterCombo, &QComboBox::activated, this,
            [this](int index) { setBookmarkFilter(BookmarkFilter(index)); });
    m_notesTable = new QTableWidget(0, 3, notesWidget);
    m_notesTable->setObjectName("notesTable");
    m_notesTable->setHorizontalHeaderLabels({"Frame", "Note", "Author"});
    m_notesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_notesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_notesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_notesTable->verticalHeader()->hide();
    m_notesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_notesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_notesTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_notesTable->setWordWrap(true);
    nl->addWidget(m_notesTable, 1);
    m_notesDock->setWidget(notesWidget);
    addDockWidget(Qt::RightDockWidgetArea, m_notesDock);
    connect(addNote, &QToolButton::clicked, this, [this] { editNote(); });
    connect(addMark, &QToolButton::clicked, this, [this] { toggleBookmark(false); });
    connect(exportNotes, &QToolButton::clicked, this,
            [this] { m_actions["exportNotes"]->trigger(); });
    connect(m_notesTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (!m_session->playing())
            m_session->seek(m_notesTable->item(row, 0)->data(Qt::UserRole).toLongLong());
    });
    connect(m_notesTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto item = m_notesTable->item(row, 0);
        m_session->seek(item->data(Qt::UserRole).toLongLong());
        if (item->data(KindRole) == "note")
            editNote(item->data(IdRole).toString());
        else {
            QString id = item->data(IdRole).toString();
            bool ok = false;
            auto text = QInputDialog::getMultiLineText(
                this, "Bookmark", "Note:", m_notesTable->item(row, 1)->text(), &ok);
            if (ok)
                m_session->mutateProject("Edit bookmark", [=](Project &p) {
                    for (auto &b : p.bookmarks)
                        if (b.id == id)
                            b.text = text;
                });
        }
    });
    connect(m_notesTable, &QTableWidget::customContextMenuRequested, this,
            &MainWindow::noteContextMenu);
    m_consoleDock = new QDockWidget("Console", this);
    m_consoleDock->setObjectName("consoleDock");
    m_console = new QTextEdit(m_consoleDock);
    m_console->setReadOnly(true);
    m_console->setFont(QFont("Consolas", 9));
    m_consoleDock->setWidget(m_console);
    addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
    resizeDocks({m_projectDock, m_detailsDock}, {340, 280}, Qt::Vertical);
    resizeDocks({m_projectDock}, {260}, Qt::Horizontal);
    resizeDocks({m_notesDock}, {310}, Qt::Horizontal);
}
void MainWindow::createToolbars() {
    m_viewerBar = addToolBar("Viewer Controls");
    m_viewerBar->setObjectName("viewerControls");
    m_viewerBar->setIconSize(QSize(18, 18));
    m_viewerBar->addAction(m_actions["import"]);
    m_viewerBar->addAction(m_actions["save"]);
    m_viewerBar->addSeparator();
    for (int i = 0; i < 4; ++i) {
        m_viewerBar->addWidget(new QLabel(QString(" %1 ").arg(QChar('A' + i)), m_viewerBar));
        m_viewerSelectors[i] = new QComboBox(m_viewerBar);
        m_viewerSelectors[i]->setObjectName(QString("viewerAssignment%1").arg(i));
        m_viewerSelectors[i]->setMinimumContentsLength(8);
        m_viewerSelectors[i]->setMaximumWidth(170);
        m_viewerSelectors[i]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_viewerBar->addWidget(m_viewerSelectors[i]);
        connect(m_viewerSelectors[i], qOverload<int>(&QComboBox::activated), this,
                [this, i](int index) {
                    m_session->assignViewer(i, m_viewerSelectors[i]->itemData(index).toString());
                    changeViewer([i](ViewerSettings &v) { v.activePane = i; });
                });
    }
    m_viewerBar->addSeparator();
    for (auto key : {"single", "singleB", "ab", "wipe", "blend", "split", "splitVertical",
                     "grid", "swapViewers"})
        m_viewerBar->addAction(m_actions[key]);
    m_mix = new QSlider(Qt::Horizontal, m_viewerBar);
    m_mix->setObjectName("comparisonMix");
    m_mix->setTracking(true);
    m_mix->setRange(0, 100);
    m_mix->setValue(50);
    m_mix->setMaximumWidth(80);
    m_mix->setToolTip("Wipe position / transparency");
    m_viewerBar->addWidget(m_mix);
    connect(m_mix, &QSlider::sliderPressed, this, [this] {
        m_mixPreviewToken = m_session->beginViewerPreview();
    });
    connect(m_mix, &QSlider::valueChanged, this, [this](int value) {
        if (m_refreshing || !m_mix->isEnabled())
            return;
        if (m_mix->isSliderDown()) {
            m_session->previewComparisonMix(m_mixPreviewToken, value / 100.0);
            return;
        }
        changeViewer([value](ViewerSettings &v) {
            if (v.mode == ViewerMode::Blend)
                v.opacity = value / 100.0;
            else if (v.mode == ViewerMode::Wipe)
                v.wipe = value / 100.0;
        });
    });
    connect(m_mix, &QSlider::sliderReleased, this, [this] {
        const auto token = m_mixPreviewToken;
        m_mixPreviewToken = 0;
        const auto &saved = m_session->project().viewer;
        const auto preview = m_session->displayedViewerSettings();
        if (saved.wipe == preview.wipe && saved.opacity == preview.opacity)
            m_session->clearViewerPreview(token);
        else
            m_session->commitViewerPreview(token, saved.mode == ViewerMode::Blend
                                                      ? "Adjust transparency" : "Adjust wipe");
    });
    m_viewerBar->addSeparator();
    m_viewerBar->addAction(m_actions["resetView"]);
    m_viewerBar->addAction(m_actions["rotateCW"]);
    m_viewerBar->addAction(m_actions["annotationsTools"]);
    m_viewerBar->addAction(m_notesDock->toggleViewAction());
    m_playbackBar = new QToolBar("Playback Controls", this);
    m_playbackBar->setObjectName("playbackControls");
    m_playbackBar->setIconSize(QSize(18, 18));
    addToolBar(Qt::BottomToolBarArea, m_playbackBar);
    auto spacer = new QWidget(m_playbackBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_playbackBar->addWidget(spacer);
    m_rangeIn = new QSpinBox(m_playbackBar);
    m_rangeIn->setRange(0, std::numeric_limits<int>::max());
    m_rangeIn->setMaximumWidth(92);
    m_rangeIn->setPrefix("In ");
    m_rangeIn->setToolTip("Playback range start");
    m_playbackBar->addWidget(m_rangeIn);
    m_playbackBar->addAction(action("transportStart", "Seek Start", "start", {}, [this] {
        m_session->seek(m_rangeTransport ? m_session->project().rangeIn : 0);
    }));
    m_playbackBar->addAction(m_actions["stepBack"]);
    m_playbackBar->addAction(m_actions["backward"]);
    m_playbackBar->addAction(m_actions["play"]);
    m_playbackBar->addAction(m_actions["stepForward"]);
    m_playbackBar->addAction(action("transportEnd", "Seek End", "end", {}, [this] {
        m_session->seek(m_rangeTransport && m_session->project().rangeOut >= 0
                            ? m_session->project().rangeOut
                            : m_session->durationFrames() - 1);
    }));
    m_frameSpin = new QSpinBox(m_playbackBar);
    m_frameSpin->setObjectName("currentFrame");
    m_frameSpin->setRange(0, std::numeric_limits<int>::max());
    m_frameSpin->setMinimumWidth(80);
    m_frameSpin->setMaximumWidth(100);
    m_frameSpin->setAlignment(Qt::AlignCenter);
    m_frameSpin->setToolTip("Current frame");
    m_playbackBar->addWidget(m_frameSpin);
    m_rangeOut = new QSpinBox(m_playbackBar);
    m_rangeOut->setRange(0, std::numeric_limits<int>::max());
    m_rangeOut->setMaximumWidth(100);
    m_rangeOut->setPrefix("Out ");
    m_rangeOut->setToolTip("Playback range end");
    m_playbackBar->addWidget(m_rangeOut);
    m_playbackBar->addSeparator();
    m_speed = new QComboBox(m_playbackBar);
    m_speed->setObjectName("playbackSpeed");
    m_speed->addItems({"0.0625×", "0.125×", "0.25×", "0.5×", "1×", "1.5×", "2×", "4×"});
    m_speed->setCurrentText("1×");
    m_speed->setEditable(true);
    m_speed->setMaximumWidth(82);
    m_speed->setToolTip("Playback speed: 1/16× to 16×; negative values play backwards");
    m_playbackBar->addWidget(m_speed);
    m_loop = new QComboBox(m_playbackBar);
    m_loop->addItems({"Once", "Loop", "Ping-Pong"});
    m_loop->setCurrentIndex(1);
    m_loop->setMaximumWidth(100);
    m_playbackBar->addWidget(m_loop);
    m_playbackBar->addAction(m_actions["mute"]);
    m_volume = new QSlider(Qt::Horizontal, m_playbackBar);
    m_volume->setRange(0, 100);
    m_volume->setValue(100);
    m_volume->setMaximumWidth(75);
    m_volume->setToolTip("Audio volume");
    m_volumeControlAction = m_playbackBar->addWidget(m_volume);
    auto volumePopup = new QToolButton(m_playbackBar);
    volumePopup->setIcon(uiIcon("sound"));
    volumePopup->setToolTip("Volume");
    m_volumePopupAction = m_playbackBar->addWidget(volumePopup);
    m_volumePopupAction->setVisible(false);
    connect(volumePopup, &QToolButton::clicked, this, [this, volumePopup] {
        QMenu menu(this);
        auto slider = new QSlider(Qt::Vertical, &menu);
        slider->setRange(0, 100);
        slider->setValue(m_volume->value());
        slider->setMinimumHeight(130);
        auto widget = new QWidgetAction(&menu);
        widget->setDefaultWidget(slider);
        menu.addAction(widget);
        connect(slider, &QSlider::valueChanged, m_volume, &QSlider::setValue);
        menu.exec(volumePopup->mapToGlobal(QPoint(0, -150)));
    });
    auto spacer2 = new QWidget(m_playbackBar);
    spacer2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_playbackBar->addWidget(spacer2);
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int f) {
        if (!m_refreshing) {
            if (m_pauseOnSeek)
                m_session->pause();
            m_session->navigate(
                frameFromDisplay(m_session->project(), m_session->activeOwnerId(), f), true);
        }
    });
    connect(m_rangeIn, qOverload<int>(&QSpinBox::valueChanged), this, [this](int f) {
        if (!m_refreshing) {
            auto offset = frameDisplayOffset(m_session->project(), m_session->activeOwnerId());
            m_session->setRange(std::min(f, m_rangeOut->value()) - offset,
                                m_rangeOut->value() - offset);
        }
    });
    connect(m_rangeOut, qOverload<int>(&QSpinBox::valueChanged), this, [this](int f) {
        if (!m_refreshing) {
            auto offset = frameDisplayOffset(m_session->project(), m_session->activeOwnerId());
            m_session->setRange(m_rangeIn->value() - offset,
                                std::max(f, m_rangeIn->value()) - offset);
        }
    });
    connect(m_speed, &QComboBox::currentTextChanged, this, [this](QString text) {
        text.remove(QChar(0x00d7));
        bool ok = false;
        double v = text.toDouble(&ok);
        if (ok && std::abs(v) >= .0625 && std::abs(v) <= 16)
            m_session->setPlaybackRate(v);
    });
    connect(m_speed->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        QString text = m_speed->currentText();
        text.remove(QChar(0x00d7));
        bool ok = false;
        const double rate = text.toDouble(&ok);
        if (!ok || !std::isfinite(rate) || std::abs(rate) < .0625 || std::abs(rate) > 16) {
            const QSignalBlocker blocked(m_speed);
            m_speed->setCurrentText(QString::number(m_session->playbackRate(), 'g', 5) + "×");
            statusBar()->showMessage(
                "Playback speed must be between 1/16× and 16× in either direction", 5000);
        }
    });
    connect(m_loop, qOverload<int>(&QComboBox::activated), this,
            [this](int mode) { m_session->setLoopMode(LoopMode(mode)); });
    connect(m_volume, &QSlider::valueChanged, this,
            [this](int v) { m_session->setVolume(v / 100.0); });
    m_annotationBar = new QToolBar("Annotation Tools", this);
    m_annotationBar->setObjectName("annotationTools");
    m_annotationBar->setIconSize(QSize(20, 20));
    addToolBar(Qt::LeftToolBarArea, m_annotationBar);
    auto group = new QActionGroup(this);
    struct Tool {
        AnnotationTool value;
        const char *name;
        const char *icon;
    };
    for (auto item : {Tool{AnnotationTool::Pencil, "Pencil", "pencil"},
                      {AnnotationTool::Arrow, "Arrow", "arrow"},
                      {AnnotationTool::Rectangle, "Rectangle", "rectangle"},
                      {AnnotationTool::Ellipse, "Ellipse", "ellipse"},
                      {AnnotationTool::Text, "Text", "text"},
                      {AnnotationTool::Eraser, "Eraser", "erase"}}) {
        auto ac = action(
            "draw" + QString(item.name), item.name, item.icon, {},
            [this, item] { m_viewer->setAnnotationTool(item.value); }, true);
        group->addAction(ac);
        m_annotationBar->addAction(ac);
        if (item.value == AnnotationTool::Pencil)
            ac->setChecked(true);
    }
    for (auto name : {QString("eyedropper"), QString("laserPointer")}) {
        group->addAction(m_actions[name]);
        m_annotationBar->addAction(m_actions[name]);
    }
    m_annotationBar->addSeparator();
    auto layer = m_annotationBar->addAction("FG");
    layer->setToolTip("Foreground / background layer");
    layer->setCheckable(true);
    connect(layer, &QAction::toggled, this, [this, layer](bool bg) {
        layer->setText(bg ? "BG" : "FG");
        m_viewer->setAnnotationBackground(bg);
    });
    auto hold = action(
        "holdAnnotations", "Hold", "", {},
        [this] {
            bool enabled = m_actions["holdAnnotations"]->isChecked();
            changeViewer([=](ViewerSettings &v) { v.holdAnnotations = enabled; });
        },
        true);
    hold->setToolTip("Hold all foreground annotation frames until the next keyframe");
    m_annotationBar->addAction(hold);
    auto ghost = action(
        "ghostAnnotations", "Ghost", "eye", {},
        [this] {
            bool on = m_actions["ghostAnnotations"]->isChecked();
            changeViewer([=](ViewerSettings &v) { v.ghostEnabled = on; });
        },
        true);
    ghost->setToolTip("Onion skinning · right-click for previous / next counts and tint colors");
    m_annotationBar->addAction(ghost);
    auto ghostButton = m_annotationBar->widgetForAction(ghost);
    ghostButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ghostButton, &QWidget::customContextMenuRequested, this, [this] { ghostingDialog(); });
    m_annotationOptions = new QToolBar("Annotation Options", this);
    m_annotationOptions->setObjectName("annotationOptions");
    addToolBarBreak(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, m_annotationOptions);
    m_annotationOptions->addWidget(new QLabel("  Size ", m_annotationOptions));
    auto width = new QDoubleSpinBox(m_annotationOptions);
    width->setRange(.5, 120);
    width->setSingleStep(.5);
    width->setValue(4);
    width->setMaximumWidth(70);
    m_annotationOptions->addWidget(width);
    connect(width, qOverload<double>(&QDoubleSpinBox::valueChanged), m_viewer,
            &ViewerWidget::setAnnotationWidth);
    m_annotationOptions->addWidget(new QLabel(" Opacity ", m_annotationOptions));
    auto opacity = new QSpinBox(m_annotationOptions);
    opacity->setRange(1, 100);
    opacity->setValue(100);
    opacity->setSuffix("%");
    opacity->setMaximumWidth(75);
    m_annotationOptions->addWidget(opacity);
    connect(opacity, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int v) { m_viewer->setAnnotationOpacity(v / 100.0); });
    auto color = m_annotationOptions->addAction("Color");
    const QColor initialColor(QSettings().value("annotation/color", "#ffffcc40").toString());
    m_viewer->setAnnotationColor(initialColor.isValid() ? initialColor : QColor(255, 204, 64));
    color->setIcon(uiIcon("pencil", m_viewer->annotationColor()));
    connect(color, &QAction::triggered, this, [this, color] {
        auto c = QColorDialog::getColor(m_viewer->annotationColor(), this, "Annotation color",
                                        QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            m_viewer->setAnnotationColor(c);
            color->setIcon(uiIcon("pencil", c));
            QSettings().setValue("annotation/color", c.name(QColor::HexArgb));
        }
    });
    connect(m_viewer, &ViewerWidget::annotationColorPicked, this, [color](QColor picked) {
        color->setIcon(uiIcon("pencil", picked));
        QSettings().setValue("annotation/color", picked.name(QColor::HexArgb));
    });
    auto sample = new QLabel(m_annotationOptions);
    sample->setObjectName("eyedropperReadout");
    sample->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto sampleAction = m_annotationOptions->addWidget(sample);
    sampleAction->setVisible(false);
    connect(m_viewer, &ViewerWidget::colorSampled, this, [sample](QColor c) {
        sample->setText(c.isValid() ? QString(" RGB %1, %2, %3  %4 ")
                                          .arg(c.red())
                                          .arg(c.green())
                                          .arg(c.blue())
                                          .arg(c.name().toUpper())
                                    : " Point at the viewer; click to pick a color. ");
    });
    connect(m_viewer, &ViewerWidget::transientToolChanged, this, [this, sampleAction, sample] {
        const auto tool = m_viewer->transientTool();
        m_actions["eyedropper"]->setChecked(tool == ViewerWidget::TransientTool::Eyedropper);
        m_actions["laserPointer"]->setChecked(tool == ViewerWidget::TransientTool::Laser);
        m_actions["cancelTransientTool"]->setEnabled(tool != ViewerWidget::TransientTool::None);
        sampleAction->setVisible(tool == ViewerWidget::TransientTool::Eyedropper);
        sample->setText(" Point at the viewer; click to pick a color. ");
        if (tool == ViewerWidget::TransientTool::None) {
            const QStringList names{"Pencil", "Arrow", "Rectangle", "Ellipse", "Text", "Eraser"};
            m_actions["draw" + names[int(m_viewer->annotationTool())]]->setChecked(true);
        }
    });
    auto fill = m_annotationOptions->addAction("Background Fill");
    connect(fill, &QAction::triggered, this, [this] {
        auto color =
            QColorDialog::getColor(m_session->project().viewer.annotationBackground, this,
                                   "Annotation background fill", QColorDialog::ShowAlphaChannel);
        if (color.isValid())
            changeViewer([=](ViewerSettings &v) { v.annotationBackground = color; });
    });
    m_annotationOptions->addSeparator();
    m_annotationOptions->addAction(m_actions["undo"]);
    m_annotationOptions->addAction(m_actions["redo"]);
    auto clear = m_annotationOptions->addAction(uiIcon("delete"), "Clear Frame");
    connect(clear, &QAction::triggered, m_viewer, &ViewerWidget::clearCurrentAnnotations);
    connect(m_annotationBar, &QToolBar::visibilityChanged, this, [this](bool visible) {
        m_viewer->setAnnotationsEnabled(visible);
        m_actions["annotationsTools"]->setChecked(visible);
        m_annotationOptions->setVisible(visible);
    });
}
void MainWindow::refreshProject() {
    if (m_refreshing)
        return;
    m_refreshing = true;
    QSignalBlocker treeBlock(m_projectTree), inBlock(m_rangeIn), outBlock(m_rangeOut),
        frameBlock(m_frameSpin);
    QString selection;
    if (auto item = m_projectTree->currentItem())
        selection = item->data(0, IdRole).toString();
    m_projectTree->clear();
    const auto &p = m_session->project();
    auto timelines = new QTreeWidgetItem(m_projectTree, {"Timelines"});
    timelines->setData(0, KindRole, "timelines");
    timelines->setIcon(0, uiIcon("timeline"));
    timelines->setFlags(timelines->flags() & ~Qt::ItemIsDragEnabled);
    for (const auto &tl : p.timelines) {
        auto item = new QTreeWidgetItem(timelines, {tl.name});
        item->setData(0, IdRole, tl.id);
        item->setData(0, KindRole, "timeline");
        item->setIcon(0, uiIcon("timeline", tl.id == p.activeTimelineId ? QColor("#8dc4e0")
                                                                        : QColor("#b2bbc3")));
        if (tl.id == p.activeTimelineId) {
            auto f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        if (tl.id == selection)
            m_projectTree->setCurrentItem(item);
    }
    auto sources =
        new QTreeWidgetItem(m_projectTree, {QString("Media & Folders (%1)").arg(p.sources.size())});
    sources->setData(0, KindRole, "sources");
    sources->setIcon(0, uiIcon("folder"));
    sources->setFlags(sources->flags() & ~Qt::ItemIsDragEnabled);
    QHash<QString, QTreeWidgetItem *> folders;
    for (const auto &f : p.folders) {
        auto item = new QTreeWidgetItem(QStringList{f.name});
        item->setData(0, IdRole, f.id);
        item->setData(0, KindRole, "folder");
        item->setIcon(0, uiIcon("folder"));
        folders[f.id] = item;
    }
    for (const auto &f : p.folders) {
        auto item = folders[f.id];
        if (!f.parentId.isEmpty() && f.parentId != f.id && folders.contains(f.parentId))
            folders[f.parentId]->addChild(item);
        else
            sources->addChild(item);
        if (f.id == selection)
            m_projectTree->setCurrentItem(item);
    }
    for (const auto &tl : p.timelines)
        if (folders.contains(tl.folderId)) {
            for (int i = 0; i < timelines->childCount(); ++i)
                if (timelines->child(i)->data(0, IdRole).toString() == tl.id) {
                    folders[tl.folderId]->addChild(timelines->takeChild(i));
                    break;
                }
        }
    for (const auto &s : p.sources) {
        auto item = new QTreeWidgetItem(folders.value(s.folderId, sources), {s.name});
        item->setData(0, IdRole, s.id);
        item->setData(0, KindRole, "source");
        item->setToolTip(0, s.path);
        item->setIcon(0, uiIcon("media", s.missing ? QColor("#dd806f") : QColor("#a6bac7")));
        if (s.missing)
            item->setText(0, s.name + "  [missing]");
        if (s.id == selection)
            m_projectTree->setCurrentItem(item);
    }
    m_projectTree->expandAll();
    refreshViewerSelectors();
    refreshClips();
    refreshNotes();
    refreshDetails();
    auto offset = frameDisplayOffset(p, m_session->activeOwnerId());
    m_rangeIn->setMinimum(displayBounded(offset));
    m_rangeOut->setMinimum(displayBounded(offset));
    m_frameSpin->setRange(
        displayBounded(offset),
        displayBounded(offset + std::max<qint64>(0, m_session->durationFrames() - 1)));
    m_rangeIn->setValue(displayBounded(p.rangeIn + offset));
    m_rangeOut->setValue(
        displayBounded((p.rangeOut < 0 ? m_session->durationFrames() - 1 : p.rangeOut) + offset));
    m_frameSpin->setValue(displayBounded(m_session->currentFrame() + offset));
    m_loop->setCurrentIndex(int(p.loopMode));
    for (int i = 0; i < 3; ++i)
        m_actions["loop" + QString::number(i)]->setChecked(int(p.loopMode) == i);
    m_actions["loopHoldEmpty"]->setChecked(p.loopHoldEmpty);
    m_actions["loopDelay"]->setText(
        QString("Loop Delay… (%1 s)").arg(p.loopDelayMs / 1000.0, 0, 'g', 5));
    m_actions["ghostAnnotations"]->setChecked(p.viewer.ghostEnabled);
    m_actions["holdAnnotations"]->setChecked(p.viewer.holdAnnotations);
    m_actions["mediaInfo"]->setChecked(p.viewer.showOverlay);
    m_actions["pictureType"]->setChecked(p.viewer.showPictureType);
    m_mix->setEnabled(p.viewer.mode == ViewerMode::Blend || p.viewer.mode == ViewerMode::Wipe);
    m_mix->setValue(qRound((p.viewer.mode == ViewerMode::Blend ? p.viewer.opacity : p.viewer.wipe) * 100));
    const QStringList overlayKeys{"Off",     "Safe",   "Grid", "Center", "DynamicSymmetry",
                                  "PhiGrid", "Custom", "Image"};
    for (int i = 0; i < overlayKeys.size(); ++i)
        m_actions["overlay" + overlayKeys[i]]->setChecked(int(p.viewer.overlay.type) == i);
    if (p.viewer.overlay.type != OverlayType::Off)
        m_lastOverlayType = p.viewer.overlay.type;
    m_actions["togglePan"]->setChecked(p.viewer.panZoom.enabled);
    m_durationLabel->setText(
        QString("%1 frames  |  %2 fps  |  %3")
            .arg(m_session->durationFrames())
            .arg(p.activeTimeline() ? p.activeTimeline()->fps.value() : p.fps.value(), 0, 'g', 5)
            .arg(durationString(m_session->durationFrames(), p.fps.value())));
    m_zoomLabel->setText(p.viewer.panZoom.enabled
                             ? QString::number(p.viewer.panZoom.zoom * 100, 'f', 0) + "%"
                             : "Pan/zoom off");
    m_refreshing = false;
    refreshFrame(m_session->currentFrame());
    updateTitle();
    refreshUndoActions();
    refreshTransportState();
}
void MainWindow::refreshViewerSelectors() {
    const auto &p = m_session->project();
    for (int i = 0; i < 4; ++i) {
        QSignalBlocker blocker(m_viewerSelectors[i]);
        auto combo = m_viewerSelectors[i];
        combo->clear();
        combo->addItem("Active timeline", QString());
        for (const auto &t : p.timelines)
            combo->addItem(uiIcon("timeline"), t.name, t.id);
        for (const auto &s : p.sources)
            combo->addItem(uiIcon("media"), s.name, s.id);
        int index = combo->findData(p.viewer.assignments.value(i));
        combo->setCurrentIndex(std::max(0, index));
        combo->setEnabled(i < 2 || p.viewer.mode == ViewerMode::Grid);
    }
}
void MainWindow::refreshFrame(qint64 frame) {
    refreshUndoActions();
    QSignalBlocker block(m_frameSpin);
    auto displayFrame = displayFrameNumber(m_session->project(), m_session->activeOwnerId(), frame);
    m_frameSpin->setValue(displayBounded(displayFrame));
    if (m_frameLabel)
        m_frameLabel->setText(statusBar()->currentMessage().isEmpty()
                                  ? QString("Frame %1").arg(displayFrame)
                                  : QString());
    auto eval =
        evaluateTimeline(m_session->project(), m_session->project().activeTimelineId, frame);
    if (eval) {
        for (int row = 0; row < m_clipTable->rowCount(); ++row) {
            bool active = m_highlightCurrentClip &&
                          m_clipTable->item(row, 0)->data(IdRole).toString() == eval->clipId;
            for (int col = 0; col < 3; ++col)
                m_clipTable->item(row, col)->setBackground(active ? QColor("#3a505e")
                                                                  : QColor("#25272a"));
        }
    }
}
void MainWindow::refreshDetails() {
    const auto &p = m_session->project();
    QString id = selectedSourceId();
    if (id.isEmpty()) {
        auto ev = evaluateTimeline(p, p.activeTimelineId, m_session->currentFrame());
        if (ev)
            id = ev->sourceId;
    }
    if (auto s = p.source(id)) {
        m_selectionLabel->setText(s->name);
        auto &i = s->info;
        QString info =
            QString("%1 × %2\n%3 fps · %4 frames\n%5\nAudio: %6\n\n%7")
                .arg(i.width)
                .arg(i.height)
                .arg(i.fps.value(), 0, 'g', 6)
                .arg(i.frameCount())
                .arg(durationString(i.frameCount(), i.fps.value()))
                .arg(i.hasAudio ? QString("%1 Hz · %2 channel(s)").arg(i.sampleRate).arg(i.channels)
                                : "none")
                .arg(s->path);
        if (i.hasVideo)
            info += QString("\n\nSource color: %1 bit · %2\n%3 / %4\n%5 / %6 range")
                        .arg(i.color.componentBits > 0 ? QString::number(i.color.componentBits)
                                                       : "unknown")
                        .arg(i.color.pixelFormat.isEmpty() ? "unknown format" : i.color.pixelFormat)
                        .arg(i.color.primaries, i.color.transfer, i.color.matrix, i.color.range);
        if (s->missing)
            info = "MISSING MEDIA — right-click to relink\n\n" + info;
        m_mediaInfo->setText(info);
    } else if (auto tl = p.timeline(selectedTimelineId().isEmpty() ? p.activeTimelineId
                                                                   : selectedTimelineId())) {
        m_selectionLabel->setText(tl->name);
        m_mediaInfo->setText(QString("Timeline\n%1 clips\n%2 frames\n%3 fps")
                                 .arg(tl->clips.size())
                                 .arg(p.durationFrames(tl->id))
                                 .arg(tl->fps.value(), 0, 'g', 6));
    } else {
        m_selectionLabel->setText("No source selected");
        m_mediaInfo->clear();
    }
}
void MainWindow::refreshClips() {
    int selected = m_clipTable->currentRow();
    m_clipTable->setRowCount(0);
    const auto &p = m_session->project();
    if (auto tl = p.activeTimeline())
        for (const auto &c : tl->clips) {
            int r = m_clipTable->rowCount();
            m_clipTable->insertRow(r);
            auto source = p.source(c.sourceId);
            auto name = new QTableWidgetItem(
                c.name.isEmpty() ? (source ? source->name : "Missing source") : c.name);
            name->setData(IdRole, c.id);
            m_clipTable->setItem(r, 0, name);
            m_clipTable->setItem(r, 1, new QTableWidgetItem(QString::number(c.inFrame)));
            m_clipTable->setItem(
                r, 2,
                new QTableWidgetItem(QString::number(
                    c.outFrame < 0 ? (source ? source->info.frameCount() - 1 : 0) : c.outFrame)));
        }
    if (selected >= 0 && selected < m_clipTable->rowCount())
        m_clipTable->selectRow(selected);
}
void MainWindow::refreshNotes() {
    m_notesTable->setRowCount(0);
    const auto &p = m_session->project();
    struct Entry {
        qint64 frame;
        QString id, text, author, kind;
        QColor color;
        QColor label;
    };
    QVector<Entry> entries;
    for (const auto &n : p.notes)
        if (n.timelineId == m_session->activeOwnerId() &&
            (m_bookmarkFilter == BookmarkFilter::All || m_bookmarkFilter == BookmarkFilter::Notes))
            entries.append({n.frame, n.id, n.text, n.author, "note", QColor("#b9c1c9"), n.color});
    for (const auto &b : p.bookmarks)
        if (b.timelineId == m_session->activeOwnerId() && bookmarkMatches(p, b, m_bookmarkFilter))
            entries.append({b.frame, b.id, b.text.isEmpty() ? b.name : b.text,
                            b.endFrame > b.frame ? QString("to %1").arg(displayFrameNumber(
                                                       p, b.timelineId, b.endFrame))
                                                 : "Bookmark",
                            "bookmark", b.color, Qt::transparent});
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto &a, const auto &b) { return a.frame < b.frame; });
    for (const auto &e : entries) {
        int r = m_notesTable->rowCount();
        m_notesTable->insertRow(r);
        auto f = new QTableWidgetItem(
            QString::number(displayFrameNumber(p, m_session->activeOwnerId(), e.frame)));
        f->setData(Qt::UserRole, e.frame);
        f->setData(IdRole, e.id);
        f->setData(KindRole, e.kind);
        f->setForeground(e.color);
        if (e.label.isValid() && e.label.alpha() > 0) {
            f->setBackground(e.label);
            f->setForeground(e.label.lightness() < 140 ? Qt::white : Qt::black);
        }
        f->setIcon(uiIcon(e.kind, e.color));
        m_notesTable->setItem(r, 0, f);
        m_notesTable->setItem(r, 1, new QTableWidgetItem(e.text));
        m_notesTable->setItem(r, 2, new QTableWidgetItem(e.author));
    }
    m_notesTable->resizeRowsToContents();
}
QString MainWindow::selectedSourceId() const {
    auto i = m_projectTree->currentItem();
    return i && i->data(0, KindRole) == "source" ? i->data(0, IdRole).toString() : QString();
}
QString MainWindow::selectedTimelineId() const {
    auto i = m_projectTree->currentItem();
    return i && i->data(0, KindRole) == "timeline" ? i->data(0, IdRole).toString() : QString();
}
void MainWindow::refreshTransportState() {
    if (!m_speed || !m_volume || !m_durationLabel)
        return;
    QSignalBlocker speedBlock(m_speed), volumeBlock(m_volume);
    m_speed->setCurrentText(QString::number(m_session->playbackRate(), 'g', 5) + "×");
    m_volume->setValue(int(std::round(m_session->volume() * 100)));
    m_actions["mute"]->setChecked(!m_session->audioEnabled());
    m_actions["audioScrub"]->setChecked(m_session->audioScrubEnabled());
    m_actions["audioTimeStretching"]->setChecked(m_session->audioTimeStretchingEnabled());
    const auto &p = m_session->project();
    auto owner = m_session->activeOwnerId();
    double fps = p.fps.value();
    if (auto source = p.source(owner))
        fps = source->info.fps.value();
    else if (auto timeline = p.timeline(owner))
        fps = timeline->fps.value();
    m_durationLabel->setText(
        QString("%1 frames  |  %2  |  %3")
            .arg(m_session->durationFrames())
            .arg(m_session->bookmarkPlayback() ? "Stepped" : QString::number(fps, 'g', 6) + " fps")
            .arg(durationString(m_session->durationFrames(), fps)));
}
void MainWindow::importMediaPaths(const QStringList &paths) {
    if (paths.isEmpty())
        return;
    if (QSettings().value("general/newPlayers", false).toBool()) {
        for (const auto &path : paths) {
            QString error;
            auto safe = LocalPathPolicy::validateInput(path, &error);
            if (safe.isEmpty()) {
                showError(error);
                continue;
            }
            QProcess::startDetached(QCoreApplication::applicationFilePath(), {safe});
        }
    } else
        m_session->importMedia(paths);
}
void MainWindow::importFiles() {
    auto files = LocalFileDialog::getOpenFileNames(
        this, "Import media", QSettings().value("project/sourceRoot").toString(),
        "Media (*.mov *.mp4 *.mkv *.avi *.webm *.m4v *.mpg *.mpeg *.wmv *.wav *.mp3 *.aac *.flac "
        "*.ogg *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.exr);;All files (*)");
    if (!files.isEmpty()) {
        importMediaPaths(files);
        for (const auto &file : files)
            updateRecent("recent/files", file);
    }
}
void MainWindow::importFolder() {
    auto path = LocalFileDialog::getExistingDirectory(
        this, "Import media folder", QSettings().value("project/sourceRoot").toString());
    if (path.isEmpty())
        return;
    auto paths = LocalFileDialog::localFiles(path, {"*.mov", "*.mp4", "*.mkv", "*.avi", "*.webm",
                                                    "*.wav", "*.mp3", "*.flac", "*.png", "*.jpg",
                                                    "*.jpeg", "*.tif", "*.tiff"});
    if (paths.isEmpty()) {
        statusBar()->showMessage("No supported media found in folder", 5000);
        return;
    }
    importMediaPaths(paths);
}
void MainWindow::importSequence() {
    auto paths = LocalFileDialog::getOpenFileNames(
        this, "Select image sequence frames", QSettings().value("project/sourceRoot").toString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.exr)");
    if (paths.isEmpty())
        return;
    std::sort(paths.begin(), paths.end());
    QSettings prefs;
    QDialog dialog(this);
    dialog.setWindowTitle("Image Sequence Frame Rate");
    auto form = new QFormLayout(&dialog);
    auto numerator = new QSpinBox(&dialog), denominator = new QSpinBox(&dialog);
    numerator->setRange(1, 1000000);
    denominator->setRange(1, 1000000);
    numerator->setValue(prefs.value("project/sequenceFpsNum", 24).toInt());
    denominator->setValue(prefs.value("project/sequenceFpsDen", 1).toInt());
    form->addRow("FPS numerator", numerator);
    form->addRow("FPS denominator", denominator);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        m_session->importImageSequence(paths, {numerator->value(), denominator->value()});
}
void MainWindow::openProject() {
    if (!confirmClose())
        return;
    auto path = LocalFileDialog::getOpenFileName(this, "Open project", {},
                                                 "OpenFrame project (*.oframe *.json)");
    if (!path.isEmpty() && m_session->openProject(path))
        updateRecent("recent/projects", path);
}
void MainWindow::saveAs() {
    auto path = LocalFileDialog::getSaveFileName(this, "Save project",
                                                 m_session->projectPath().isEmpty()
                                                     ? m_session->project().name + ".oframe"
                                                     : m_session->projectPath(),
                                                 "OpenFrame project (*.oframe);;JSON (*.json)");
    if (!path.isEmpty() && m_session->saveProject(path)) {
        updateRecent("recent/projects", path);
        updateTitle();
    }
}
bool MainWindow::confirmClose() {
    if (m_smokeMode)
        return true;
    auto mode = QSettings().value("general/confirmClose", "always").toString();
    if (mode == "disabled" ||
        (mode == "saved" && (m_session->projectPath().isEmpty() || !m_session->dirty())))
        return true;
    if (!m_session->dirty())
        return QMessageBox::question(this, "Close Project", "Close the current project?",
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::Yes) == QMessageBox::Yes;
    auto result = QMessageBox::warning(
        this, "Unsaved project", "Save changes to the current project?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (result == QMessageBox::Cancel)
        return false;
    if (result == QMessageBox::Discard)
        return true;
    if (m_session->projectPath().isEmpty())
        saveAs();
    else
        m_session->saveProject();
    return !m_session->dirty();
}
void MainWindow::addFolder() {
    bool ok = false;
    auto name =
        QInputDialog::getText(this, "New folder", "Name:", QLineEdit::Normal, "Folder", &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QString parent;
    if (auto item = m_projectTree->currentItem(); item && item->data(0, KindRole) == "folder")
        parent = item->data(0, IdRole).toString();
    m_session->mutateProject(
        "New folder", [=](Project &p) { p.folders.append({newId(), name.trimmed(), parent}); });
}
void MainWindow::addTimeline() {
    bool ok = false;
    auto name = QInputDialog::getText(
        this, "New timeline", "Name:", QLineEdit::Normal,
        QString("Timeline %1").arg(m_session->project().timelines.size() + 1), &ok);
    if (ok && !name.trimmed().isEmpty()) {
        auto id = m_session->addTimeline(name.trimmed());
        m_session->setActiveTimeline(id);
    }
}
void MainWindow::appendSelected() {
    QStringList ids;
    for (auto item : m_projectTree->selectedItems())
        if (item->data(0, KindRole) == "source")
            ids << item->data(0, IdRole).toString();
    for (auto id : ids)
        m_session->appendSource(id);
}
void MainWindow::deleteSelected() {
    auto items = m_projectTree->selectedItems();
    QStringList sources, folders, timelines;
    for (auto i : items) {
        auto kind = i->data(0, KindRole).toString(), id = i->data(0, IdRole).toString();
        if (kind == "source")
            sources << id;
        if (kind == "folder")
            folders << id;
        if (kind == "timeline")
            timelines << id;
    }
    if (!sources.isEmpty() || !timelines.isEmpty()) {
        if (QMessageBox::question(this, "Remove from project",
                                  "Remove the selected items and their timeline references? Media "
                                  "files remain on disk.") != QMessageBox::Yes)
            return;
    }
    for (auto id : sources)
        m_session->removeSource(id);
    if (!folders.isEmpty() || !timelines.isEmpty())
        m_session->mutateProject("Remove project items", [=](Project &p) {
            for (auto &s : p.sources)
                if (folders.contains(s.folderId))
                    s.folderId.clear();
            for (auto &t : p.timelines)
                if (folders.contains(t.folderId))
                    t.folderId.clear();
            for (auto &f : p.folders)
                if (folders.contains(f.parentId))
                    f.parentId.clear();
            p.folders.removeIf([=](const MediaFolder &f) { return folders.contains(f.id); });
            p.timelines.removeIf([=](const Timeline &t) { return timelines.contains(t.id); });
            if (!p.timeline(p.activeTimelineId))
                p.activeTimelineId = p.timelines.isEmpty() ? QString() : p.timelines.first().id;
        });
}
void MainWindow::renameSelected() {
    auto item = m_projectTree->currentItem();
    if (!item)
        return;
    auto id = item->data(0, IdRole).toString();
    if (id.isEmpty())
        return;
    bool ok = false;
    auto name =
        QInputDialog::getText(this, "Rename", "Name:", QLineEdit::Normal, item->text(0), &ok);
    if (ok && !name.trimmed().isEmpty())
        m_session->mutateProject("Rename", [=](Project &p) {
            for (auto &s : p.sources)
                if (s.id == id)
                    s.name = name;
            for (auto &t : p.timelines)
                if (t.id == id)
                    t.name = name;
            for (auto &f : p.folders)
                if (f.id == id)
                    f.name = name;
        });
}
void MainWindow::relinkSelected() {
    auto id = selectedSourceId();
    if (id.isEmpty())
        return;
    auto path = LocalFileDialog::getOpenFileName(this, "Relink source");
    if (!path.isEmpty())
        m_session->relinkSource(id, path);
}
void MainWindow::assignSelected(int pane) {
    auto id = selectedSourceId();
    if (id.isEmpty())
        id = selectedTimelineId();
    if (!id.isEmpty())
        m_session->assignViewer(pane, id);
}
void MainWindow::projectContextMenu(QPoint pos) {
    QMenu menu(this);
    auto item = m_projectTree->itemAt(pos);
    if (item)
        m_projectTree->setCurrentItem(item);
    if (!selectedSourceId().isEmpty()) {
        menu.addAction("Append to Active Timeline", this, &MainWindow::appendSelected);
        menu.addAction("Relink Source…", this, &MainWindow::relinkSelected);
    }
    if (!selectedSourceId().isEmpty() || !selectedTimelineId().isEmpty()) {
        auto viewer = menu.addMenu("Assign to Viewer");
        for (int i = 0; i < 4; ++i)
            viewer->addAction(QString(QChar('A' + i)), this, [this, i] { assignSelected(i); });
        menu.addAction("Properties…", this, &MainWindow::editSelectedProperties);
        if (!selectedTimelineId().isEmpty()) {
            auto defaultTimeline = menu.addAction("Default timeline for imports");
            defaultTimeline->setCheckable(true);
            defaultTimeline->setChecked(selectedTimelineId() ==
                                        m_session->project().defaultTimelineId);
            connect(defaultTimeline, &QAction::triggered, this, [this](bool on) {
                auto id = selectedTimelineId();
                m_session->mutateProject("Default import timeline", [=](Project &p) {
                    p.defaultTimelineId = on ? id : QString();
                });
            });
        }
        menu.addAction("Rename…", this, &MainWindow::renameSelected);
        menu.addAction("Remove", this, &MainWindow::deleteSelected);
        menu.addSeparator();
    }
    menu.addAction("New Folder…", this, &MainWindow::addFolder);
    menu.addAction("New Timeline…", this, &MainWindow::addTimeline);
    menu.addAction("Import Media…", this, &MainWindow::importFiles);
    menu.exec(m_projectTree->viewport()->mapToGlobal(pos));
}
void MainWindow::moveClip(int direction) {
    int from = m_clipTable->currentRow();
    auto tl = m_session->project().activeTimeline();
    if (!tl || from < 0 || from + direction < 0 || from + direction >= tl->clips.size())
        return;
    QString id = tl->id;
    m_session->mutateProject("Reorder clip", [=](Project &p) {
        for (auto &t : p.timelines)
            if (t.id == id)
                t.clips.move(from, from + direction);
    });
    m_clipTable->selectRow(from + direction);
}
void MainWindow::removeClip() {
    int index = m_clipTable->currentRow();
    auto tl = m_session->project().activeTimeline();
    if (!tl || index < 0 || index >= tl->clips.size())
        return;
    QString id = tl->id;
    m_session->mutateProject("Remove clip", [=](Project &p) {
        for (auto &t : p.timelines)
            if (t.id == id)
                t.clips.removeAt(index);
    });
}
void MainWindow::editClip() {
    int index = m_clipTable->currentRow();
    auto tl = m_session->project().activeTimeline();
    if (!tl || index < 0 || index >= tl->clips.size())
        return;
    auto clip = tl->clips[index];
    auto source = m_session->project().source(clip.sourceId);
    if (!source)
        return;
    QString timelineId = tl->id;
    QDialog dialog(this);
    dialog.setWindowTitle("Clip Properties");
    auto form = new QFormLayout(&dialog);
    auto name = new QLineEdit(clip.name, &dialog);
    auto in = new QSpinBox(&dialog), out = new QSpinBox(&dialog);
    in->setRange(0, bounded(source->info.frameCount() - 1));
    out->setRange(0, bounded(source->info.frameCount() - 1));
    in->setValue(bounded(clip.inFrame));
    out->setValue(bounded(clip.outFrame < 0 ? source->info.frameCount() - 1 : clip.outFrame));
    auto gain = new QDoubleSpinBox(&dialog);
    gain->setRange(0, 4);
    gain->setSingleStep(.1);
    gain->setValue(clip.gain);
    auto offset = new QDoubleSpinBox(&dialog);
    offset->setRange(-86400, 86400);
    offset->setDecimals(3);
    offset->setSuffix(" s");
    offset->setValue(clip.audioOffsetUs / 1000000.0);
    auto muted = new QCheckBox("Mute clip audio", &dialog);
    muted->setChecked(clip.muted);
    form->addRow("Name", name);
    form->addRow("Source in", in);
    form->addRow("Source out (inclusive)", out);
    form->addRow("Audio gain", gain);
    form->addRow("Audio offset", offset);
    form->addRow(muted);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (out->value() < in->value()) {
        showError("Clip out must be at or after clip in.");
        return;
    }
    clip.name = name->text();
    clip.inFrame = in->value();
    clip.outFrame = out->value();
    clip.gain = gain->value();
    clip.muted = muted->isChecked();
    clip.audioOffsetUs = qint64(offset->value() * 1000000);
    m_session->mutateProject("Edit clip", [=](Project &p) {
        for (auto &t : p.timelines)
            if (t.id == timelineId)
                t.clips[index] = clip;
    });
}
void MainWindow::replaceClipAudio() {
    int index = m_clipTable->currentRow();
    auto tl = m_session->project().activeTimeline();
    if (!tl || index < 0 || index >= tl->clips.size())
        return;
    QStringList labels{"Original source audio"}, ids{QString()};
    for (const auto &s : m_session->project().sources)
        if (s.info.hasAudio) {
            labels << s.name;
            ids << s.id;
        }
    bool ok = false;
    QString label =
        QInputDialog::getItem(this, "Replace clip audio", "Audio source:", labels, 0, false, &ok);
    if (!ok)
        return;
    QString audio = ids.value(labels.indexOf(label)), timelineId = tl->id;
    m_session->mutateProject("Replace clip audio", [=](Project &p) {
        for (auto &t : p.timelines)
            if (t.id == timelineId)
                t.clips[index].audioSourceId = audio;
    });
}
void MainWindow::clipContextMenu(QPoint pos) {
    if (auto item = m_clipTable->itemAt(pos))
        m_clipTable->selectRow(item->row());
    QMenu menu(this);
    menu.addAction("Clip Properties…", this, &MainWindow::editClip);
    menu.addAction("Replace Audio…", this, &MainWindow::replaceClipAudio);
    menu.addAction("Move Earlier", this, [this] { moveClip(-1); });
    menu.addAction("Move Later", this, [this] { moveClip(1); });
    menu.addAction("Remove Clip", this, &MainWindow::removeClip);
    menu.exec(m_clipTable->viewport()->mapToGlobal(pos));
}
void MainWindow::editSelectedProperties() {
    QString id = selectedSourceId();
    if (id.isEmpty())
        id = selectedTimelineId();
    if (id.isEmpty())
        id = m_session->activeOwnerId();
    const auto project = m_session->project();
    auto source = project.source(id);
    auto timeline = project.timeline(id);
    if (!source && !timeline)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(source ? "Source Properties" : "Timeline Properties");
    dialog.resize(570, source ? 640 : 440);
    auto layout = new QVBoxLayout(&dialog);
    auto scroll = new QScrollArea(&dialog);
    scroll->setObjectName("ownerPropertiesScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto page = new QWidget(scroll);
    auto form = new QFormLayout(page);
    scroll->setWidget(page);
    layout->addWidget(scroll, 1);
    auto name = new QLineEdit(source ? source->name : timeline->name, &dialog);
    form->addRow("Name", name);
    auto fpsNum = new QSpinBox(&dialog), fpsDen = new QSpinBox(&dialog);
    fpsNum->setRange(1, 1000000);
    fpsDen->setRange(1, 1000000);
    auto fps = source ? source->info.fps : timeline->fps;
    fpsNum->setValue(int(fps.num));
    fpsDen->setValue(int(fps.den));
    bool editableFps =
        timeline || source->kind == SourceKind::Sequence || source->kind == SourceKind::Image;
    fpsNum->setEnabled(editableFps);
    fpsDen->setEnabled(editableFps);
    form->addRow("Frame rate", row(&dialog, {fpsNum, new QLabel("/", &dialog), fpsDen}));
    auto in = new QSpinBox(&dialog), out = new QSpinBox(&dialog);
    if (source) {
        in->setRange(0, bounded(source->info.frameCount() - 1));
        out->setRange(0, bounded(source->info.frameCount() - 1));
        in->setValue(bounded(source->inFrame));
        out->setValue(
            bounded(source->outFrame < 0 ? source->info.frameCount() - 1 : source->outFrame));
        form->addRow("Default source in (0-based)", in);
        form->addRow("Default source out (inclusive)", out);
    } else {
        in->hide();
        out->hide();
    }
    auto audioTrack = new QComboBox(&dialog);
    audioTrack->setObjectName("sourceAudioTrack");
    audioTrack->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    audioTrack->setMinimumContentsLength(20);
    if (source) {
        audioTrack->addItem(
            source->info.audioTracks.isEmpty() ? "No audio tracks" : "Default track", -1);
        for (const auto &track : source->info.audioTracks)
            audioTrack->addItem(
                QString("%1 · %2 · %3 Hz · %4 ch%5%6")
                    .arg(track.streamIndex)
                    .arg(track.codec)
                    .arg(track.sampleRate)
                    .arg(track.channels)
                    .arg(track.language.isEmpty() ? QString() : " · " + track.language)
                    .arg(track.title.isEmpty() ? QString() : " · " + track.title),
                track.streamIndex);
        audioTrack->setCurrentIndex(std::max(0, audioTrack->findData(source->selectedAudioTrack)));
        audioTrack->setEnabled(!source->info.audioTracks.isEmpty());
        audioTrack->setToolTip(
            "Applies to playback, scrubbing and export for every use of this source.");
        form->addRow("Audio track", audioTrack);
    } else
        audioTrack->hide();
    auto audioOffset = new QDoubleSpinBox(&dialog);
    audioOffset->setObjectName("sourceAudioOffsetMs");
    audioOffset->setRange(-double(MaximumAudioOffsetUs) / 1000,
                          double(MaximumAudioOffsetUs) / 1000);
    audioOffset->setDecimals(3);
    audioOffset->setSingleStep(1);
    audioOffset->setSuffix(" ms");
    if (source) {
        audioOffset->setValue(source->audioOffsetUs / 1000.0);
        audioOffset->setEnabled(source->info.hasAudio);
        audioOffset->setToolTip(
            "Positive advances audio; negative delays it. Applies wherever this source is used.");
        form->addRow("Source audio offset", audioOffset);
    } else
        audioOffset->hide();
    const double originalAudioOffsetValue = audioOffset->value();
    auto panZoom = id == m_session->activeOwnerId() ? project.viewer.panZoom
                                                    : project.panZoomByOwner.value(id);
    auto panEnabled = new QCheckBox("Enable pan / zoom when this is Viewer A", &dialog);
    panEnabled->setObjectName("ownerPanZoomEnabled");
    panEnabled->setChecked(panZoom.enabled);
    auto zoom = new QDoubleSpinBox(&dialog), panX = new QDoubleSpinBox(&dialog),
         panY = new QDoubleSpinBox(&dialog);
    zoom->setObjectName("ownerZoom");
    panX->setObjectName("ownerPanX");
    panY->setObjectName("ownerPanY");
    zoom->setRange(.01, 64);
    zoom->setDecimals(3);
    zoom->setValue(panZoom.zoom);
    for (auto spin : {panX, panY}) {
        spin->setRange(-100, 100);
        spin->setDecimals(4);
        spin->setSingleStep(.01);
    }
    panX->setValue(panZoom.pan.x());
    panY->setValue(panZoom.pan.y());
    form->addRow(panEnabled);
    form->addRow("Whole-viewer zoom", zoom);
    form->addRow("Whole-viewer pan X / Y", row(&dialog, {panX, panY}));
    auto offsetEnabled = new QCheckBox("Use custom display frame numbering", &dialog);
    offsetEnabled->setChecked(source ? source->timecodeOffsetEnabled
                                     : timeline->timecodeOffsetEnabled);
    auto offset = new QSpinBox(&dialog);
    offset->setRange(-1000000000, 1000000000);
    offset->setValue(offsetEnabled->isChecked()
                         ? int(source ? source->timecodeOffset : timeline->timecodeOffset)
                         : 1);
    offset->setEnabled(offsetEnabled->isChecked());
    form->addRow(offsetEnabled);
    form->addRow("Displayed number for first frame", offset);
    connect(offsetEnabled, &QCheckBox::toggled, offset, &QWidget::setEnabled);
    auto note =
        new QLabel("Changes displayed frame labels; source timing stays the same.", &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto propertyValues = [&] {
        return QVariantList{name->text(),    fpsNum->value(),
                            fpsDen->value(), in->value(),
                            out->value(),    offsetEnabled->isChecked(),
                            offset->value(), panEnabled->isChecked(),
                            zoom->value(),   panX->value(),
                            panY->value()};
    };
    const auto originalPropertyValues = propertyValues();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto currentSource = m_session->project().source(id);
    if ((source && (!currentSource || currentSource->path != source->path ||
                    currentSource->info.frameCount() != source->info.frameCount())) ||
        (timeline && !m_session->project().timeline(id))) {
        showError("This source or timeline changed while its properties were open. Reopen "
                  "Properties to edit the current item.");
        return;
    }
    if (source && out->value() < in->value()) {
        showError("Source out must be at or after source in.");
        return;
    }
    QString title = name->text();
    Rational rate{fpsNum->value(), fpsDen->value()};
    bool custom = offsetEnabled->isChecked();
    qint64 base = offset->value(), first = in->value(), last = out->value();
    const int selectedTrack = audioTrack->currentData().toInt();
    const bool changeTrack = source && selectedTrack != source->selectedAudioTrack;
    const qint64 selectedOffset = std::clamp<qint64>(qRound64(audioOffset->value() * 1000),
                                                     -MaximumAudioOffsetUs, MaximumAudioOffsetUs);
    const bool changeOffset = source && audioOffset->value() != originalAudioOffsetValue &&
                              selectedOffset != source->audioOffsetUs;
    const bool changeProperties = propertyValues() != originalPropertyValues;
    if (!changeProperties && !changeTrack && !changeOffset)
        return;
    const QString primaryOwner = m_session->activeOwnerId();
    panZoom.enabled = panEnabled->isChecked();
    panZoom.zoom = zoom->value();
    panZoom.pan = {panX->value(), panY->value()};
    const bool groupUndo = m_session->projectUndoEnabled() && (changeTrack || changeOffset);
    if (groupUndo)
        m_session->undoStack()->beginMacro("Source / timeline properties");
    if (changeProperties)
        m_session->mutateProject("Source / timeline properties", [=](Project &p) {
            p.panZoomByOwner[id] = panZoom;
            if (primaryOwner == id)
                p.viewer.panZoom = panZoom;
            for (auto &s : p.sources)
                if (s.id == id) {
                    s.name = title;
                    s.inFrame = first;
                    s.outFrame = last;
                    s.timecodeOffsetEnabled = custom;
                    s.timecodeOffset = base;
                    if (editableFps) {
                        s.info.fps = rate;
                        s.info.timeBase = {rate.den, rate.num};
                        s.info.durationUs = rate.frameToUs(s.info.frameCount());
                    }
                }
            for (auto &t : p.timelines)
                if (t.id == id) {
                    t.name = title;
                    t.fps = rate;
                    t.timecodeOffsetEnabled = custom;
                    t.timecodeOffset = base;
                }
        });
    if (changeTrack)
        m_session->setSourceAudioTrack(id, selectedTrack);
    if (changeOffset)
        m_session->setSourceAudioOffset(id, selectedOffset);
    if (groupUndo)
        m_session->undoStack()->endMacro();
    refreshUndoActions();
}
void MainWindow::setOverlayType(OverlayType type) {
    auto settings = m_session->project().viewer.overlay;
    settings.type = type;
    if (type == OverlayType::Image && settings.imagePath.isEmpty()) {
        OverlayDialog dialog(settings, this);
        if (dialog.exec() != QDialog::Accepted) {
            refreshProject();
            return;
        }
        settings = dialog.settings();
    }
    changeViewer([&](ViewerSettings &viewer) {
        viewer.overlay = settings;
        viewer.overlayGrid = viewer.overlaySafe = viewer.overlayCenter = false;
    });
}
void MainWindow::overlayDialog() {
    OverlayDialog dialog(m_session->project().viewer.overlay, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    changeViewer([&](ViewerSettings &viewer) {
        viewer.overlay = dialog.settings();
        viewer.overlayGrid = viewer.overlaySafe = viewer.overlayCenter = false;
    });
}
void MainWindow::loopDelayDialog() {
    QDialog dialog(this);
    dialog.setObjectName("loopDelayDialog");
    dialog.setWindowTitle("Loop Delay");
    auto form = new QFormLayout(&dialog);
    auto seconds = new QDoubleSpinBox(&dialog);
    seconds->setObjectName("loopDelaySeconds");
    seconds->setRange(0, 600);
    seconds->setDecimals(3);
    seconds->setSingleStep(.1);
    seconds->setSuffix(" s");
    seconds->setValue(m_session->project().loopDelayMs / 1000.0);
    form->addRow("Wait before repeating", seconds);
    auto empty = new QCheckBox("Hold an empty frame during the delay", &dialog);
    empty->setObjectName("loopDelayEmpty");
    empty->setChecked(m_session->project().loopHoldEmpty);
    form->addRow(empty);
    auto note = new QLabel("The transport waits at each looping boundary. Without empty hold, the "
                           "boundary image remains visible. A zero delay repeats immediately.",
                           &dialog);
    note->setWordWrap(true);
    note->setMaximumWidth(390);
    form->addRow(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        m_session->setLoopDelay(qRound(seconds->value() * 1000), empty->isChecked());
}
void MainWindow::changeViewer(std::function<void(ViewerSettings &)> change) {
    auto v = m_session->project().viewer;
    change(v);
    m_session->setViewerSettings(v);
}
void MainWindow::toggleBookmark(bool range) {
    auto p = m_session->project();
    qint64 first = range ? p.rangeIn : m_session->currentFrame(),
           last = range ? (p.rangeOut < 0 ? m_session->durationFrames() - 1 : p.rangeOut) : first;
    QString existing;
    for (auto &b : p.bookmarks)
        if (b.timelineId == m_session->activeOwnerId() && b.frame == first && b.endFrame == last) {
            existing = b.id;
            break;
        }
    if (existing.isEmpty())
        m_session->addBookmark(range ? "Range bookmark" : "Bookmark", first, last);
    else
        m_session->mutateProject("Remove bookmark", [existing](Project &p) {
            p.bookmarks.removeIf([&](const Bookmark &b) { return b.id == existing; });
        });
}
void MainWindow::jumpBookmark(int direction, bool ranges) {
    const auto &p = m_session->project();
    QVector<Bookmark> marks;
    for (auto &b : p.bookmarks)
        if (b.timelineId == m_session->activeOwnerId() && (!ranges || b.endFrame > b.frame) &&
            bookmarkMatches(p, b, m_bookmarkFilter))
            marks << b;
    if (marks.isEmpty())
        return;
    std::sort(marks.begin(), marks.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
    qint64 frame = m_session->currentFrame();
    const Bookmark *chosen = nullptr;
    if (direction > 0) {
        for (auto &b : marks)
            if (b.frame > frame) {
                chosen = &b;
                break;
            }
        if (!chosen && p.loopMode != LoopMode::Once)
            chosen = &marks.first();
    } else {
        for (auto i = marks.rbegin(); i != marks.rend(); ++i)
            if (i->frame < frame) {
                chosen = &*i;
                break;
            }
        if (!chosen && p.loopMode != LoopMode::Once)
            chosen = &marks.last();
    }
    if (chosen) {
        if (ranges)
            m_session->setRange(chosen->frame, chosen->endFrame);
        m_session->seek(chosen->frame);
    }
}
void MainWindow::setBookmarkFilter(BookmarkFilter filter) {
    m_bookmarkFilter = filter;
    QSettings().setValue("bookmarks/filter", int(filter));
    const QSignalBlocker block(m_bookmarkFilterCombo);
    m_bookmarkFilterCombo->setCurrentIndex(int(filter));
    for (int i = 0; i < 4; ++i)
        if (m_actions.contains("bookmarkFilter" + QString::number(i)))
            m_actions["bookmarkFilter" + QString::number(i)]->setChecked(i == int(filter));
    m_timeline->setBookmarkFilter(filter);
    m_rangeTimeline->setBookmarkFilter(filter);
    refreshNotes();
}
void MainWindow::removeEmptyBookmarks() {
    QStringList ids;
    const auto &project = m_session->project();
    for (const auto &bookmark : project.bookmarks)
        if (bookmark.timelineId == m_session->activeOwnerId() &&
            bookmarkMatches(project, bookmark, BookmarkFilter::Empty))
            ids.append(bookmark.id);
    if (ids.isEmpty()) {
        statusBar()->showMessage("No empty bookmarks in the current source or timeline", 4000);
        return;
    }
    m_session->mutateProject("Remove empty bookmarks", [ids](Project &p) {
        p.bookmarks.removeIf([&](const Bookmark &b) { return ids.contains(b.id); });
    });
    statusBar()->showMessage(QString("Removed %1 empty bookmark(s)").arg(ids.size()), 4000);
}
void MainWindow::jumpEdit(int direction) {
    const auto &p = m_session->project();
    auto tl = p.activeTimeline();
    if (!tl)
        return;
    QVector<qint64> edits{0};
    qint64 offset = 0;
    for (auto &c : tl->clips) {
        offset += clipDurationFrames(p, *tl, c);
        edits << offset;
    }
    auto frame = m_session->currentFrame();
    if (direction > 0) {
        for (auto f : edits)
            if (f > frame) {
                m_session->seek(f);
                return;
            }
    } else
        for (auto i = edits.rbegin(); i != edits.rend(); ++i)
            if (*i < frame) {
                m_session->seek(*i);
                return;
            }
}
void MainWindow::copyBookmark(bool cut) {
    auto timeline = m_session->activeOwnerId();
    auto frame = m_session->currentFrame();
    m_annotationClipboard.clear();
    for (auto &a : m_session->project().annotations)
        if (a.timelineId == timeline && a.frame == frame && !a.background)
            m_annotationClipboard << a;
    if (cut)
        m_session->mutateProject("Cut bookmark", [=](Project &p) {
            p.annotations.removeIf([=](const Annotation &a) {
                return a.timelineId == timeline && a.frame == frame && !a.background;
            });
            p.bookmarks.removeIf(
                [=](const Bookmark &b) { return b.timelineId == timeline && b.frame == frame; });
        });
}
void MainWindow::pasteBookmark() {
    if (m_annotationClipboard.isEmpty())
        return;
    auto copies = m_annotationClipboard;
    auto tl = m_session->activeOwnerId();
    auto f = m_session->currentFrame();
    m_session->mutateProject("Paste bookmark", [=](Project &p) mutable {
        p.annotations.removeIf([=](const Annotation &a) {
            return a.timelineId == tl && a.frame == f && !a.background;
        });
        for (auto &a : copies) {
            a.id = newId();
            a.timelineId = tl;
            a.endFrame = f + std::max<qint64>(0, a.endFrame - a.frame);
            a.frame = f;
            p.annotations << a;
        }
        bool found = false;
        for (auto &b : p.bookmarks)
            if (b.timelineId == tl && b.frame == f)
                found = true;
        if (!found)
            p.bookmarks.append({newId(), tl, f, f, "Bookmark", {}, QColor(255, 188, 64)});
    });
}
void MainWindow::editNote(const QString &noteId) {
    Note existing;
    bool found = false;
    for (auto &n : m_session->project().notes)
        if ((!noteId.isEmpty() && n.id == noteId) ||
            (noteId.isEmpty() && n.timelineId == m_session->activeOwnerId() &&
             n.frame == m_session->currentFrame())) {
            existing = n;
            found = true;
            break;
        }
    QDialog dialog(this);
    dialog.setWindowTitle(
        QString("Note — frame %1")
            .arg(displayFrameNumber(m_session->project(), m_session->activeOwnerId(),
                                    found ? existing.frame : m_session->currentFrame())));
    dialog.resize(470, 320);
    auto l = new QVBoxLayout(&dialog);
    auto text = new QTextEdit(&dialog);
    text->setAcceptRichText(false);
    text->setPlainText(existing.text);
    text->setPlaceholderText("Add review notes for this frame…");
    l->addWidget(text);
    auto author = new QLineEdit(existing.author, &dialog);
    author->setPlaceholderText("Author (optional)");
    l->addWidget(author);
    QColor noteColor = existing.color;
    auto labelButton =
        new QPushButton(noteColor.alpha() ? "Label: " + noteColor.name() : "Label…", &dialog);
    auto clearLabel = new QPushButton("No label", &dialog);
    l->addWidget(row(&dialog, {labelButton, clearLabel}));
    connect(labelButton, &QPushButton::clicked, &dialog, [&] {
        auto color =
            QColorDialog::getColor(noteColor.alpha() ? noteColor : QColor(220, 170, 65), &dialog,
                                   "Note label color", QColorDialog::ShowAlphaChannel);
        if (color.isValid()) {
            noteColor = color;
            labelButton->setText("Label: " + color.name());
        }
    });
    connect(clearLabel, &QPushButton::clicked, &dialog, [&] {
        noteColor = Qt::transparent;
        labelButton->setText("Label…");
    });
    auto buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Discard, &dialog);
    auto del = buttons->addButton("Delete", QDialogButtonBox::DestructiveRole);
    del->setEnabled(found);
    l->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, &dialog,
            &QDialog::accept);
    connect(buttons->button(QDialogButtonBox::Discard), &QPushButton::clicked, &dialog,
            &QDialog::reject);
    connect(del, &QPushButton::clicked, &dialog, [&dialog] { dialog.done(2); });
    text->setFocus();
    int result = dialog.exec();
    if (result == QDialog::Rejected)
        return;
    if (result == 2) {
        auto id = existing.id;
        m_session->mutateProject("Delete note", [=](Project &p) {
            p.notes.removeIf([=](const Note &n) { return n.id == id; });
        });
        return;
    }
    auto body = text->toPlainText(), by = author->text();
    if (found) {
        auto id = existing.id;
        m_session->mutateProject("Edit note", [=](Project &p) {
            for (auto &n : p.notes)
                if (n.id == id) {
                    n.text = body;
                    n.author = by;
                    n.color = noteColor;
                }
        });
    } else if (!body.trimmed().isEmpty()) {
        Note note;
        note.id = newId();
        note.timelineId = m_session->activeOwnerId();
        note.frame = m_session->currentFrame();
        note.text = body;
        note.author = by;
        note.color = noteColor;
        m_session->mutateProject("Add note", [=](Project &p) { p.notes.append(note); });
    }
    m_notesDock->show();
}
void MainWindow::noteContextMenu(QPoint pos) {
    auto item = m_notesTable->itemAt(pos);
    if (!item)
        return;
    auto first = m_notesTable->item(item->row(), 0);
    auto id = first->data(IdRole).toString(), kind = first->data(KindRole).toString();
    QMenu menu(this);
    if (kind == "note")
        menu.addAction("Edit…", this, [this, id] { editNote(id); });
    menu.addAction("Delete", this, [this, id, kind] {
        m_session->mutateProject("Delete note or bookmark", [=](Project &p) {
            if (kind == "note")
                p.notes.removeIf([=](const Note &n) { return n.id == id; });
            else
                p.bookmarks.removeIf([=](const Bookmark &b) { return b.id == id; });
        });
    });
    menu.exec(m_notesTable->viewport()->mapToGlobal(pos));
}
void MainWindow::exportDialog() {
    if (m_session->durationFrames() <= 0) {
        showError("Import media before exporting.");
        return;
    }
    m_session->pause();
    QSettings preferences;
    QDialog dialog(this);
    dialog.setWindowTitle("Export to File");
    dialog.setMinimumWidth(510);
    auto layout = new QVBoxLayout(&dialog);
    auto output = new QGroupBox("Output", &dialog);
    auto form = new QFormLayout(output);
    auto kind = new QComboBox(output);
    kind->setObjectName("exportKind");
    kind->addItems({"Video file", "Audio file", "Image sequence"});
    kind->setCurrentIndex(preferences.value("export/kind", 0).toInt());
    auto path = new QLineEdit(output);
    path->setObjectName("exportPath");
    path->setText(preferences
                      .value("export/directory",
                             QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                      .toString() +
                  "/" + m_session->project().name +
                  (kind->currentIndex() == 0   ? ".mp4"
                   : kind->currentIndex() == 1 ? ".wav"
                                               : "_frames"));
    auto browse = new QPushButton("Browse…", output);
    form->addRow("Format", kind);
    form->addRow("Output", row(output, {path, browse}));
    auto forceOverwrite = new QCheckBox("Force overwrite without confirmation", output);
    forceOverwrite->setObjectName("exportForceOverwrite");
    forceOverwrite->setChecked(preferences.value("export/forceOverwrite", false).toBool());
    form->addRow(forceOverwrite);
    layout->addWidget(output);
    auto options = new QGroupBox("Export Options", &dialog);
    auto fields = new QFormLayout(options);
    auto input = new QComboBox(options);
    input->setObjectName("exportInput");
    input->addItems({"Viewer Capture", "Timeline Sources"});
    input->setCurrentIndex(preferences.value("export/viewerComposition", true).toBool() ? 0 : 1);
    fields->addRow("Video input", input);
    auto width = new QSpinBox(options), height = new QSpinBox(options);
    width->setRange(16, 8192);
    height->setRange(16, 8192);
    width->setValue(preferences.value("export/width", 1920).toInt());
    height->setValue(preferences.value("export/height", 1080).toInt());
    fields->addRow("Resolution", row(options, {width, new QLabel("×", options), height}));
    Rational ownerRate = m_session->project().fps;
    const auto owner = m_session->activeOwnerId();
    if (auto source = m_session->project().source(owner))
        ownerRate = source->info.fps;
    else if (auto timeline = m_session->project().timeline(owner))
        ownerRate = timeline->fps;
    if (!ownerRate.valid())
        ownerRate = {24, 1};
    auto fpsNum = new QSpinBox(options), fpsDen = new QSpinBox(options);
    fpsNum->setObjectName("exportFpsNumerator");
    fpsDen->setObjectName("exportFpsDenominator");
    fpsNum->setRange(1, 1000000);
    fpsDen->setRange(1, 1000000);
    fpsNum->setValue(int(ownerRate.num));
    fpsDen->setValue(int(ownerRate.den));
    fpsNum->setToolTip("Video only. All frames preserve source timing; stepped captures play "
                       "selected frames consecutively at this constant rate.");
    fpsDen->setToolTip(fpsNum->toolTip());
    fields->addRow("Output FPS (num / den)",
                   row(options, {fpsNum, new QLabel("/", options), fpsDen}));
    auto codec = new QComboBox(options);
    codec->setObjectName("exportCodec");
    codec->addItem("MPEG-4 — MP4 / AVI", "mpeg4");
    codec->addItem("FFV1 — lossless MKV", "ffv1");
    codec->addItem("H.264 — Windows Media Foundation", "h264_mf");
    codec->setCurrentIndex(
        std::max(0, codec->findData(preferences.value("export/videoCodec", "mpeg4"))));
    fields->addRow("Video codec", codec);
    auto imageFormat = new QComboBox(options);
    imageFormat->setObjectName("exportImageFormat");
    imageFormat->addItems({"png", "jpg", "bmp", "tiff"});
    imageFormat->setCurrentText(preferences.value("export/imageFormat", "png").toString());
    fields->addRow("Image sequence format", imageFormat);
    auto colorMode = new QComboBox(options);
    colorMode->setObjectName("exportColorMode");
    colorMode->addItems({"Display converted (sRGB)", "Preserve source color encoding"});
    colorMode->setCurrentIndex(preferences.value("export/colorMode", 0).toInt());
    fields->addRow("Color output", colorMode);
    auto bitDepth = new QComboBox(options);
    bitDepth->setObjectName("exportBitDepth");
    bitDepth->addItem("Automatic", 0);
    for (int depth : {8, 10, 12, 16})
        bitDepth->addItem(QString::number(depth) + " bit", depth);
    bitDepth->setCurrentIndex(
        std::max(0, bitDepth->findData(preferences.value("export/bitDepth", 8))));
    fields->addRow("Component depth", bitDepth);
    auto colorHint = new QLabel(options);
    colorHint->setObjectName("exportColorHint");
    colorHint->setWordWrap(true);
    fields->addRow(colorHint);
    auto quality = new QSpinBox(options);
    quality->setRange(0, 51);
    quality->setValue(preferences.value("export/crf", 18).toInt());
    fields->addRow("Quality (CRF)", quality);
    auto whole = new QCheckBox("Ignore range (entire timeline)", options);
    whole->setChecked(preferences.value("export/whole", false).toBool());
    auto audio = new QCheckBox("Include audio", options);
    audio->setObjectName("exportIncludeAudio");
    audio->setChecked(preferences.value("export/includeAudio", true).toBool());
    auto annotations = new QCheckBox("Include annotations", options);
    annotations->setObjectName("exportIncludeAnnotations");
    annotations->setChecked(preferences.value("export/includeAnnotations", true).toBool());
    fields->addRow(whole);
    fields->addRow(audio);
    fields->addRow(annotations);
    auto frameStep = new QSpinBox(options);
    frameStep->setObjectName("exportFrameStep");
    frameStep->setRange(1, 10000);
    frameStep->setValue(m_session->playbackFrameStep());
    fields->addRow("Capture every N frames", frameStep);
    auto bookmarksOnly = new QCheckBox("Only bookmarked frames (image sequences)", options);
    bookmarksOnly->setObjectName("exportBookmarksOnly");
    fields->addRow(bookmarksOnly);
    auto numbering = new QComboBox(options);
    numbering->addItem("Start at 1", "one_based");
    numbering->addItem("Start at 0", "zero_based");
    numbering->addItem("Match timeline frame", "match_frame");
    numbering->addItem("Custom start", "custom");
    fields->addRow("Sequence numbering", numbering);
    auto sequenceStart = new QSpinBox(options);
    sequenceStart->setRange(0, std::numeric_limits<int>::max());
    sequenceStart->setValue(1);
    fields->addRow("Custom first number", sequenceStart);
    auto sequencePadding = new QSpinBox(options);
    sequencePadding->setRange(1, 12);
    sequencePadding->setValue(6);
    fields->addRow("Sequence padding", sequencePadding);
    auto baseName = new QLineEdit("frame", options);
    fields->addRow("Sequence base name", baseName);
    auto backgroundOverride = new QCheckBox("Override annotation background", options);
    fields->addRow(backgroundOverride);
    auto backgroundColor = new QPushButton("Background color…", options);
    QColor exportBackground = Qt::transparent;
    fields->addRow(backgroundColor);
    connect(backgroundColor, &QPushButton::clicked, &dialog, [&] {
        auto color =
            QColorDialog::getColor(exportBackground, &dialog, "Export annotation background",
                                   QColorDialog::ShowAlphaChannel);
        if (color.isValid()) {
            exportBackground = color;
            backgroundColor->setText(color.name(QColor::HexArgb));
        }
    });
    auto summary = new QLabel(options);
    summary->setWordWrap(true);
    fields->addRow(summary);
    auto optionsScroll = new QScrollArea(&dialog);
    optionsScroll->setObjectName("exportOptionsScroll");
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    optionsScroll->setWidget(options);
    optionsScroll->setMinimumHeight(320);
    optionsScroll->setMaximumHeight(480);
    layout->addWidget(optionsScroll);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText("Export");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto update = [&] {
        bool video = kind->currentIndex() != 1;
        input->setEnabled(video);
        width->setEnabled(video);
        height->setEnabled(video);
        fpsNum->setEnabled(kind->currentIndex() == 0);
        fpsDen->setEnabled(kind->currentIndex() == 0);
        codec->setEnabled(kind->currentIndex() == 0);
        quality->setEnabled(kind->currentIndex() == 0);
        imageFormat->setEnabled(kind->currentIndex() == 2);
        colorMode->setEnabled(video);
        bitDepth->setEnabled(video);
        const int depth = bitDepth->currentData().toInt();
        const bool preserve = colorMode->currentIndex() == 1;
        const bool ffv1 = kind->currentIndex() == 0 && codec->currentData() == "ffv1";
        const bool png = kind->currentIndex() == 2 && imageFormat->currentText() == "png";
        QString colorError;
        if (video && preserve && input->currentIndex() != 1)
            colorError = "Preserving source color requires Timeline Sources.";
        else if (video && preserve && !ffv1 && !png)
            colorError =
                "Preserve source color using FFV1 / MKV or a supported 16-bit PNG profile.";
        else if (video && preserve && png && depth != 16)
            colorError = "Preserving source color in PNG requires explicit 16-bit output.";
        else if (video && depth != 0 && depth != 8 && !ffv1 && !(png && depth == 16))
            colorError = "This format supports 8-bit output. Choose FFV1 for 10/12/16 bit, or PNG "
                         "for 16 bit.";
        buttons->button(QDialogButtonBox::Save)->setEnabled(colorError.isEmpty());
        colorHint->setText(!video                  ? "Color settings do not apply to audio files."
                           : !colorError.isEmpty() ? colorError
                           : preserve
                               ? "Source color metadata is retained. Incompatible source profiles "
                                 "or unsupported codec depths stop the export before publication."
                               : "Uses HDR / Display Color settings. Automatic depth is 8 bit; "
                                 "select 10/12/16 bit explicitly for higher precision.");
        forceOverwrite->setEnabled(true);
        annotations->setEnabled(video);
        const bool sparseImages = kind->currentIndex() == 2 && input->currentIndex() == 0 &&
                                  (frameStep->value() > 1 || bookmarksOnly->isChecked());
        audio->setEnabled(kind->currentIndex() != 1 && !sparseImages);
        audio->setText(sparseImages ? "Audio unavailable for stepped/bookmark images"
                                    : "Include audio");
        audio->setToolTip(sparseImages
                              ? "Sparse image captures do not include a continuous audio.wav file."
                              : "Continuous image sequences can include audio.wav.");
        frameStep->setEnabled(video && input->currentIndex() == 0);
        bookmarksOnly->setEnabled(kind->currentIndex() == 2 && input->currentIndex() == 0);
        numbering->setEnabled(kind->currentIndex() == 2);
        sequenceStart->setEnabled(kind->currentIndex() == 2 &&
                                  numbering->currentData() == "custom");
        sequencePadding->setEnabled(kind->currentIndex() == 2);
        baseName->setEnabled(kind->currentIndex() == 2);
        backgroundOverride->setEnabled(video && input->currentIndex() == 0);
        backgroundColor->setEnabled(video && input->currentIndex() == 0 &&
                                    backgroundOverride->isChecked());
        auto first = whole->isChecked() ? 0 : m_session->project().rangeIn;
        auto last = whole->isChecked()
                        ? m_session->durationFrames() - 1
                        : (m_session->project().rangeOut < 0 ? m_session->durationFrames() - 1
                                                             : m_session->project().rangeOut);
        qint64 sourceDurationUs = ownerRate.frameToUs(last - first + 1);
        if (auto source = m_session->project().source(owner);
            source && first >= 0 && last >= first && last < source->info.frameCount())
            sourceDurationUs = source->info.frameEndUs(last) - source->info.frameStartUs(first);
        summary->setText(
            QString("Source frames %1–%2 · %3 frames · %4\n%5")
                .arg(displayFrameNumber(m_session->project(), m_session->activeOwnerId(), first))
                .arg(displayFrameNumber(m_session->project(), m_session->activeOwnerId(), last))
                .arg(last - first + 1)
                .arg(durationString(sourceDurationUs, 1000000.0))
                .arg(kind->currentIndex() == 2
                         ? "Use an existing directory or enter a new folder path. "
                           "Stepped/bookmark sequences omit continuous audio."
                         : "The final file is published after export completes."));
    };
    connect(numbering, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { update(); });
    for (auto control : {colorMode, bitDepth, imageFormat})
        connect(control, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                [&] { update(); });
    connect(backgroundOverride, &QCheckBox::toggled, &dialog, [&] { update(); });
    connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int k) {
        auto fi = QFileInfo(path->text());
        path->setText(fi.absolutePath() + "/" + fi.completeBaseName() +
                      (k == 0   ? ".mp4"
                       : k == 1 ? ".wav"
                                : "_frames"));
        update();
    });
    connect(input, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] { update(); });
    connect(whole, &QCheckBox::toggled, &dialog, [&] { update(); });
    connect(frameStep, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&] { update(); });
    connect(bookmarksOnly, &QCheckBox::toggled, &dialog, [&] { update(); });
    connect(codec, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int index) {
        if (index == 1) {
            auto fi = QFileInfo(path->text());
            path->setText(fi.absolutePath() + "/" + fi.completeBaseName() + ".mkv");
        }
        update();
    });
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        QString selected;
        if (kind->currentIndex() == 2) {
            selected = LocalFileDialog::getExistingDirectory(
                &dialog, "Image sequence output directory", QFileInfo(path->text()).absolutePath());
        } else
            selected = LocalFileDialog::getSaveFileName(
                &dialog, "Output file", path->text(),
                kind->currentIndex() == 0 ? "Video (*.mp4 *.mkv *.avi)" : "Audio (*.wav *.m4a)");
        if (!selected.isEmpty())
            path->setText(selected);
    });
    update();
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (path->text().trimmed().isEmpty()) {
        showError("Choose an output path.");
        return;
    }
    ExportSettings settings;
    settings.path = path->text().trimmed();
    settings.kind = ExportKind(kind->currentIndex());
    settings.timelineId = m_session->activeOwnerId();
    settings.firstFrame = whole->isChecked() ? 0 : m_session->project().rangeIn;
    settings.lastFrame =
        whole->isChecked() ? m_session->durationFrames() - 1 : m_session->project().rangeOut;
    settings.size = {width->value(), height->value()};
    if (settings.kind == ExportKind::Video) {
        settings.outputFps = {fpsNum->value(), fpsDen->value()};
        if (settings.outputFps.value() < .001 || settings.outputFps.value() > 1000) {
            showError("Output FPS must be between 0.001 and 1000.");
            return;
        }
    }
    settings.viewerComposition = input->currentIndex() == 0;
    settings.includeAnnotations = annotations->isChecked() && settings.kind != ExportKind::Audio;
    settings.includeAudio =
        (audio->isChecked() && audio->isEnabled()) || settings.kind == ExportKind::Audio;
    settings.videoCodec = codec->currentData().toString();
    settings.colorMode = settings.kind == ExportKind::Audio
                             ? ExportColorMode::DisplayConverted
                             : ExportColorMode(colorMode->currentIndex());
    settings.bitDepth = settings.kind == ExportKind::Audio ? 8 : bitDepth->currentData().toInt();
    settings.audioCodec =
        QFileInfo(settings.path).suffix().toLower() == "wav" ? "pcm_s16le" : "aac";
    settings.crf = quality->value();
    settings.imageFormat = imageFormat->currentText();
    settings.frameStep =
        (settings.viewerComposition && settings.kind != ExportKind::Audio) ? frameStep->value() : 1;
    settings.bookmarksOnly = settings.viewerComposition &&
                             settings.kind == ExportKind::ImageSequence &&
                             bookmarksOnly->isChecked();
    settings.frameNumbering = numbering->currentData().toString();
    settings.sequenceStart = sequenceStart->value();
    settings.sequencePadding = sequencePadding->value();
    settings.sequenceBaseName = baseName->text();
    settings.overrideAnnotationBackground =
        settings.viewerComposition && backgroundOverride->isChecked();
    settings.annotationBackground = exportBackground;
    QString outputError;
    QString validatedOutput;
    settings.forceOverwrite = forceOverwrite->isChecked();
    if (settings.kind == ExportKind::ImageSequence) {
        validatedOutput = LocalPathPolicy::validateDirectory(settings.path);
        settings.publishSequenceIntoExistingDirectory = !validatedOutput.isEmpty();
    }
    if (validatedOutput.isEmpty())
        validatedOutput = LocalPathPolicy::validateOutput(settings.path, &outputError);
    if (validatedOutput.isEmpty()) {
        showError(outputError);
        return;
    }
    settings.path = validatedOutput;
    bool existingTargets = false;
    if (settings.publishSequenceIntoExistingDirectory) {
        QStringList patterns{settings.sequenceBaseName + "_*." + settings.imageFormat};
        if (settings.includeAudio && settings.frameStep == 1 && !settings.bookmarksOnly)
            patterns << "audio.wav";
        existingTargets = !LocalFileDialog::localFiles(settings.path, patterns).isEmpty();
    } else if (settings.kind != ExportKind::ImageSequence) {
        existingTargets = !LocalPathPolicy::validateInput(settings.path).isEmpty();
    }
    if (existingTargets && !settings.forceOverwrite) {
        const QString question =
            settings.kind == ExportKind::ImageSequence
                ? "Matching output files exist. Replace selected sequence files after rendering? "
                  "Other files in this directory will be preserved."
                : "The output file exists. Replace it after the export completes?";
        if (QMessageBox::question(this, "Replace output?", question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        settings.forceOverwrite = true;
    }
    m_session->setPlaybackFrameStep(frameStep->value());
    preferences.setValue("export/directory", QFileInfo(settings.path).absolutePath());
    preferences.setValue("export/kind", kind->currentIndex());
    preferences.setValue("export/videoCodec", settings.videoCodec);
    preferences.setValue("export/imageFormat", settings.imageFormat);
    preferences.setValue("export/colorMode", colorMode->currentIndex());
    preferences.setValue("export/bitDepth", bitDepth->currentData());
    preferences.setValue("export/crf", settings.crf);
    preferences.setValue("export/viewerComposition", settings.viewerComposition);
    preferences.setValue("export/includeAudio", audio->isChecked());
    preferences.setValue("export/includeAnnotations", annotations->isChecked());
    preferences.setValue("export/whole", whole->isChecked());
    preferences.setValue("export/forceOverwrite", forceOverwrite->isChecked());
    if (!m_session->exportAsync(settings))
        return;
    m_exportProgress = new QProgressDialog("Rendering frames and audio…", "Cancel", 0, 100, this);
    m_exportProgress->setWindowTitle("Export");
    m_exportProgress->setWindowModality(Qt::WindowModal);
    m_exportProgress->setMinimumDuration(0);
    m_exportProgress->setAutoClose(false);
    m_exportProgress->setAutoReset(false);
    connect(m_exportProgress, &QProgressDialog::canceled, m_session, &ReviewSession::cancelExport);
    m_exportProgress->show();
}
void MainWindow::revealExportedFile(const QString &path) {
    const auto platform = QApplication::platformName();
    if (m_smokeMode || platform == "offscreen" || platform == "minimal")
        return;

    const QFileInfo file(path);
#ifdef Q_OS_WIN
    const auto nativePath = QDir::toNativeSeparators(file.absoluteFilePath()).toStdWString();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool revealed = false;
    if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(SHParseDisplayName(nativePath.c_str(), nullptr, &item, 0, nullptr))) {
            revealed = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
            CoTaskMemFree(item);
        }
        if (SUCCEEDED(initialized))
            CoUninitialize();
    }
#else
    const bool revealed = QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()));
#endif
    if (!revealed) {
        const auto message = tr("File saved. Could not show it in its folder: %1")
                                 .arg(QDir::toNativeSeparators(file.absoluteFilePath()));
        statusBar()->showMessage(message, 15000);
        log(message);
    }
}
void MainWindow::projectSettingsDialog() {
    const auto &p = m_session->project();
    QDialog dialog(this);
    dialog.setWindowTitle("Project Settings");
    auto form = new QFormLayout(&dialog);
    auto name = new QLineEdit(p.name, &dialog);
    auto fpsNum = new QSpinBox(&dialog), fpsDen = new QSpinBox(&dialog);
    fpsNum->setRange(1, 1000000);
    fpsDen->setRange(1, 1000000);
    fpsNum->setValue(int(p.fps.num));
    fpsDen->setValue(int(p.fps.den));
    auto width = new QSpinBox(&dialog), height = new QSpinBox(&dialog);
    width->setRange(16, 8192);
    height->setRange(16, 8192);
    width->setValue(p.viewer.outputSize.width());
    height->setValue(p.viewer.outputSize.height());
    form->addRow("Project name", name);
    form->addRow("Frame rate (rational)", row(&dialog, {fpsNum, new QLabel("/", &dialog), fpsDen}));
    form->addRow("Viewer output", row(&dialog, {width, new QLabel("×", &dialog), height}));
    auto allTimelines = new QCheckBox("Apply frame rate to all timelines", &dialog);
    form->addRow(allTimelines);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString title = name->text();
    Rational fps{fpsNum->value(), fpsDen->value()};
    QSize size(width->value(), height->value());
    bool all = allTimelines->isChecked();
    m_session->mutateProject("Project settings", [=](Project &p) {
        p.name = title;
        p.fps = fps;
        p.viewer.outputSize = size;
        if (all)
            for (auto &t : p.timelines)
                t.fps = fps;
    });
}
void MainWindow::preferencesDialog() {
    QSettings settings;
    QHash<QString, QCheckBox *> optionChecks;
    QHash<QString, QSpinBox *> optionInts;
    QHash<QString, QDoubleSpinBox *> optionDoubles;
    QDialog dialog(this);
    auto addCheck = [&](QFormLayout *form, const QString &label, const QString &key,
                        bool fallback) {
        auto control = new QCheckBox(label, &dialog);
        control->setObjectName(key);
        control->setChecked(settings.value(key, fallback).toBool());
        form->addRow(control);
        optionChecks[key] = control;
        return control;
    };
    auto addInt = [&](QFormLayout *form, const QString &label, const QString &key, int fallback,
                      int minimum, int maximum) {
        auto control = new QSpinBox(&dialog);
        control->setObjectName(key);
        control->setRange(minimum, maximum);
        control->setValue(settings.value(key, fallback).toInt());
        form->addRow(label, control);
        optionInts[key] = control;
        return control;
    };
    auto addDouble = [&](QFormLayout *form, const QString &label, const QString &key,
                         double fallback, double minimum, double maximum) {
        auto control = new QDoubleSpinBox(&dialog);
        control->setObjectName(key);
        control->setRange(minimum, maximum);
        control->setDecimals(2);
        control->setValue(settings.value(key, fallback).toDouble());
        form->addRow(label, control);
        optionDoubles[key] = control;
        return control;
    };
    dialog.setWindowTitle("Preferences");
    dialog.resize(650, 580);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs, 1);
    auto general = new QWidget(tabs);
    auto form = new QFormLayout(general);
    form->setContentsMargins(18, 18, 18, 18);
    auto cache = new QSpinBox(general);
    cache->setRange(64, 32768);
    cache->setSingleStep(128);
    cache->setSuffix(" MiB");
    cache->setValue(settings.value("media/cacheMiB", 512).toInt());
    form->addRow("Memory cache", cache);
    auto pauseScrub = new QCheckBox("Pause playback on viewer scrub", general);
    pauseScrub->setChecked(settings.value("viewer/pauseOnScrub", true).toBool());
    form->addRow(pauseScrub);
    auto panZoom = new QCheckBox("Enable pan / zoom mouse controls", general);
    panZoom->setObjectName("viewer/panZoom");
    panZoom->setChecked(settings.value("viewer/panZoom", true).toBool());
    form->addRow(panZoom);
    auto hideTools = new QCheckBox("Hide annotation tools on launch", general);
    hideTools->setChecked(settings.value("ui/hideAnnotationTools", true).toBool());
    form->addRow(hideTools);
    auto sensitivity = new QDoubleSpinBox(general);
    sensitivity->setRange(.5, 100);
    sensitivity->setValue(settings.value("viewer/scrubPixels", 8).toDouble());
    sensitivity->setSuffix(" px / frame");
    form->addRow("Scrub sensitivity", sensitivity);
    auto recentCount = new QSpinBox(general);
    recentCount->setRange(0, 100);
    recentCount->setValue(
        settings.value("files/recentFiles", settings.value("files/recentCount", 12)).toInt());
    form->addRow("Recent files", recentCount);
    auto recentProjects = addInt(form, "Recent projects", "files/recentProjects", 12, 0, 100);
    Q_UNUSED(recentProjects);
    auto closePolicy = new QComboBox(general);
    closePolicy->addItem("Always", "always");
    closePolicy->addItem("Saved project changes", "saved");
    closePolicy->addItem("Disabled", "disabled");
    closePolicy->setCurrentIndex(
        std::max(0, closePolicy->findData(settings.value("general/confirmClose", "always"))));
    form->addRow("Confirm close", closePolicy);
    addCheck(form, "Open media in new players", "general/newPlayers", false);
    addCheck(form, "Resize to fit video after import", "general/resizeOnLoad", false);
    addCheck(form, "Pause playback on time-control seek", "general/pauseOnSeek", true);
    addCheck(form, "Endless viewer mouse scrub", "viewer/endlessScrub", false);
    addCheck(form, "Highlight the current clip in Timeline", "general/highlightCurrentClip", true);
    addCheck(form, "Playback controls seek range start / end", "general/rangeTransport", false);
    addCheck(form, "Audio on backwards playback", "audio/reverse", true);
    addCheck(form, "Preserve pitch when changing speed", "audio/timeStretching", true);
    addCheck(form, "Show volume as a popup", "audio/popupVolume", false);
    auto tempPath = new QLineEdit(
        settings.value("general/tempDirectory", QDir::tempPath()).toString(), general);
    auto tempBrowse = new QPushButton("Browse…", general);
    form->addRow("Runtime temporary directory", row(general, {tempPath, tempBrowse}));
    connect(tempBrowse, &QPushButton::clicked, &dialog, [&] {
        auto path = LocalFileDialog::getExistingDirectory(&dialog, "Local temporary directory",
                                                          tempPath->text());
        if (!path.isEmpty())
            tempPath->setText(path);
    });
    auto tempInfo = new QLabel("Applies to runtime scratch on the next launch. Atomic export "
                               "staging stays on the output filesystem.",
                               general);
    tempInfo->setWordWrap(true);
    form->addRow(tempInfo);
    auto generalScroll = new QScrollArea(tabs);
    generalScroll->setWidgetResizable(true);
    generalScroll->setFrameShape(QFrame::NoFrame);
    generalScroll->setWidget(general);
    tabs->addTab(generalScroll, "General");
    auto projectTab = new QWidget(tabs);
    auto pf = new QFormLayout(projectTab);
    pf->setContentsMargins(18, 18, 18, 18);
    auto sourceRoot = new QLineEdit(settings.value("project/sourceRoot").toString(), projectTab);
    auto sourceBrowse = new QPushButton("Browse…", projectTab);
    pf->addRow("Source root directory", row(projectTab, {sourceRoot, sourceBrowse}));
    connect(sourceBrowse, &QPushButton::clicked, &dialog, [&] {
        auto path =
            LocalFileDialog::getExistingDirectory(&dialog, "Local source root", sourceRoot->text());
        if (!path.isEmpty())
            sourceRoot->setText(path);
    });
    addInt(pf, "Sequence FPS numerator", "project/sequenceFpsNum", 24, 1, 1000000);
    addInt(pf, "Sequence FPS denominator", "project/sequenceFpsDen", 1, 1, 1000000);
    addInt(pf, "Default first displayed frame", "project/displayOffset", 1, -1000000000,
           1000000000);
    addCheck(pf, "Add a default timeline to new projects", "project/addDefaultTimeline", true);
    addCheck(pf, "Activate the default timeline in new projects", "project/activateDefaultTimeline",
             true);
    addCheck(pf, "Restore pan / zoom on project load", "project/restorePanZoom", true);
    addCheck(pf, "Restore annotation holds on project load", "project/restoreHold", true);
    tabs->addTab(projectTab, "Project");
    auto annotationTab = new QWidget(tabs);
    auto af = new QFormLayout(annotationTab);
    af->setContentsMargins(18, 18, 18, 18);
    addCheck(af, "Use a dialog for text annotations", "annotation/textDialog", true);
    auto textSize =
        addInt(af, "Text dialog size relative to viewer", "annotation/textSize", 50, 10, 95);
    textSize->setSuffix("%");
    auto inlineHint = new QLabel("When disabled, type directly over the viewer. Ctrl+Enter applies "
                                 "the text; Escape discards it.",
                                 annotationTab);
    inlineHint->setWordWrap(true);
    af->addRow(inlineHint);
    auto laserWidth = addDouble(af, "Laser width", "annotation/laserWidth", 3, 1, 40);
    laserWidth->setSuffix(" px");
    auto laserFade =
        addInt(af, "Laser fade after release", "annotation/laserFadeMs", 350, 50, 3000);
    laserFade->setSuffix(" ms");
    QColor laserColor(settings.value("annotation/laserColor", "#ffff3c3c").toString());
    if (!laserColor.isValid())
        laserColor = QColor(255, 60, 60);
    auto laserColorButton = new QPushButton(laserColor.name(), annotationTab);
    laserColorButton->setObjectName("annotation/laserColor");
    af->addRow("Laser color", laserColorButton);
    connect(laserColorButton, &QPushButton::clicked, &dialog, [&] {
        auto chosen = QColorDialog::getColor(laserColor, &dialog, "Laser Color",
                                             QColorDialog::ShowAlphaChannel |
                                                 QColorDialog::DontUseNativeDialog);
        if (chosen.isValid()) {
            laserColor = chosen;
            laserColorButton->setText(chosen.name());
        }
    });
    auto pointerHint = new QLabel(
        "Eyedropper: Shift+I, click to use the sampled color. Laser: Shift+L, drag to point. "
        "Escape leaves either tool. Laser marks fade and are never saved or exported.",
        annotationTab);
    pointerHint->setWordWrap(true);
    af->addRow(pointerHint);
    addCheck(af, "Use pen pressure for pencil / eraser width", "annotation/pressure", true);
    auto smoothing = addInt(af, "Stroke smoothing", "annotation/smoothing", 0, 0, 100);
    smoothing->setSuffix(" %");
    smoothing->setToolTip("Smooth incoming points and pressure. Completed strokes retain their "
                          "shape when this preference changes.");
    auto tabletBackend = new QComboBox(annotationTab);
    tabletBackend->setObjectName("annotation/tabletBackend");
    tabletBackend->addItem("Windows Ink (WM_POINTER)", "windows_ink");
    tabletBackend->addItem("WinTab (tablet driver required)", "wintab");
    tabletBackend->setCurrentIndex(std::max(0, tabletBackend->findData(activeTabletBackend())));
    tabletBackend->setEnabled(tabletBackendSwitchSupported());
    af->addRow("Tablet input", tabletBackend);
    auto tabletStatus = new QLabel(
        tabletBackendSwitchSupported()
            ? "Backend changes apply on OK. WinTab is accepted only if the driver initializes. "
              "A physical pen is required to verify pressure and the eraser."
            : "This platform receives Qt tablet events. Windows Ink / WinTab switching is "
              "available "
              "with the Windows platform plugin in Qt 6.8.3.",
        annotationTab);
    tabletStatus->setWordWrap(true);
    af->addRow(tabletStatus);
    auto tabletEvent =
        new QLabel(m_lastTabletDiagnostic.isEmpty() ? "No pen event received in this window."
                                                    : m_lastTabletDiagnostic,
                   annotationTab);
    tabletEvent->setObjectName("tabletDiagnostic");
    tabletEvent->setWordWrap(true);
    af->addRow("Last pen event", tabletEvent);
    connect(m_viewer, &ViewerWidget::tabletDiagnosticChanged, tabletEvent, &QLabel::setText);
    auto annotationScroll = new QScrollArea(tabs);
    annotationScroll->setWidgetResizable(true);
    annotationScroll->setFrameShape(QFrame::NoFrame);
    annotationScroll->setWidget(annotationTab);
    tabs->addTab(annotationScroll, "Annotation");
    auto advanced = new QWidget(tabs);
    auto adv = new QFormLayout(advanced);
    adv->setContentsMargins(18, 18, 18, 18);
    addDouble(adv, "Shift scrub (pixels / frame)", "viewer/shiftScrubPixels", 32, .1, 200);
    addDouble(adv, "Alt scrub (pixels / frame)", "viewer/altScrubPixels", 2, .1, 200);
    addCheck(adv, "Audio when scrubbing the viewer", "audio/viewerScrub", true);
    addCheck(adv, "Audio when scrubbing global time", "audio/globalScrub", true);
    addCheck(adv, "Audio when scrubbing range time", "audio/rangeScrub", true);
    addCheck(adv, "Enable project undo / redo", "project/undo", true);
    auto rendererInfo = new QLabel("Renderer: Qt raster (software). The viewer and exported "
                                   "annotations share the same CPU compositor.",
                                   advanced);
    rendererInfo->setWordWrap(true);
    adv->addRow(rendererInfo);
    tabs->addTab(advanced, "Advanced");
    auto output = new QWidget(tabs);
    auto ef = new QFormLayout(output);
    ef->setContentsMargins(18, 18, 18, 18);
    auto dir =
        new QLineEdit(settings
                          .value("export/directory",
                                 QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                          .toString(),
                      output);
    auto browse = new QPushButton("Browse…", output);
    ef->addRow("Export directory", row(output, {dir, browse}));
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        auto path =
            LocalFileDialog::getExistingDirectory(&dialog, "Default export directory", dir->text());
        if (!path.isEmpty())
            dir->setText(path);
    });
    auto width = new QSpinBox(output), height = new QSpinBox(output);
    width->setRange(16, 8192);
    height->setRange(16, 8192);
    width->setValue(settings.value("export/width", 1920).toInt());
    height->setValue(settings.value("export/height", 1080).toInt());
    ef->addRow("Default export size", row(output, {width, new QLabel("×", output), height}));
    auto renderer =
        new QLabel("Video, audio and image sequences use the bundled local FFmpeg engine.\nImage "
                   "capture and annotations share the viewer compositor.",
                   output);
    renderer->setWordWrap(true);
    ef->addRow(renderer);
    auto defaultKind = new QComboBox(output);
    defaultKind->addItem("Video", 0);
    defaultKind->addItem("Audio", 1);
    defaultKind->addItem("Image sequence", 2);
    defaultKind->setCurrentIndex(settings.value("export/kind", 0).toInt());
    ef->addRow("Default output", defaultKind);
    auto defaultCodec = new QComboBox(output);
    defaultCodec->addItem("MPEG-4", "mpeg4");
    defaultCodec->addItem("FFV1 lossless", "ffv1");
    defaultCodec->addItem("H.264 Media Foundation", "h264_mf");
    defaultCodec->setCurrentIndex(
        std::max(0, defaultCodec->findData(settings.value("export/videoCodec", "mpeg4"))));
    ef->addRow("Video codec", defaultCodec);
    auto defaultImage = new QComboBox(output);
    defaultImage->addItems({"png", "jpg", "bmp", "tiff"});
    defaultImage->setCurrentText(settings.value("export/imageFormat", "png").toString());
    ef->addRow("Sequence format", defaultImage);
    addInt(ef, "Quality (CRF)", "export/crf", 18, 0, 51);
    addCheck(ef, "Viewer capture by default", "export/viewerComposition", true);
    addCheck(ef, "Include audio by default", "export/includeAudio", true);
    addCheck(ef, "Include annotations by default", "export/includeAnnotations", true);
    addCheck(ef, "Ignore range by default", "export/whole", false);
    addCheck(ef, "Force overwrite without confirmation", "export/forceOverwrite", false);
    auto exportScroll = new QScrollArea(tabs);
    exportScroll->setWidgetResizable(true);
    exportScroll->setFrameShape(QFrame::NoFrame);
    exportScroll->setWidget(output);
    tabs->addTab(exportScroll, "Export");
    auto keyboard = new QWidget(tabs);
    auto kl = new QVBoxLayout(keyboard);
    auto search = new QLineEdit(keyboard);
    search->setPlaceholderText("Filter commands…");
    kl->addWidget(search);
    auto commands = new QListWidget(keyboard);
    QStringList ids = m_actions.keys();
    std::sort(ids.begin(), ids.end(), [this](const auto &a, const auto &b) {
        return m_actions[a]->text() < m_actions[b]->text();
    });
    QHash<QString, QList<QKeySequence>> bindings;
    for (auto id : ids) {
        bindings[id] = m_actions[id]->shortcuts();
        auto item = new QListWidgetItem(m_actions[id]->text().remove('&'), commands);
        item->setData(Qt::UserRole, id);
    }
    kl->addWidget(commands, 1);
    auto assigned = new QComboBox(keyboard);
    auto remove = new QPushButton("Remove", keyboard);
    kl->addWidget(row(keyboard, {new QLabel("Assigned shortcuts", keyboard), assigned, remove}));
    auto keyEdit = new QKeySequenceEdit(keyboard);
    auto assign = new QPushButton("Assign", keyboard);
    kl->addWidget(row(keyboard, {keyEdit, assign}));
    auto conflict = new QLabel(keyboard);
    conflict->setWordWrap(true);
    kl->addWidget(conflict);
    auto defaults = new QPushButton("Restore Defaults", keyboard);
    kl->addWidget(defaults, 0, Qt::AlignLeft);
    tabs->addTab(keyboard, "Keyboard");
    auto refreshAssigned = [&] {
        assigned->clear();
        auto item = commands->currentItem();
        if (!item)
            return;
        for (auto key : bindings[item->data(Qt::UserRole).toString()])
            assigned->addItem(key.toString(QKeySequence::NativeText),
                              key.toString(QKeySequence::PortableText));
    };
    connect(commands, &QListWidget::currentItemChanged, &dialog, [&] { refreshAssigned(); });
    connect(search, &QLineEdit::textChanged, &dialog, [&](const QString &text) {
        for (int i = 0; i < commands->count(); ++i)
            commands->item(i)->setHidden(
                !commands->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(
        keyEdit, &QKeySequenceEdit::keySequenceChanged, &dialog, [&](const QKeySequence &sequence) {
            QStringList found;
            for (auto it = bindings.cbegin(); it != bindings.cend(); ++it)
                if (it.value().contains(sequence))
                    found << m_actions[it.key()]->text();
            conflict->setText(found.isEmpty()
                                  ? "Shortcut is available"
                                  : "Currently assigned to: " + found.join(", ") +
                                        ". Assign moves this shortcut to the selected command.");
        });
    connect(assign, &QPushButton::clicked, &dialog, [&] {
        auto item = commands->currentItem();
        auto key = keyEdit->keySequence();
        if (!item || key.isEmpty())
            return;
        for (auto it = bindings.begin(); it != bindings.end(); ++it)
            it.value().removeAll(key);
        bindings[item->data(Qt::UserRole).toString()].append(key);
        refreshAssigned();
        keyEdit->clear();
        conflict->clear();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        auto item = commands->currentItem();
        if (!item || assigned->currentIndex() < 0)
            return;
        bindings[item->data(Qt::UserRole).toString()].removeAt(assigned->currentIndex());
        refreshAssigned();
    });
    connect(defaults, &QPushButton::clicked, &dialog, [&] {
        bindings = m_defaultShortcuts;
        refreshAssigned();
    });
    if (commands->count())
        commands->setCurrentRow(0);
    auto network = new QWidget(tabs);
    auto nf = new QFormLayout(network);
    nf->setContentsMargins(18, 18, 18, 18);
    auto enabled = new QCheckBox("Enable local integration server", network);
    enabled->setChecked(settings.value("api/enabled", true).toBool());
    auto port = new QSpinBox(network);
    port->setRange(0, 65535);
    port->setSpecialValueText("Automatic");
    port->setValue(settings.value("api/port", 0).toInt());
    nf->addRow(enabled);
    nf->addRow("Port on 127.0.0.1", port);
    auto mayaTimeout =
        addDouble(nf, "Default Maya connection timeout", "maya/timeoutSeconds", .75, .05, 120);
    mayaTimeout->setSuffix(" s");
    auto explanation =
        new QLabel("The adapter discovers the port and authentication token through a private "
                   "local endpoint file. External addresses are not accepted.",
                   network);
    explanation->setWordWrap(true);
    nf->addRow(explanation);
    tabs->addTab(network, "Network");
    tabs->tabBar()->moveTab(tabs->indexOf(keyboard), 2);
    tabs->tabBar()->moveTab(tabs->indexOf(advanced), tabs->count() - 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString directoryError;
    auto validTemp = LocalPathPolicy::validateDirectory(tempPath->text(), &directoryError);
    if (validTemp.isEmpty()) {
        showError(directoryError);
        return;
    }
    QString validRoot;
    if (!sourceRoot->text().isEmpty()) {
        validRoot = LocalPathPolicy::validateDirectory(sourceRoot->text(), &directoryError);
        if (validRoot.isEmpty()) {
            showError(directoryError);
            return;
        }
    }
    if (tabletBackend->isEnabled()) {
        QString tabletError;
        if (!applyTabletBackend(tabletBackend->currentData().toString(), &tabletError)) {
            showError(tabletError);
            return;
        }
        settings.setValue("annotation/tabletBackend", activeTabletBackend());
    }
    for (auto it = optionChecks.cbegin(); it != optionChecks.cend(); ++it)
        settings.setValue(it.key(), it.value()->isChecked());
    for (auto it = optionInts.cbegin(); it != optionInts.cend(); ++it)
        settings.setValue(it.key(), it.value()->value());
    for (auto it = optionDoubles.cbegin(); it != optionDoubles.cend(); ++it)
        settings.setValue(it.key(), it.value()->value());
    settings.setValue("general/tempDirectory", validTemp);
    settings.setValue("general/confirmClose", closePolicy->currentData());
    settings.setValue("project/sourceRoot", validRoot);
    settings.setValue("files/recentFiles", recentCount->value());
    settings.setValue("export/kind", defaultKind->currentIndex());
    settings.setValue("export/videoCodec", defaultCodec->currentData());
    settings.setValue("export/imageFormat", defaultImage->currentText());
    settings.setValue("media/cacheMiB", cache->value());
    settings.setValue("viewer/pauseOnScrub", pauseScrub->isChecked());
    settings.setValue("viewer/panZoom", panZoom->isChecked());
    settings.setValue("viewer/scrubPixels", sensitivity->value());
    settings.setValue("ui/hideAnnotationTools", hideTools->isChecked());
    settings.setValue("annotation/laserColor", laserColor.name(QColor::HexArgb));
    settings.setValue("files/recentCount", recentCount->value());
    settings.setValue("export/directory", dir->text());
    settings.setValue("export/width", width->value());
    settings.setValue("export/height", height->value());
    settings.setValue("api/enabled", enabled->isChecked());
    settings.setValue("api/port", port->value());
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        QStringList values;
        for (auto key : it.value())
            values << key.toString(QKeySequence::PortableText);
        settings.setValue("shortcuts/" + it.key(), values);
        m_actions[it.key()]->setShortcuts(it.value());
    }
    settings.sync();
    applyPreferences();
    if (m_server) {
        m_server->stop();
        if (enabled->isChecked() && !m_server->start(quint16(port->value())))
            showError(m_server->lastError());
    }
}
void MainWindow::mayaDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Maya Tools");
    dialog.resize(560, 480);
    auto layout = new QVBoxLayout(&dialog);
    auto status = new QLabel(&dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto endpoint = new QLineEdit(m_server ? m_server->endpointPath() : QString(), &dialog);
    endpoint->setReadOnly(true);
    auto copy = new QPushButton("Copy endpoint path", &dialog);
    layout->addWidget(row(&dialog, {endpoint, copy}));
    connect(copy, &QPushButton::clicked, &dialog,
            [endpoint] { QApplication::clipboard()->setText(endpoint->text()); });
    auto sessions = new QComboBox(&dialog);
    auto form = new QFormLayout;
    form->addRow("Maya session", sessions);
    auto direction = new QComboBox(&dialog);
    direction->addItem("Off", "off");
    direction->addItem("Maya → OpenFrame", "maya_to_openframe");
    direction->addItem("OpenFrame → Maya", "openframe_to_maya");
    direction->addItem("Both directions", "bidirectional");
    form->addRow("Sync direction", direction);
    auto offset = new QDoubleSpinBox(&dialog);
    offset->setRange(-1000000, 1000000);
    offset->setDecimals(3);
    form->addRow("Frame offset", offset);
    auto multiplier = new QDoubleSpinBox(&dialog);
    multiplier->setRange(.001, 1000);
    multiplier->setDecimals(6);
    multiplier->setValue(1);
    form->addRow("Frame multiplier", multiplier);
    auto fromRange = new QCheckBox("Offset from range start", &dialog);
    form->addRow(fromRange);
    auto timeout = new QDoubleSpinBox(&dialog);
    timeout->setObjectName("mayaTimeoutSeconds");
    timeout->setRange(.05, 120);
    timeout->setDecimals(2);
    timeout->setSuffix(" s");
    timeout->setValue(QSettings().value("maya/timeoutSeconds", .75).toDouble());
    form->addRow("Connection timeout", timeout);
    auto rememberTimeout = new QCheckBox("Use this timeout as the next-session default", &dialog);
    form->addRow(rememberTimeout);
    layout->addLayout(form);
    auto apply = new QPushButton("Apply to Maya session", &dialog);
    layout->addWidget(apply);
    auto instructions = new QTextBrowser(&dialog);
    instructions->setOpenExternalLinks(false);
    instructions->setHtml(
        "<p><b>Connect from Maya</b></p><p>Install the bundled <code>maya/openframe_maya</code> "
        "package on Maya's Python path and run:</p><pre>import "
        "openframe_maya\nopenframe_maya.show()</pre><p>Use the adapter's Connect button and its "
        "sync/playblast controls. Select a connected session above to update synchronization "
        "settings.</p><p>The transport is authenticated and limited to <code>127.0.0.1</code>. No "
        "arbitrary Python or MEL commands are accepted by this app.</p>");
    layout->addWidget(instructions, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto loadConfig = [&] {
        if (!m_server)
            return;
        auto selected = sessions->currentData().toString();
        for (auto value : m_server->mayaSessions()) {
            auto config = value.toObject();
            if (config.value("sessionId").toString() != selected)
                continue;
            direction->setCurrentIndex(
                std::max(0, direction->findData(config.value("direction").toString())));
            offset->setValue(config.value("offset").toDouble());
            multiplier->setValue(config.value("multiplier").toDouble(1));
            fromRange->setChecked(config.value("fromRangeStart").toBool());
            timeout->setValue(
                config.value("timeoutSeconds")
                    .toDouble(QSettings().value("maya/timeoutSeconds", .75).toDouble()));
        }
    };
    connect(sessions, qOverload<int>(&QComboBox::activated), &dialog, [&] { loadConfig(); });
    auto refresh = [&] {
        QString selected = sessions->currentData().toString();
        QSignalBlocker block(sessions);
        sessions->clear();
        if (m_server) {
            status->setText(
                m_server->isListening()
                    ? QString("Listening on 127.0.0.1:%1").arg(m_server->listeningPort())
                    : "Local API server is stopped. Enable it in Preferences → Network.");
            endpoint->setText(m_server->endpointPath());
            for (auto value : m_server->mayaSessions()) {
                auto obj = value.toObject();
                sessions->addItem(obj.value("name").toString("Maya"),
                                  obj.value("sessionId").toString());
            }
            int index = sessions->findData(selected);
            if (index >= 0)
                sessions->setCurrentIndex(index);
        } else
            status->setText("Integration server is unavailable.");
        apply->setEnabled(sessions->count() > 0);
        loadConfig();
    };
    connect(apply, &QPushButton::clicked, &dialog, [&] {
        if (!m_server || sessions->currentIndex() < 0)
            return;
        QJsonObject config{{"direction", direction->currentData().toString()},
                           {"offset", offset->value()},
                           {"multiplier", multiplier->value()},
                           {"fromRangeStart", fromRange->isChecked()},
                           {"timeoutSeconds", timeout->value()}};
        if (m_server->configureMayaSession(sessions->currentData().toString(), config)) {
            if (rememberTimeout->isChecked()) {
                QSettings().setValue("maya/timeoutSeconds", timeout->value());
                m_server->setDefaultMayaTimeout(timeout->value());
            }
            status->setText("Synchronization settings sent to the selected Maya adapter.");
        } else
            status->setText(m_server->lastError());
    });
    if (m_server)
        connect(m_server, &ApiServer::clientsChanged, &dialog, refresh);
    refresh();
    dialog.exec();
}
void MainWindow::documentationDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("OpenFrame — Local Help");
    dialog.resize(650, 530);
    auto layout = new QVBoxLayout(&dialog);
    auto text = new QTextBrowser(&dialog);
    text->setOpenExternalLinks(false);
    text->setHtml(
        "<h2>OpenFrame</h2><p>Import local media with <b>Ctrl+I</b> or drop files into the viewer. "
        "Double-click a source or timeline to assign it to A. Use the A–D selectors to compare "
        "sources.</p><h3>Playback and review</h3><p><b>Space</b> plays or pauses, <b>J/K/L</b> "
        "reverse/pause/forward, arrow keys step one frame, <b>Shift+arrows</b> step ten. Drag "
        "horizontally in the viewer to scrub. Drag the gold handles on the time control to adjust "
        "the playback range. <b>I/O</b> set the range at the current "
        "frame.</p><h3>Viewer</h3><p><b>Ctrl+wheel</b> zooms. <b>Ctrl+middle drag</b> pans. "
        "Double-click fits the viewer. In Video → Layout, Single A and Single B show a fixed "
        "slot. Active follows the selected slot. Horizontal places A and B side by side, and "
        "Vertical stacks A above B. Use the slider in Transparency to blend the two images, "
        "or in Wipe to move the divider. You can also <b>drag the divider</b> directly. "
        "Drag the round handle to rotate and the pivot to reposition. "
        "Ctrl temporarily disables center/right-angle snapping; Alt allows wipe edits with "
        "annotation tools open. Video → Wipe offers exact values and reset. HDR / Display Color "
        "controls the display conversion; export color encoding and bit depth are independent "
        "options.</p><h3>Project</h3><p>Use the Project panel context menu to "
        "make folders and timelines, append a source, assign a viewer or relink missing media. "
        "Drag sources onto a timeline or folder. In Details → Timeline, double-click clips to edit "
        "in/out, gain and audio offsets; use arrow buttons to reorder clips.</p><h3>Annotations "
        "and notes</h3><p><b>F8</b> opens drawing tools. Pencil, arrow, shapes and text draw in "
        "the viewer. FG is frame-specific; BG remains visible. Hold retains a drawing until the "
        "next keyframe. Ghost displays nearby drawings. Pen pressure changes pencil/eraser width. "
        "The pen eraser temporarily selects erasing; it removes pixels only from the current "
        "owner, frame and layer. Preferences → Annotation chooses smoothing and the Windows "
        "Ink / WinTab input backend. Completed strokes retain their shape. Escape discards a "
        "stroke or wipe drag before committing. <b>B</b> toggles a bookmark, <b>Ctrl+W</b> "
        "edits a note. Notes import/export uses CSV. Undo/redo applies to project edits and "
        "annotations.</p><h3>Files and integration</h3><p><b>Ctrl+S</b> saves an open JSON-based "
        ".oframe project. <b>Ctrl+E</b> exports the active timeline or composed viewer. Maya Tools "
        "displays the local adapter endpoint and connected sessions. Preferences includes editable "
        "command shortcuts, cache, input, export and API settings.</p><p><b>Development "
        "status:</b> this is an independent implementation. Exact reference behavior, proprietary "
        "project compatibility and pixel-level visual parity remain unverified. This help is "
        "local; no browser is opened.</p>");
    layout->addWidget(text);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void MainWindow::applyPreferences() {
    QSettings s;
    if (m_server)
        m_server->setDefaultMayaTimeout(s.value("maya/timeoutSeconds", .75).toDouble());
    m_session->setPlaybackFrameStep(s.value("playback/frameStep", 1).toInt());
    for (auto key :
         {QString("loopStep"), QString("loopRange"), QString("lockRange"), QString("autoplay")})
        m_actions[key]->setChecked(s.value("playback/" + key, false).toBool());
    m_session->setLoopOnStepScrub(m_actions["loopStep"]->isChecked());
    m_session->setLoopStepScrubToRange(m_actions["loopRange"]->isChecked());
    m_session->setLockStepScrubToRange(m_actions["lockRange"]->isChecked());
    m_session->setAutoplay(m_actions["autoplay"]->isChecked());
    m_session->setCacheLimitMiB(s.value("media/cacheMiB", 512).toInt());
    m_viewer->setScrubSensitivity(s.value("viewer/scrubPixels", 8).toDouble());
    m_viewer->setPauseOnScrub(s.value("viewer/pauseOnScrub", true).toBool());
    m_viewer->setPanZoomEnabled(s.value("viewer/panZoom", true).toBool());
    m_viewer->setScrubModifiers(s.value("viewer/shiftScrubPixels", 32).toDouble(),
                                s.value("viewer/altScrubPixels", 2).toDouble());
    m_viewer->setEndlessScrub(s.value("viewer/endlessScrub", false).toBool());
    m_viewer->setAudioOnScrub(s.value("audio/viewerScrub", true).toBool());
    m_viewer->setTextDialog(s.value("annotation/textDialog", true).toBool(),
                            s.value("annotation/textSize", 50).toDouble() / 100.0);
    m_pauseOnSeek = s.value("general/pauseOnSeek", true).toBool();
    m_highlightCurrentClip = s.value("general/highlightCurrentClip", true).toBool();
    m_rangeTransport = s.value("general/rangeTransport", false).toBool();
    m_timeline->setPauseOnSeek(m_pauseOnSeek);
    m_rangeTimeline->setPauseOnSeek(m_pauseOnSeek);
    m_timeline->setAudioOnScrub(s.value("audio/globalScrub", true).toBool());
    m_rangeTimeline->setAudioOnScrub(s.value("audio/rangeScrub", true).toBool());
    m_session->setReverseAudioEnabled(s.value("audio/reverse", true).toBool());
    m_session->setAudioTimeStretchingEnabled(s.value("audio/timeStretching", true).toBool());
    m_viewer->setLaserSettings(QColor(s.value("annotation/laserColor", "#ffff3c3c").toString()),
                               s.value("annotation/laserWidth", 3).toDouble(),
                               s.value("annotation/laserFadeMs", 350).toInt());
    if (tabletBackendSwitchSupported()) {
        QString tabletError;
        if (!applyTabletBackend(s.value("annotation/tabletBackend", "windows_ink").toString(),
                                &tabletError))
            log(tabletError);
    }
    m_viewer->setTabletOptions(s.value("annotation/pressure", true).toBool(),
                               s.value("annotation/smoothing", 0).toDouble() / 100,
                               activeTabletBackend());
    for (auto key : {QString("wipeSnap"), QString("wipeGuides")})
        m_actions[key]->setChecked(s.value("viewer/" + key, true).toBool());
    m_viewer->setWipeOptions(m_actions["wipeSnap"]->isChecked(),
                             m_actions["wipeGuides"]->isChecked());
    setBookmarkFilter(BookmarkFilter(std::clamp(s.value("bookmarks/filter", 0).toInt(), 0, 3)));
    m_session->setProjectUndoEnabled(s.value("project/undo", true).toBool());
    m_session->setProjectDefaults({s.value("project/sequenceFpsNum", 24).toLongLong(),
                                   s.value("project/sequenceFpsDen", 1).toLongLong()},
                                  s.value("project/displayOffset", 1).toLongLong(),
                                  s.value("project/addDefaultTimeline", true).toBool(),
                                  s.value("project/activateDefaultTimeline", true).toBool(),
                                  s.value("project/sourceRoot").toString());
    m_session->setProjectLoadOptions(s.value("project/restorePanZoom", true).toBool(),
                                     s.value("project/restoreHold", true).toBool());
    bool popupVolume = s.value("audio/popupVolume", false).toBool();
    m_volumeControlAction->setVisible(!popupVolume);
    m_volumePopupAction->setVisible(popupVolume);
    if (m_firstPreferencesApply) {
        m_session->setVolume(s.value("audio/volume", 100).toDouble() / 100.0);
        m_session->setAudioEnabled(!s.value("audio/muted", false).toBool());
        m_session->setAudioScrubEnabled(s.value("audio/scrub", true).toBool());
    }
    if (m_firstPreferencesApply && s.value("ui/hideAnnotationTools", true).toBool()) {
        m_annotationBar->hide();
        m_annotationOptions->hide();
        m_viewer->setAnnotationsEnabled(false);
        m_actions["annotationsTools"]->setChecked(false);
    }
    for (auto it = m_actions.begin(); it != m_actions.end(); ++it)
        if (s.contains("shortcuts/" + it.key())) {
            QList<QKeySequence> keys;
            for (auto text : s.value("shortcuts/" + it.key()).toStringList())
                keys << QKeySequence(text, QKeySequence::PortableText);
            it.value()->setShortcuts(keys);
        }
    m_firstPreferencesApply = false;
    rebuildRecentMenus();
    refreshFrame(m_session->currentFrame());
    refreshTransportState();
}
void MainWindow::storeSettings() {
    if (m_smokeMode)
        return;
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    s.setValue("audio/volume", m_volume->value());
    s.setValue("audio/muted", m_actions["mute"]->isChecked());
    s.setValue("audio/scrub", m_session->audioScrubEnabled());
    s.setValue("viewer/lastOverlayType", int(m_lastOverlayType));
    auto overlayDefaults = emptyProject();
    overlayDefaults.viewer.overlay = m_session->project().viewer.overlay;
    s.setValue("viewer/overlayDefaults",
               QJsonDocument(projectToJson(overlayDefaults)).toJson(QJsonDocument::Compact));
    s.sync();
}
void MainWindow::resetLayout() {
    restoreState(m_defaultLayout);
    m_annotationBar->hide();
    m_annotationOptions->hide();
    m_viewer->setAnnotationsEnabled(false);
    m_actions["annotationsTools"]->setChecked(false);
    m_notesDock->hide();
    m_consoleDock->hide();
    m_timeline->show();
    m_rangeTimeline->hide();
    menuBar()->show();
    m_playbackBar->show();
    m_viewerBar->show();
    resizeDocks({m_projectDock}, {260}, Qt::Horizontal);
}
void MainWindow::updateTitle() {
    setWindowTitle(
        QString("%1%2 — OpenFrame").arg(m_session->project().name, m_session->dirty() ? " *" : ""));
}
void MainWindow::updateRecent(const QString &key, const QString &path) {
    QSettings s;
    auto list = s.value(key).toStringList();
    list.removeAll(path);
    list.prepend(path);
    int limit = s.value(key == "recent/projects" ? "files/recentProjects" : "files/recentFiles",
                        s.value("files/recentCount", 12))
                    .toInt();
    while (list.size() > limit)
        list.removeLast();
    s.setValue(key, list);
    rebuildRecentMenus();
}
void MainWindow::rebuildRecentMenus() {
    if (!m_recentFilesMenu || !m_recentProjectsMenu)
        return;
    QSettings s;
    for (auto pair : {std::pair{m_recentFilesMenu, QString("recent/files")},
                      std::pair{m_recentProjectsMenu, QString("recent/projects")}}) {
        pair.first->clear();
        auto paths = s.value(pair.second).toStringList();
        int count = 0, limit = s.value(pair.second == "recent/projects" ? "files/recentProjects"
                                                                        : "files/recentFiles",
                                       s.value("files/recentCount", 12))
                                   .toInt();
        for (auto path : paths) {
            if (count++ >= limit)
                break;
            pair.first->addAction(QFileInfo(path).fileName(), this,
                                  [this, path, key = pair.second] {
                                      if (key == "recent/projects") {
                                          if (confirmClose() && m_session->openProject(path))
                                              updateRecent(key, path);
                                      } else {
                                          importMediaPaths({path});
                                          updateRecent(key, path);
                                      }
                                  });
        }
        if (pair.first->actions().isEmpty()) {
            auto action = pair.first->addAction("No recent items");
            action->setEnabled(false);
        }
    }
}
void MainWindow::log(const QString &text) {
    if (m_console)
        m_console->append(QDateTime::currentDateTime().toString("hh:mm:ss") + "  " +
                          text.toHtmlEscaped());
}
void MainWindow::showError(const QString &text) {
    log(text);
    statusBar()->showMessage(text, 12000);
    if (!m_smokeMode) {
        auto message =
            new QMessageBox(QMessageBox::Warning, "OpenFrame", text, QMessageBox::Ok, this);
        message->setTextFormat(Qt::PlainText);
        message->setAttribute(Qt::WA_DeleteOnClose);
        message->setWindowModality(Qt::NonModal);
        message->show();
    }
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!confirmClose()) {
        event->ignore();
        return;
    }
    m_bookmarkTimer->stop();
    m_session->pause();
    m_session->cancelExport();
    storeSettings();
    event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (auto url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty())
        importMediaPaths(paths);
    e->acceptProposedAction();
}
void MainWindow::renderSyntheticPreview(const QImage &image) {
    m_viewer->setPreviewImage(image);
    m_thumbnail->setPixmap(
        QPixmap::fromImage(image).scaled(250, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_notesDock->show();
    m_annotationBar->show();
    m_annotationOptions->show();
    m_viewer->setAnnotationsEnabled(true);
    m_actions["annotationsTools"]->setChecked(true);
}
QJsonObject MainWindow::captureSyntheticTransientTools(const QString &directory) {
    const auto safe = LocalPathPolicy::validateDirectory(directory);
    if (!m_smokeMode || safe.isEmpty())
        return {{"success", false}};
    const auto before =
        QJsonDocument(projectToJson(m_session->project())).toJson(QJsonDocument::Compact);
    const int history = m_session->undoStack()->index();
    const bool dirty = m_session->dirty();
    const bool toolsVisible = m_annotationBar->isVisible();
    const auto previous = m_viewer->transientTool();
    QJsonArray captures;
    bool saved = true;
    auto capture = [&](const QString &name) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const auto path = LocalPathPolicy::validateOutput(QDir(safe).filePath(name + ".png"));
        const bool ok = !path.isEmpty() && grab().save(path);
        captures.append(QJsonObject{{"file", name + ".png"}, {"saved", ok}});
        saved &= ok;
    };
    auto event = [&](QEvent::Type type, QPointF position, Qt::MouseButton button,
                     Qt::MouseButtons buttons) {
        QMouseEvent mouse(type, position, m_viewer->mapToGlobal(position), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(m_viewer, &mouse);
    };
    m_actions["eyedropper"]->trigger();
    const auto area = m_viewer->imageRect();
    const QPointF center = area.center();
    event(QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    capture("eyedropper");
    const bool sampled = m_viewer->sampledColor().isValid();
    m_actions["laserPointer"]->trigger();
    event(QEvent::MouseButtonPress, center - QPointF(80, 30), Qt::LeftButton, Qt::LeftButton);
    event(QEvent::MouseMove, center, Qt::NoButton, Qt::LeftButton);
    event(QEvent::MouseMove, center + QPointF(80, -30), Qt::NoButton, Qt::LeftButton);
    capture("laser-active");
    const bool laserDrawn = m_viewer->laserStrokeVisible();
    event(QEvent::MouseButtonRelease, center + QPointF(80, -30), Qt::LeftButton, Qt::NoButton);
    QEventLoop wait;
    QTimer poll;
    poll.setInterval(20);
    connect(&poll, &QTimer::timeout, &wait, [&] {
        if (!m_viewer->laserStrokeVisible())
            wait.quit();
    });
    QTimer::singleShot(3500, &wait, &QEventLoop::quit);
    poll.start();
    wait.exec();
    poll.stop();
    capture("laser-faded");
    const bool faded = !m_viewer->laserStrokeVisible();
    m_viewer->setTransientTool(previous);
    if (!toolsVisible)
        m_annotationBar->hide();
    const bool unchanged =
        before ==
            QJsonDocument(projectToJson(m_session->project())).toJson(QJsonDocument::Compact) &&
        history == m_session->undoStack()->index() && dirty == m_session->dirty();
    return {{"success", saved && sampled && laserDrawn && faded && unchanged},
            {"captures", captures},
            {"colorSampled", sampled},
            {"laserDrawn", laserDrawn},
            {"laserFaded", faded},
            {"projectAndHistoryUnchanged", unchanged}};
}
QJsonObject MainWindow::captureSyntheticDrawingTools(const QString &directory) {
    const auto safe = LocalPathPolicy::validateDirectory(directory);
    if (!m_smokeMode || safe.isEmpty())
        return {{"success", false}};
    const auto before = projectToJson(m_session->project());
    const auto history = m_session->undoStack()->index();
    const auto dirty = m_session->dirty();
    QJsonArray captures;
    bool saved = true;
    auto render = [&](const std::function<bool()> &request) {
        QEventLoop wait;
        bool received = false;
        auto connection = connect(m_session, &ReviewSession::imageReady, &wait, [&] {
            received = true;
            wait.quit();
        });
        QTimer::singleShot(3000, &wait, &QEventLoop::quit);
        const bool accepted = request();
        if (accepted)
            wait.exec();
        disconnect(connection);
        return accepted && received;
    };
    auto capture = [&](const QString &name) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const auto path = LocalPathPolicy::validateOutput(QDir(safe).filePath(name + ".png"));
        const bool ok = !path.isEmpty() && grab().save(path);
        captures.append(QJsonObject{
            {"file", name + ".png"}, {"saved", ok}, {"devicePixelRatio", devicePixelRatioF()}});
        saved &= ok;
    };
    Annotation stroke;
    stroke.id = newId();
    stroke.timelineId = displayedAnnotationOwner(m_session->project());
    stroke.frame = stroke.endFrame =
        displayedAnnotationFrame(m_session->project(), m_session->currentFrame());
    stroke.points = {{.12, .80}, {.25, .68}, {.38, .80}, {.55, .63}, {.68, .77}, {.84, .65}};
    stroke.widthScales = {.05, .2, .4, .65, .85, 1};
    stroke.width = 24;
    stroke.color = QColor("#efc96f");
    auto token = m_session->beginViewerPreview();
    bool pressure = render([&] { return m_session->previewAnnotation(token, stroke); });
    capture("pressure-preview");
    render([&] {
        m_session->clearViewerPreview(token);
        return true;
    });
    stroke.tool = AnnotationTool::Eraser;
    stroke.width = 65;
    stroke.points = {{.58, .15}, {.58, .85}};
    stroke.widthScales = {1, 1};
    token = m_session->beginViewerPreview();
    bool eraser = render([&] { return m_session->previewAnnotation(token, stroke); });
    capture("eraser-preview");
    render([&] {
        m_session->clearViewerPreview(token);
        return true;
    });
    auto wipe = m_session->project().viewer;
    wipe.mode = ViewerMode::Wipe;
    wipe.wipe = .45;
    wipe.wipePivotY = .54;
    wipe.wipeAngleDegrees = 37;
    while (wipe.assignments.size() < 2)
        wipe.assignments.append(QString{});
    if (!m_session->project().sources.isEmpty())
        wipe.assignments[1] = m_session->project().sources.first().id;
    wipe.transforms[1].flipHorizontal = true;
    token = m_session->beginViewerPreview();
    bool rotated = render([&] { return m_session->previewViewerSettings(token, wipe); });
    capture("wipe-rotated-preview");
    render([&] {
        m_session->clearViewerPreview(token);
        return true;
    });
    const bool unchanged = before == projectToJson(m_session->project()) &&
                           history == m_session->undoStack()->index() &&
                           dirty == m_session->dirty();
    return {{"success", saved && pressure && eraser && rotated && unchanged},
            {"captures", captures},
            {"pressurePreviewRendered", pressure},
            {"eraserPreviewRendered", eraser},
            {"rotatedWipePreviewRendered", rotated},
            {"projectAndHistoryUnchanged", unchanged},
            {"tabletBackend", activeTabletBackend()},
            {"tabletBackendSwitchSupported", tabletBackendSwitchSupported()},
            {"physicalTabletTested", false}};
}
QJsonObject MainWindow::captureSyntheticDialogs(const QString &directory) {
    QJsonObject report;
    QString error;
    const auto safeDirectory = LocalPathPolicy::validateDirectory(directory, &error);
    if (!m_smokeMode || safeDirectory.isEmpty())
        return QJsonObject{{"success", false}, {"error", error}};
    const auto before =
        QJsonDocument(projectToJson(m_session->project())).toJson(QJsonDocument::Compact);
    QJsonArray captures;
    bool succeeded = true;
    auto capture = [&](const QString &name, const std::function<void()> &openDialog) {
        QObject timerScope;
        bool observed = false;
        QTimer::singleShot(0, &timerScope, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(80, dialog, [&, dialog] {
                auto save = [&](const QString &suffix) {
                    QString pathError;
                    const QString path = LocalPathPolicy::validateOutput(
                        QDir(safeDirectory).filePath(name + suffix + ".png"), &pathError);
                    bool visibleButtons = true;
                    for (auto box : dialog->findChildren<QDialogButtonBox *>())
                        if (box->isVisible())
                            visibleButtons &= dialog->rect().contains(
                                QRect(box->mapTo(dialog, QPoint()), box->size()));
                    const bool saved = !path.isEmpty() && dialog->grab().save(path);
                    succeeded &= saved && visibleButtons;
                    captures.append(QJsonObject{{"file", name + suffix + ".png"},
                                                {"saved", saved},
                                                {"buttonsInsideDialog", visibleButtons},
                                                {"logicalWidth", dialog->width()},
                                                {"logicalHeight", dialog->height()},
                                                {"devicePixelRatio", dialog->devicePixelRatioF()}});
                };
                auto saveWithScroll = [&](const QString &suffix) {
                    save(suffix);
                    for (auto area : dialog->findChildren<QScrollArea *>()) {
                        if (!area->isVisible() || area->verticalScrollBar()->maximum() <= 0)
                            continue;
                        const int previous = area->verticalScrollBar()->value();
                        for (int pass = 0; pass < 3; ++pass) {
                            if (area->widget() && area->widget()->layout())
                                area->widget()->layout()->activate();
                            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                            area->verticalScrollBar()->setValue(
                                area->verticalScrollBar()->maximum());
                        }
                        save(suffix + "-bottom");
                        area->verticalScrollBar()->setValue(previous);
                    }
                };
                if (name == "overlays") {
                    auto type = dialog->findChild<QComboBox *>("overlayType");
                    if (type) {
                        for (auto index : {1, 2, 3, 4, 5}) {
                            type->setCurrentIndex(index);
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                            saveWithScroll("-" + type->currentText().toLower().replace(' ', '_'));
                        }
                        type->setCurrentIndex(int(OverlayType::Custom));
                        if (auto add = dialog->findChild<QPushButton *>("overlayAddShape"))
                            add->click();
                        if (auto shape = dialog->findChild<QComboBox *>("overlayShapeType"))
                            shape->setCurrentIndex(int(OverlayShapeType::Rectangle));
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        saveWithScroll("-custom");
                        type->setCurrentIndex(int(OverlayType::Image));
                        const auto files = LocalFileDialog::localFiles(
                            QDir(safeDirectory).filePath("synthetic_frames"), {"*.png"});
                        if (auto path = dialog->findChild<QLineEdit *>("overlayImagePath");
                            path && !files.isEmpty())
                            path->setText(files.first());
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        saveWithScroll("-image");
                    } else
                        succeeded = false;
                } else if (name == "preferences") {
                    auto tabs = dialog->findChild<QTabWidget *>();
                    if (tabs) {
                        for (int i = 0; i < tabs->count(); ++i) {
                            tabs->setCurrentIndex(i);
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                            saveWithScroll("-" + tabs->tabText(i).toLower().replace(' ', '_'));
                        }
                    } else {
                        succeeded = false;
                    }
                } else if (name == "export") {
                    saveWithScroll({});
                    auto kind = dialog->findChild<QComboBox *>("exportKind");
                    if (kind) {
                        if (auto codec = dialog->findChild<QComboBox *>("exportCodec"))
                            codec->setCurrentIndex(codec->findData("ffv1"));
                        if (auto depth = dialog->findChild<QComboBox *>("exportBitDepth"))
                            depth->setCurrentIndex(depth->findData(16));
                        if (auto input = dialog->findChild<QComboBox *>("exportInput"))
                            input->setCurrentIndex(1);
                        if (auto color = dialog->findChild<QComboBox *>("exportColorMode"))
                            color->setCurrentIndex(1);
                        saveWithScroll("-source-color-16bit");
                        if (auto color = dialog->findChild<QComboBox *>("exportColorMode"))
                            color->setCurrentIndex(0);
                        if (auto input = dialog->findChild<QComboBox *>("exportInput"))
                            input->setCurrentIndex(0);
                        kind->setCurrentIndex(2);
                        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                        saveWithScroll("-sequence");
                        if (auto step = dialog->findChild<QSpinBox *>("exportFrameStep")) {
                            step->setValue(2);
                            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                            saveWithScroll("-sequence-stepped");
                        }
                    } else {
                        succeeded = false;
                    }
                } else {
                    saveWithScroll({});
                }
                observed = true;
                dialog->reject();
            });
        });
        QTimer::singleShot(3000, &timerScope, [&] {
            if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        openDialog();
        succeeded &= observed;
    };
    capture("preferences", [this] { preferencesDialog(); });
    capture("export", [this] { exportDialog(); });
    capture("note", [this] { editNote(); });
    capture("ghosting", [this] { ghostingDialog(); });
    capture("wipe-options", [this] { editWipeSettings(m_session, this); });
    capture("display-color", [this] { editColorSettings(m_session, this); });
    capture("overlays", [this] { overlayDialog(); });
    capture("loop-delay", [this] { loopDelayDialog(); });
    capture("project-settings", [this] { projectSettingsDialog(); });
    capture("owner-properties", [this] { editSelectedProperties(); });
    if (!m_session->project().sources.isEmpty()) {
        QSignalBlocker blockedSelection(m_projectTree);
        auto previous = m_projectTree->currentItem();
        const auto id = m_session->project().sources.first().id;
        for (QTreeWidgetItemIterator it(m_projectTree); *it; ++it)
            if ((*it)->data(0, IdRole).toString() == id) {
                m_projectTree->setCurrentItem(*it);
                capture("source-properties", [this] { editSelectedProperties(); });
                break;
            }
        m_projectTree->setCurrentItem(previous);
    }
    capture("maya", [this] { mayaDialog(); });
    capture("help", [this] { documentationDialog(); });
    // Captures need a deterministic QWidget dialog. This explicit QA override
    // must not be mistaken for validation of the Windows native shell dialog.
    const bool nativeDialogsDisabled = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    capture("local-file-picker", [&] {
        LocalFileDialog::getOpenFileNames(this, "Local media picker — synthetic fixture",
                                          QDir(safeDirectory).filePath("synthetic_frames"),
                                          "Synthetic frames (*.png)");
    });
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    report.insert("filePickerBackend", "custom fallback for synthetic capture");
    report.insert("nativeFileDialogsValidated", false);
    const QString csv =
        LocalPathPolicy::validateOutput(QDir(safeDirectory).filePath("preview-notes.csv"));
    if (!csv.isEmpty() && m_session->saveNotesCsv(csv))
        capture("csv-preview", [&] { importNotesCsvDialog(m_session, this, csv); });
    else
        succeeded = false;
    report.insert("captures", captures);
    report.insert(
        "projectUnchanged",
        before ==
            QJsonDocument(projectToJson(m_session->project())).toJson(QJsonDocument::Compact));
    succeeded &= report.value("projectUnchanged").toBool();
    report.insert("success", succeeded);
    return report;
}
} // namespace openframe
