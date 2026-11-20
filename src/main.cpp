#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QRawFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QAction>
#include <memory>
#include <cmath>
#include <algorithm>
#include "core/ReviewSession.h"
#include "integration/ApiServer.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/LocalFileDialog.h"
#include "integration/LocalPathPolicy.h"
#include <QStandardPaths>
namespace {
bool saveLocalImage(const QImage &image, const QString &path) {
    auto safe = openframe::LocalPathPolicy::validateOutput(path);
    return !safe.isEmpty() && image.save(safe);
}
QImage syntheticFrame(int frame) {
    QImage image(960, 540, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient background(0, 0, 0, 540);
    background.setColorAt(0, QColor("#4a5262"));
    background.setColorAt(.68, QColor("#323b49"));
    background.setColorAt(1, QColor("#202832"));
    p.fillRect(image.rect(), background);
    p.setPen(QPen(QColor(120, 145, 163, 45), 1));
    for (int x = -400; x < 1500; x += 100)
        p.drawLine(QPointF(480, 295), QPointF(x, 540));
    for (int y = 330; y < 540; y += 40)
        p.drawLine(0, y, 960, y);
    const qreal phase = frame / 12.0;
    const qreal x = 350 + frame * 6;
    const qreal bob = std::sin(phase * 3.14159265) * 7;
    QColor accent("#d6a76a");
    p.setPen(QPen(QColor(0, 0, 0, 70), 16, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(x - 35, 448), QPointF(x + 65, 448));
    p.setPen(QPen(accent, 22, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(x, 242 + bob), QPointF(x + 8, 336 + bob));
    p.setBrush(accent);
    p.drawEllipse(QPointF(x - 3, 208 + bob), 26, 30);
    p.setPen(QPen(accent, 16, Qt::SolidLine, Qt::RoundCap));
    qreal stride = std::sin(phase * 3.14159265) * 45;
    p.drawLine(QPointF(x + 8, 334 + bob), QPointF(x - 20 + stride, 388));
    p.drawLine(QPointF(x - 20 + stride, 388), QPointF(x - 30 + stride, 441));
    p.drawLine(QPointF(x + 8, 334 + bob), QPointF(x + 31 - stride, 389));
    p.drawLine(QPointF(x + 31 - stride, 389), QPointF(x + 48 - stride, 441));
    p.drawLine(QPointF(x, 257 + bob), QPointF(x - 44 - stride * .5, 297));
    p.drawLine(QPointF(x - 44 - stride * .5, 297), QPointF(x - 55 - stride * .6, 345));
    p.drawLine(QPointF(x + 3, 257 + bob), QPointF(x + 42 + stride * .4, 294));
    p.drawLine(QPointF(x + 42 + stride * .4, 294), QPointF(x + 60 + stride * .7, 330));
    p.setPen(QColor("#d5dee8"));
    p.setFont(QFont("Segoe UI", 11));
    p.drawText(QPointF(26, 34), "OPENFRAME  /  SYNTHETIC WALK STUDY");
    p.setFont(QFont("Consolas", 11));
    p.drawText(QRectF(700, 16, 230, 28), Qt::AlignRight,
               QString("FRAME %1  ·  24 FPS").arg(frame + 1, 3, 10, QChar('0')));
    return image;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OpenFrame");
    QCoreApplication::setApplicationName("OpenFrame");
    QCoreApplication::setApplicationVersion("0.4.1");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCommandLineParser parser;
    parser.setApplicationDescription("Independent local animation review player");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"no-api", "Do not start the local integration server."});
    parser.addOption({"no-settings", "Use isolated temporary settings."});
    parser.addOption(
        {"quit-after", "Exit automatically after a number of milliseconds.", "milliseconds"});
    parser.addOption({"smoke-test", "Run synthetic UI checks and exit."});
    parser.addOption({"render-preview",
                      "Save synthetic UI captures and a JSON report to a directory.", "directory"});
    parser.addPositionalArgument("files", "Local media or OpenFrame project files.", "[files...]");
    parser.process(app);
    const bool smoke = parser.isSet("smoke-test") || parser.isSet("render-preview");
    const bool isolated = smoke || parser.isSet("no-settings");
    QString pathError;
    QString preferredTemp = QDir::tempPath();
    if (!isolated) {
        auto settingsRoot = openframe::createLocalDirectoryChain(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/config",
            &pathError);
        if (settingsRoot.isEmpty())
            return 2;
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot);
        QSettings preferences;
        preferredTemp = preferences.value("general/tempDirectory", preferredTemp).toString();
    }
    QString tempRoot = openframe::LocalPathPolicy::validateDirectory(preferredTemp, &pathError);
    if (tempRoot.isEmpty())
        return 2;
    QTemporaryDir scratch(QDir(tempRoot).filePath("OpenFrame-XXXXXX"));
    if (!scratch.isValid())
        return 2;
    if (isolated) {
        QCoreApplication::setApplicationName("OpenFrameSyntheticCheck");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    }
    openframe::applyDarkTheme(app);
    openframe::ReviewSession session;
    openframe::ApiServer server(&session);
    openframe::MainWindow window(&session, &server);
    window.setSmokeMode(smoke);
    window.show();
    if (!smoke && !parser.isSet("no-api")) {
        QSettings settings;
        if (settings.value("api/enabled", true).toBool())
            server.start(quint16(settings.value("api/port", 0).toUInt()));
    }
    if (smoke) {
        QString output = openframe::createLocalDirectoryChain(
            parser.isSet("render-preview") ? parser.value("render-preview")
                                           : QDir(scratch.path()).filePath("capture"),
            &pathError);
        if (output.isEmpty())
            return 2;
        auto report = std::make_shared<QJsonObject>();
        auto completed = std::make_shared<bool>(false);
        auto seeded = std::make_shared<bool>(false);
        auto finish = [&, output, report, completed](bool success, const QString &failure) {
            if (*completed)
                return;
            *completed = true;
            QRawFont font = QRawFont::fromFont(app.font());
            bool readableFont = !QFontDatabase::families().isEmpty() && font.isValid() &&
                                font.supportsCharacter(QChar('A')) &&
                                font.supportsCharacter(QChar('0'));
            report->insert("fontReadable", readableFont);
            report->insert("fontFamily", app.font().family());
            success = success && readableFont;
            report->insert("success", success);
            report->insert("failure",
                           !readableFont
                               ? QString("The platform font backend has no usable Latin font. "
                                         "Configure local fonts before reviewing captures.")
                               : failure);
            report->insert("visualParity",
                           "unverified; independently authored synthetic data only");
            report->insert("commands", window.commandActions().size());
            QJsonArray commands;
            auto actions = window.commandActions();
            std::sort(actions.begin(), actions.end(),
                      [](QAction *a, QAction *b) { return a->objectName() < b->objectName(); });
            for (auto action : actions) {
                QJsonArray shortcuts;
                for (const auto &key : action->shortcuts())
                    shortcuts.append(key.toString(QKeySequence::PortableText));
                commands.append(QJsonObject{{"id", action->objectName()},
                                            {"label", action->text()},
                                            {"shortcuts", shortcuts},
                                            {"checkable", action->isCheckable()},
                                            {"enabled", action->isEnabled()}});
            }
            report->insert("commandCatalog", commands);
            report->insert("sources", session.project().sources.size());
            report->insert("timelines", session.project().timelines.size());
            report->insert("annotations", session.project().annotations.size());
            report->insert("notes", session.project().notes.size());
            report->insert("bookmarks", session.project().bookmarks.size());
            auto reportPath =
                openframe::LocalPathPolicy::validateOutput(QDir(output).filePath("ui-report.json"));
            QFile file(reportPath);
            if (!reportPath.isEmpty() && file.open(QIODevice::WriteOnly))
                file.write(QJsonDocument(*report).toJson(QJsonDocument::Indented));
            QTimer::singleShot(0, &app, [&, success] { app.exit(success ? 0 : 3); });
        };
        QObject::connect(&session, &openframe::ReviewSession::error, &app,
                         [finish](const QString &error) { finish(false, error); });
        QObject::connect(
            &session, &openframe::ReviewSession::projectChanged, &app,
            [&, output, report, completed, seeded, finish] {
                if (*completed || *seeded || session.project().sources.isEmpty())
                    return;
                *seeded = true;
                session.mutateProject("Synthetic UI fixture", [](openframe::Project &p) {
                    p.name = "Walk study — review";
                    const auto id = p.activeTimelineId;
                    p.bookmarks.append(
                        {openframe::newId(), id, 4, 4, "Contact", {}, QColor("#ebbf67")});
                    p.bookmarks.append({openframe::newId(), id, 12, 20, "Passing pose",
                                        "Check the weight shift", QColor("#76b7d2")});
                    p.notes.append(
                        {openframe::newId(), id, 12,
                         "Push the hip over the support foot.\nKeep the leading shoulder relaxed.",
                         "Review"});
                    openframe::Annotation a;
                    a.id = openframe::newId();
                    a.timelineId = id;
                    a.frame = 12;
                    a.endFrame = 12;
                    a.tool = openframe::AnnotationTool::Arrow;
                    a.points = {{.69, .39}, {.49, .60}};
                    a.width = 6;
                    a.color = QColor("#edc66b");
                    p.annotations.append(a);
                    openframe::Annotation text = a;
                    text.id = openframe::newId();
                    text.tool = openframe::AnnotationTool::Text;
                    text.text = "WEIGHT SHIFT";
                    text.textPosition = {.68, .34};
                    text.fontSize = 28;
                    p.annotations.append(text);
                });
                session.seek(12, false);
                QTimer::singleShot(1200, &app, [&, output, report, completed, finish] {
                    if (*completed)
                        return;
                    bool checks = session.durationFrames() == 24;
                    report->insert("importSequence24Frames", checks);
                    auto find = [&](QString id) -> QAction * {
                        for (auto a : window.commandActions())
                            if (a->objectName() == id)
                                return a;
                        return nullptr;
                    };
                    if (auto step = find("stepForward")) {
                        auto before = session.currentFrame();
                        step->trigger();
                        checks = checks && session.currentFrame() == before + 1;
                    } else
                        checks = false;
                    report->insert("stepCommand", checks);
                    if (auto overlay = find("overlayGrid")) {
                        overlay->trigger();
                        checks = checks && session.project().viewer.overlay.type ==
                                               openframe::OverlayType::Grid;
                        find("overlayOff")->trigger();
                    } else
                        checks = false;
                    report->insert("overlayCommand", checks);
                    if (auto mode = find("grid")) {
                        mode->trigger();
                        checks =
                            checks && session.project().viewer.mode == openframe::ViewerMode::Grid;
                        find("single")->trigger();
                    } else
                        checks = false;
                    report->insert("gridCommand", checks);
                    session.seek(12, false);
                    QTimer::singleShot(700, &app, [&, output, report, checks, finish] {
                        QImage frame = session.displayImage();
                        if (frame.isNull()) {
                            finish(false, "Shared compositor did not produce a synthetic frame.");
                            return;
                        }
                        window.renderSyntheticPreview(frame);
                        QTimer::singleShot(120, &app, [&, output, report, checks, finish] {
                            bool captured =
                                saveLocalImage(window.grab().toImage(),
                                               QDir(output).filePath("loaded-annotations.png"));
                            report->insert("loadedCapture", captured);
                            report->insert("emptyCapture",
                                           QFileInfo::exists(QDir(output).filePath("empty.png")));
                            const auto dialogReport = window.captureSyntheticDialogs(output);
                            report->insert("dialogs", dialogReport);
                            const auto transientReport =
                                window.captureSyntheticTransientTools(output);
                            report->insert("transientTools", transientReport);
                            const auto drawingReport = window.captureSyntheticDrawingTools(output);
                            report->insert("drawingTools", drawingReport);
                            finish(checks && captured && dialogReport.value("success").toBool() &&
                                       transientReport.value("success").toBool() &&
                                       drawingReport.value("success").toBool(),
                                   checks ? QString()
                                          : QString("A synthetic UI command check failed."));
                        });
                    });
                });
            });
        QTimer::singleShot(200, &app, [&, output, report] {
            report->insert("emptyCapture", saveLocalImage(window.grab().toImage(),
                                                          QDir(output).filePath("empty.png")));
            QString directory =
                openframe::createLocalDirectoryChain(QDir(output).filePath("synthetic_frames"));
            if (directory.isEmpty()) {
                app.exit(2);
                return;
            }
            QStringList paths;
            for (int f = 0; f < 24; ++f) {
                QString path =
                    QDir(directory).filePath(QString("walk_%1.png").arg(f, 4, 10, QChar('0')));
                if (!saveLocalImage(syntheticFrame(f), path)) {
                    app.exit(2);
                    return;
                }
                paths << path;
            }
            session.importImageSequence(paths, {24, 1});
        });
        QTimer::singleShot(30000, &app,
                           [finish] { finish(false, "Synthetic UI check timed out."); });
    } else {
        QStringList files = parser.positionalArguments();
        if (!files.isEmpty())
            QTimer::singleShot(0, &session, [&session, files] {
                if (files.size() == 1 && (files.first().endsWith(".oframe", Qt::CaseInsensitive) ||
                                          files.first().endsWith(".json", Qt::CaseInsensitive)))
                    session.openProject(files.first());
                else
                    session.importMedia(files);
            });
    }
    if (parser.isSet("quit-after")) {
        bool ok = false;
        int milliseconds = parser.value("quit-after").toInt(&ok);
        if (ok && milliseconds > 0)
            QTimer::singleShot(milliseconds, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
