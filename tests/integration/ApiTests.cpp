#include "core/ReviewSession.h"
#include "integration/ApiServer.h"
#include "integration/LocalPathPolicy.h"
#include "media/MediaDecoder.h"
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

using namespace openframe;
namespace {
int passed = 0;
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
    ++passed;
}
bool until(const std::function<bool()> &condition, int ms = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}
void connectLocal(QTcpSocket &socket, quint16 port) {
    socket.setProxy(QNetworkProxy::NoProxy);
    socket.connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), port);
    check(until([&] { return socket.state() == QAbstractSocket::ConnectedState; }),
          "Cannot connect to local API");
}
QJsonObject readReply(QTcpSocket &socket) {
    check(until([&] { return socket.canReadLine(); }), "API reply timeout");
    const auto doc = QJsonDocument::fromJson(socket.readLine());
    check(doc.isObject(), "API reply is not an object");
    return doc.object();
}
QJsonObject call(QTcpSocket &socket, const QString &token, const QString &method,
                 QJsonObject params = {}) {
    static int id = 0;
    const QJsonObject request{{"jsonrpc", "2.0"}, {"api", 1},         {"id", ++id},
                              {"token", token},   {"method", method}, {"params", params}};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    const auto reply = readReply(socket);
    check(reply.value("id").toInt() == id, "Request/reply ID mismatch");
    return reply;
}
QJsonValue success(QTcpSocket &socket, const QString &token, const QString &method,
                   QJsonObject params = {}) {
    const auto reply = call(socket, token, method, params);
    if (reply.contains("error"))
        throw std::runtime_error(
            (method + ": " + reply["error"].toObject()["message"].toString()).toStdString());
    check(reply.contains("result"), "Missing result");
    return reply["result"];
}
void rejected(QTcpSocket &socket, const QString &token, const QString &method, QJsonObject params,
              int code) {
    const auto reply = call(socket, token, method, params);
    check(reply["error"].toObject()["code"].toInt() == code, "Unexpected error code");
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("OpenFrameIntegrationTests");
    try {
        QTemporaryDir fixtures;
        check(fixtures.isValid(), "Cannot create synthetic fixture directory");
        QString error;
        for (const auto &path :
             {"https://example.invalid/a.mp4", "http:video", "file:///C:/movie.mov",
              "\\\\server\\share\\video.mov", "//server/share/video.mov", "\\\\?\\C:\\video.mov",
              "C:relative.mov", "C:/a.mov:secret", "../escape.mov", "a\n.mov"})
            check(LocalPathPolicy::validateInput(QString::fromUtf8(path), &error).isEmpty(),
                  "Remote or ambiguous input accepted");
        for (const auto &address :
             {"localhost", "127.0.0.2", "::1", "::ffff:127.0.0.1", "8.8.8.8", "127.1"})
            check(!LocalPathPolicy::isAllowedAddress(QString::fromLatin1(address)),
                  "Forbidden address accepted");
        check(LocalPathPolicy::isAllowedAddress("127.0.0.1"), "Loopback rejected");
        const QString output = fixtures.filePath("output.oframe");
        check(!LocalPathPolicy::validateOutput(output, &error).isEmpty(), "Local output rejected");
        check(!LocalPathPolicy::validateDirectoryOutput(fixtures.filePath("new-directory"), &error)
                   .isEmpty(),
              "New local directory rejected");
        check(LocalPathPolicy::validateOutput(fixtures.filePath("absent/out.oframe"), &error)
                  .isEmpty(),
              "Missing parent accepted");
#ifdef Q_OS_WIN
        const QString link = fixtures.filePath("remote-link");
        const bool madeLink =
            CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),
                                L"\\\\192.0.2.1\\never-access", SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2);
        if (madeLink) {
            check(LocalPathPolicy::validateInput(link + "/never.mov", &error).isEmpty(),
                  "Remote reparse point accepted");
            RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));
        } else
            std::printf("SKIP symlink creation: Windows privilege unavailable\n");
#endif
        ReviewSession session;
        session.setAudioEnabled(false);
        ApiServer server(&session);
        check(server.start(), "API server startup failed");
        check(server.isListening() && server.listeningPort() != 0, "API is not listening");
        const QString endpointPath = server.endpointPath();
        QFile endpoint(endpointPath);
        check(endpoint.open(QIODevice::ReadOnly), "Cannot read own synthetic endpoint");
        const auto endpointData = QJsonDocument::fromJson(endpoint.readAll()).object();
        endpoint.close();
        check(endpointData.value("host") == QJsonValue("127.0.0.1"),
              "API endpoint is not exact loopback");
        const auto token = endpointData["token"].toString();
        check(token.size() == 64, "Token must contain 256 random bits");
        QTcpSocket unauthenticated;
        connectLocal(unauthenticated, server.listeningPort());
        rejected(unauthenticated, QString(64, '0'), "project.new", {}, -32001);
        check(until([&] { return unauthenticated.state() == QAbstractSocket::UnconnectedState; }),
              "Unauthorized socket was not closed");
        QTcpSocket socket;
        connectLocal(socket, server.listeningPort());
        const auto status = success(socket, token, "status").toObject();
        check(status["api"].toInt() == 1, "API version mismatch");
        bool activated = false;
        QObject::connect(&server, &ApiServer::activateRequested, &app, [&] { activated = true; });
        success(socket, token, "window.activate");
        check(activated, "Window activation signal did not fire");
        rejected(socket, token, "exec", {{"code", "print('not executed')"}}, -32601);
        rejected(socket, token, "seek", {{"frame", true}}, -32602);
        rejected(socket, token, "seek", {{"frame", 1.5}}, -32602);
        rejected(socket, token, "seek", {{"frame", 1}, {"unknown", true}}, -32602);
        rejected(socket, token, "project.open", {{"path", "https://example.invalid/project"}},
                 -32602);
        rejected(socket, token, "export.start", {{"path", "\\\\server\\share\\out.mp4"}}, -32602);
        const QJsonObject invalidExportBase{{"path", fixtures.filePath("invalid-fps.mp4")}};
        for (const auto &fps :
             {QJsonObject{{"fpsNum", 24}}, QJsonObject{{"fpsDen", 1}},
              QJsonObject{{"fpsNum", 0}, {"fpsDen", 1}}, QJsonObject{{"fpsNum", 24}, {"fpsDen", 0}},
              QJsonObject{{"fpsNum", -24}, {"fpsDen", 1}},
              QJsonObject{{"fpsNum", 24}, {"fpsDen", -1}},
              QJsonObject{{"fpsNum", true}, {"fpsDen", 1}},
              QJsonObject{{"fpsNum", 24}, {"fpsDen", true}},
              QJsonObject{{"fpsNum", 23.976}, {"fpsDen", 1}},
              QJsonObject{{"fpsNum", 24}, {"fpsDen", 1.5}},
              QJsonObject{{"fpsNum", "24"}, {"fpsDen", 1}},
              QJsonObject{{"fpsNum", 24}, {"fpsDen", QJsonValue::Null}},
              QJsonObject{{"fpsNum", 1000001}, {"fpsDen", 1001}},
              QJsonObject{{"fpsNum", 24000}, {"fpsDen", 1000001}},
              QJsonObject{{"fpsNum", 1001}, {"fpsDen", 1}},
              QJsonObject{{"fpsNum", 1}, {"fpsDen", 1001}}}) {
            auto params = invalidExportBase;
            for (auto it = fps.begin(); it != fps.end(); ++it)
                params.insert(it.key(), it.value());
            rejected(socket, token, "export.start", params, -32602);
            check(!session.exporting(), "Invalid frame rate started an export");
        }
        rejected(socket, token, "media.import",
                 {{"paths", QJsonArray{"https://example.invalid/video"}}}, -32602);
        const auto folder =
            success(socket, token, "folder.add", {{"name", "Synthetic Folder"}}).toObject();
        const auto timeline = success(socket, token, "timeline.add",
                                      {{"name", "Synthetic Timeline"}, {"parentId", folder["id"]}})
                                  .toObject();
        const auto tid = timeline["id"].toString();
        success(socket, token, "timeline.activate", {{"id", tid}});
        success(socket, token, "project.defaultTimeline", {{"id", tid}});
        rejected(socket, token, "object.update", {{"id", folder["id"]}, {"parentId", folder["id"]}},
                 -32602);
        QJsonArray images;
        for (int i = 0; i < 12; ++i) {
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(QColor(i * 20, 40, 180));
            const auto path = fixtures.filePath(QStringLiteral("synthetic-%1.bmp").arg(i));
            check(image.save(path, "BMP"), "Cannot create synthetic frame");
            images.append(path);
        }
        success(socket, token, "media.sequence",
                {{"paths", images}, {"fpsNum", 24}, {"fpsDen", 1}});
        check(until([&] { return !session.busy(); }, 15000),
              "Synthetic sequence import did not complete");
        check(session.project().sources.size() == 1, "Synthetic sequence import failed");
        const auto sid = session.project().sources.first().id;
        check(session.project().timeline(tid)->clips.size() == 1,
              "Imported sequence was not appended");
        const auto exportVideo = [&](const QString &name, const QJsonObject &fps) {
            const auto path = fixtures.filePath(name);
            QJsonObject params{{"path", path},
                               {"kind", "video"},
                               {"timelineId", tid},
                               {"first", 0},
                               {"last", 11},
                               {"width", 64},
                               {"height", 48},
                               {"viewerComposition", false},
                               {"includeAnnotations", false},
                               {"includeAudio", false},
                               {"videoCodec", "mpeg4"}};
            for (auto it = fps.begin(); it != fps.end(); ++it)
                params.insert(it.key(), it.value());
            bool finished = false;
            bool exported = false;
            QString exportMessage;
            QObject observer;
            QObject::connect(&session, &ReviewSession::exportFinished, &observer,
                             [&](bool ok, const QString &message) {
                                 exported = ok;
                                 exportMessage = message;
                                 finished = true;
                             });
            const auto job = success(socket, token, "export.start", params).toObject();
            check(!job["jobId"].toString().isEmpty(), "Export did not return a job ID");
            check(until([&] { return finished; }, 15000), "Output frame-rate export timed out");
            check(exported, qPrintable("Output frame-rate export failed: " + exportMessage));
            check(success(socket, token, "export.status").toObject()["state"] ==
                      QJsonValue("complete"),
                  "Export status did not report completion");
            return probeMedia(path).info;
        };
        const auto defaultFps = exportVideo("owner-fps.mp4", {});
        check(defaultFps.fps == Rational{24, 1} && defaultFps.frameCount() == 12,
              "Omitted frame rate did not preserve owner FPS");
        const auto halfFps = exportVideo("half-fps.mp4", {{"fpsNum", 12}, {"fpsDen", 1}});
        check(halfFps.fps == Rational{12, 1} && halfFps.frameCount() == 6 &&
                  halfFps.durationUs == 500000,
              "Explicit output frame rate was not passed to the video exporter");
        const auto rationalFps =
            exportVideo("rational-fps.mp4", {{"fpsNum", 30000}, {"fpsDen", 1001}});
        check(rationalFps.fps == Rational{30000, 1001} && rationalFps.frameCount() == 15,
              "Rational output frame rate lost its denominator");
        check(session.project().timeline(tid)->fps == Rational{24, 1},
              "Export changed the owner's frame rate");
        QTcpSocket features;
        connectLocal(features, server.listeningPort());
        success(features, token, "range.set", {{"first", 2}, {"last", 9}});
        success(features, token, "range.set", {{"loopDelayMs", 250}, {"loopHoldEmpty", true}});
        auto featureStatus = success(features, token, "status").toObject();
        check(featureStatus["rangeIn"].toInt() == 2 && featureStatus["rangeOut"].toInt() == 9 &&
                  featureStatus["loopDelayMs"].toInt() == 250 &&
                  featureStatus["loopHoldEmpty"].toBool() &&
                  !featureStatus["loopDelayActive"].toBool(),
              "Loop delay settings changed the range or failed to reach core");
        rejected(features, token, "range.set", {{"first", 5}, {"loopDelayMs", -1}}, -32602);
        rejected(features, token, "range.set", {{"loopDelayMs", 600001}}, -32602);
        rejected(features, token, "range.set", {{"loopDelayMs", 1.5}}, -32602);
        rejected(features, token, "range.set", {{"loopHoldEmpty", 1}}, -32602);
        check(session.project().rangeIn == 2 && session.project().loopDelayMs == 250,
              "Rejected loop settings partially changed the project");
        success(features, token, "viewer.set",
                {{"pane", 1},
                 {"zoom", 2.5},
                 {"panX", -.25},
                 {"panY", .125},
                 {"panZoomEnabled", false}});
        auto panZoom = success(features, token, "viewer.settings").toObject()["panZoom"].toObject();
        check(panZoom["zoom"].toDouble() == 2.5 && panZoom["panX"].toDouble() == -.25 &&
                  panZoom["panY"].toDouble() == .125 && !panZoom["enabled"].toBool(),
              "Global pan/zoom API did not update the composition");
        rejected(features, token, "viewer.set", {{"zoom", 64.1}}, -32602);
        rejected(features, token, "viewer.set", {{"panX", 101}}, -32602);
        rejected(features, token, "viewer.set", {{"panZoomEnabled", 1}}, -32602);
        success(features, token, "viewer.assign", {{"pane", 0}, {"id", sid}});
        success(features, token, "viewer.set", {{"zoom", 3}, {"panZoomEnabled", true}});
        success(features, token, "viewer.assign", {{"pane", 0}, {"id", tid}});
        check(session.project().viewer.panZoom.zoom == 2.5,
              "Changing the primary owner did not restore its global pan/zoom");
        const auto rectangle =
            QJsonObject{{"id", "synthetic-box"},
                        {"type", "rectangle"},
                        {"points", QJsonArray{QJsonArray{.1, .2}, QJsonArray{.8, .7}}},
                        {"color", "#ff123456"},
                        {"fill", "#20112233"},
                        {"width", 3},
                        {"opacity", .5}};
        auto overlay = success(features, token, "overlays.set",
                               {{"type", "custom"}, {"shapes", QJsonArray{rectangle}}})
                           .toObject();
        check(overlay["type"] == QJsonValue("custom") && overlay["shapes"].toArray().size() == 1 &&
                  overlay["shapes"].toArray()[0].toObject()["fill"] == QJsonValue("#20112233"),
              "Custom overlay did not preserve its shape and alpha");
        const auto originalOverlay = success(features, token, "overlays.get").toObject();
        for (const auto &invalidOverlay :
             {QJsonObject{{"type", "unknown"}}, QJsonObject{{"color", "not-a-color"}},
              QJsonObject{{"gridColumns", 1.5}}, QJsonObject{{"safeMargins", QJsonArray{.5, .1}}},
              QJsonObject{{"imagePath", "https://example.invalid/overlay.png"}},
              QJsonObject{{"imagePath", "\\\\server\\share\\overlay.png"}},
              QJsonObject{{"imageRect", QJsonArray{0, 0, 0, 1}}},
              QJsonObject{{"imageOpacity", true}}, QJsonObject{{"shapes", QJsonArray{true}}},
              QJsonObject{{"shapes", QJsonArray{rectangle, rectangle}}},
              QJsonObject{
                  {"shapes", QJsonArray{QJsonObject{{"type", "line"},
                                                    {"points", QJsonArray{QJsonArray{0, 0}}}}}}},
              QJsonObject{
                  {"shapes", QJsonArray{QJsonObject{{"type", "text"},
                                                    {"points", QJsonArray{QJsonArray{-1, 0}}}}}}},
              QJsonObject{{"unknown", true}}})
            rejected(features, token, "overlays.set", invalidOverlay, -32602);
        check(success(features, token, "overlays.get").toObject() == originalOverlay,
              "Rejected overlay settings changed the model");
        overlay = success(features, token, "overlays.set",
                          {{"type", "image"},
                           {"imagePath", images[0]},
                           {"imageRect", QJsonArray{.1, .1, .7, .8}},
                           {"imageKeepAspect", false}})
                      .toObject();
        check(overlay["type"] == QJsonValue("image") &&
                  !overlay["imagePath"].toString().isEmpty() &&
                  !overlay["imageKeepAspect"].toBool(),
              "Local image overlay was not accepted");
        success(features, token, "overlays.set", {{"type", "custom"}});
        auto tracks = success(features, token, "media.audioTracks", {{"id", sid}}).toObject();
        check(tracks["audioTracks"].toArray().isEmpty() &&
                  tracks["selectedAudioTrack"].toInt() == -1,
              "Silent sequence audio-track state is incorrect");
        rejected(features, token, "media.selectAudioTrack", {{"id", sid}, {"streamIndex", 0}},
                 -32602);
        rejected(features, token, "media.selectAudioTrack", {{"id", sid}, {"streamIndex", true}},
                 -32602);
        rejected(features, token, "media.selectAudioTrack", {{"id", sid}}, -32602);
        success(features, token, "media.selectAudioTrack", {{"id", sid}, {"streamIndex", -1}});
        success(socket, token, "seek", {{"frame", 7}, {"audio", false}});
        check(session.currentFrame() == 7, "Seek did not reach frame seven");
        success(socket, token, "range.set", {{"first", 2}, {"last", 9}, {"loop", "pingpong"}});
        check(session.project().rangeIn == 2 && session.project().rangeOut == 9, "Range mismatch");
        success(socket, token, "viewer.assign", {{"pane", 1}, {"id", sid}});
        success(socket, token, "viewer.set",
                {{"mode", "split"}, {"splitVertical", false}, {"zoom", 2.0}});
        check(session.project().viewer.mode == ViewerMode::Split &&
                  !session.project().viewer.splitVertical,
              "Viewer configuration mismatch");
        check(success(socket, token, "bookmarks.add",
                      {{"ownerId", tid}, {"frames", QJsonArray{2, 4, 4}}, {"name", "Markers"}})
                      .toInt() == 2,
              "Bookmarks did not deduplicate");
        check(
            success(socket, token, "notes.add",
                    {{"ownerId", tid},
                     {"notes", QJsonArray{QJsonObject{{"frame", 4}, {"text", "Synthetic note"}}}}})
                    .toInt() == 1,
            "Note was not created");
        success(socket, token, "timecode.set", {{"id", tid}, {"offset", 101}, {"enabled", true}});
        check(success(socket, token, "timecode.get", {{"id", tid}}).toObject()["offset"].toInt() ==
                  101,
              "Timecode offset mismatch");
        success(socket, token, "project.save", {{"path", output}});
        success(socket, token, "project.new");
        success(socket, token, "project.open", {{"path", output}});
        check(session.project().bookmarks.size() == 2 && session.project().notes.size() == 1,
              "Project persistence lost API changes");
        check(session.project().timeline(tid)->timecodeOffset == 101,
              "Project persistence lost timecode offset");
        check(session.project().loopDelayMs == 250 && session.project().loopHoldEmpty &&
                  session.project().viewer.overlay.type == OverlayType::Custom &&
                  session.project().viewer.overlay.shapes.size() == 1,
              "Project persistence lost new loop/overlay API settings");
        rejected(socket, token, "notes.add",
                 {{"ownerId", tid},
                  {"notes", QJsonArray{QJsonObject{
                                {"frame", 5}, {"text", "bad color"}, {"color", "not-a-color"}}}}},
                 -32602);
        success(socket, token, "notes.add",
                {{"ownerId", tid},
                 {"notes", QJsonArray{QJsonObject{
                               {"frame", 5}, {"text", "colored note"}, {"color", "#ff123456"}}}}});
        auto coloredNotes = success(socket, token, "notes.list", {{"ownerId", tid}}).toArray();
        check(coloredNotes.last().toObject()["color"] == QJsonValue("#ff123456"),
              "Note color was not preserved");
        auto hostileProject = projectToJson(session.project());
        auto hostileSources = hostileProject["sources"].toArray();
        auto hostileSource = hostileSources[0].toObject();
        hostileSource["path"] = "https://example.invalid/video.mov";
        hostileSources[0] = hostileSource;
        hostileProject["sources"] = hostileSources;
        const auto hostilePath = fixtures.filePath("nested-url.oframe");
        QFile hostileFile(hostilePath);
        check(hostileFile.open(QIODevice::WriteOnly), "Cannot create synthetic hostile project");
        hostileFile.write(QJsonDocument(hostileProject).toJson());
        hostileFile.close();
        rejected(socket, token, "project.open", {{"path", hostilePath}}, -32010);
        success(socket, token, "maya.register",
                {{"sessionId", "maya-test-a"},
                 {"name", "Synthetic Maya A"},
                 {"pid", 1},
                 {"direction", "bidirectional"}});
        QTcpSocket second;
        connectLocal(second, server.listeningPort());
        success(second, token, "maya.register",
                {{"sessionId", "maya-test-b"}, {"name", "Synthetic Maya B"}, {"pid", 2}});
        check(server.mayaSessions().size() == 2, "Multiple Maya sessions were not registered");
        auto compound =
            success(socket, token, "maya.sync", {{"sessionId", "maya-test-a"}, {"mayaFrame", 4}})
                .toObject();
        check(compound["syncFrameApplied"].toBool() && compound["frame"].toInt() == 4,
              "Compound Maya synchronization failed");
        check(server.configureMayaSession(
                  "maya-test-a", {{"offset", 2}, {"multiplier", 0.5}, {"fromRangeStart", true}}),
              "Cannot configure compound mapping");
        compound =
            success(socket, token, "maya.sync", {{"sessionId", "maya-test-a"}, {"mayaFrame", 5}})
                .toObject();
        check(compound["frame"].toInt() == 6,
              "Server did not apply current Maya mapping and range atomically");
        check(server.configureMayaSession("maya-test-a", {{"direction", "off"}}),
              "Cannot turn compound sync off");
        compound =
            success(socket, token, "maya.sync", {{"sessionId", "maya-test-a"}, {"mayaFrame", 9}})
                .toObject();
        check(!compound["syncFrameApplied"].toBool() && compound["frame"].toInt() == 6,
              "Stale client update bypassed disabled sync");
        rejected(second, token, "maya.sync", {{"sessionId", "maya-test-a"}, {"mayaFrame", 8}},
                 -32602);
        success(socket, token, "seek", {{"frame", 3}, {"origin", "maya-test-a"}});
        auto sync = success(second, token, "status", {{"sessionId", "maya-test-b"}}).toObject();
        check(sync["frameOrigin"] == QJsonValue("maya-test-a") && sync["frame"].toInt() == 3,
              "Maya origin tracking failed");
        rejected(second, token, "seek", {{"frame", 4}, {"origin", "maya-test-a"}}, -32602);
        check(server.configureMayaSession("maya-test-b", {{"offset", 12}, {"multiplier", 0.5}}),
              "Maya config update failed");
        sync = success(second, token, "status", {{"sessionId", "maya-test-b"}}).toObject();
        check(sync["maya"].toObject()["offset"].toInt() == 12, "Maya config did not propagate");
        success(socket, token, "maya.unregister");
        check(server.mayaSessions().size() == 1, "Maya unregistration failed");
        const auto audioPath = fixtures.filePath("synthetic-audio-track.wav");
        QFile audioFile(audioPath);
        check(audioFile.open(QIODevice::WriteOnly), "Cannot create synthetic audio-track fixture");
        QDataStream wave(&audioFile);
        wave.setByteOrder(QDataStream::LittleEndian);
        constexpr int samples = 4800;
        wave.writeRawData("RIFF", 4);
        wave << quint32(36 + samples * 2);
        wave.writeRawData("WAVEfmt ", 8);
        wave << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000)
             << quint16(2) << quint16(16);
        wave.writeRawData("data", 4);
        wave << quint32(samples * 2);
        for (int i = 0; i < samples; ++i)
            wave << qint16(2000 * std::sin(i * 6.283185307179586 * 440 / 48000));
        audioFile.close();
        success(features, token, "media.import", {{"paths", QJsonArray{audioPath}}});
        check(until([&] { return !session.busy(); }, 15000),
              "Synthetic audio-track import timed out");
        const auto audioId = session.project().sources.last().id;
        tracks = success(features, token, "media.audioTracks", {{"id", audioId}}).toObject();
        const auto audioTracks = tracks["audioTracks"].toArray();
        check(audioTracks.size() == 1 &&
                  audioTracks[0].toObject()["codec"] == QJsonValue("pcm_s16le") &&
                  audioTracks[0].toObject()["sampleRate"].toInt() == 48000,
              "Actual audio stream metadata was not exposed");
        const int streamIndex = audioTracks[0].toObject()["streamIndex"].toInt(-1);
        auto chosen = success(features, token, "media.selectAudioTrack",
                              {{"id", audioId}, {"streamIndex", streamIndex}})
                          .toObject();
        check(chosen["selectedAudioTrack"].toInt(-1) == streamIndex,
              "Actual audio stream selection did not reach core");
        rejected(features, token, "media.selectAudioTrack",
                 {{"id", audioId}, {"streamIndex", streamIndex + 1}}, -32602);
        success(features, token, "project.save",
                {{"path", fixtures.filePath("audio-track.oframe")}});
        success(features, token, "project.open",
                {{"path", fixtures.filePath("audio-track.oframe")}});
        check(session.project().source(audioId)->selectedAudioTrack == streamIndex,
              "Audio track selection did not survive project persistence");
        success(features, token, "media.selectAudioTrack", {{"id", audioId}, {"streamIndex", -1}});
        QTcpSocket waveTwo;
        connectLocal(waveTwo, server.listeningPort());
        success(waveTwo, token, "media.setAudioOffset", {{"id", audioId}, {"offsetUs", 1250000}});
        check(session.project().source(audioId)->audioOffsetUs == 1250000,
              "Source audio offset did not reach the model");
        success(waveTwo, token, "history.undo");
        check(session.project().source(audioId)->audioOffsetUs == 0,
              "Source audio offset was not undoable");
        success(waveTwo, token, "history.redo");
        const QJsonArray invalidOffsets{true,
                                        1.5,
                                        QJsonValue::Null,
                                        "+1",
                                        "-0",
                                        "01",
                                        " 1",
                                        "1e3",
                                        "9007199254740992",
                                        "-9007199254740992",
                                        "9223372036854775808",
                                        9007199254740992.0};
        for (const auto &offset : invalidOffsets)
            rejected(waveTwo, token, "media.setAudioOffset",
                     {{"id", audioId}, {"offsetUs", offset}}, -32602);
        rejected(waveTwo, token, "media.setAudioOffset", {{"id", audioId}}, -32602);
        rejected(waveTwo, token, "media.setAudioOffset", {{"id", "missing"}, {"offsetUs", 0}},
                 -32602);
        check(session.project().source(audioId)->audioOffsetUs == 1250000,
              "Invalid offset request changed the model");
        auto offsetObject =
            success(waveTwo, token, "media.setAudioOffset",
                    {{"id", audioId}, {"offsetUs", QString::number(MaximumAudioOffsetUs)}})
                .toObject();
        check(offsetObject["audioOffsetUs"].toInteger() == MaximumAudioOffsetUs,
              "Exact maximum source offset was rounded");
        success(waveTwo, token, "project.save", {{"path", fixtures.filePath("offset.oframe")}});
        success(waveTwo, token, "project.open", {{"path", fixtures.filePath("offset.oframe")}});
        check(session.project().source(audioId)->audioOffsetUs == MaximumAudioOffsetUs,
              "Source audio offset did not survive persistence exactly");
        success(waveTwo, token, "media.setAudioOffset",
                {{"id", audioId}, {"offsetUs", QString::number(-MaximumAudioOffsetUs)}});
        check(session.project().source(audioId)->audioOffsetUs == -MaximumAudioOffsetUs,
              "Exact minimum source offset was rounded");
        success(waveTwo, token, "media.setAudioOffset", {{"id", audioId}, {"offsetUs", 0}});
        success(waveTwo, token, "audio.timeStretch", {{"enabled", true}});
        check(success(waveTwo, token, "audio.timeStretch").toObject()["enabled"].toBool() &&
                  success(waveTwo, token, "status").toObject()["audioTimeStretching"].toBool(),
              "Audio time-stretch setting was not exposed");
        rejected(waveTwo, token, "audio.timeStretch", {{"enabled", 1}}, -32602);
        rejected(waveTwo, token, "audio.timeStretch", {{"enabled", QJsonValue::Null}}, -32602);
        rejected(waveTwo, token, "audio.timeStretch", {{"enabled", false}, {"unknown", 0}}, -32602);
        rejected(waveTwo, token, "play", {{"rate", 0.01}, {"audioTimeStretching", false}}, -32602);
        rejected(waveTwo, token, "play", {{"rate", -0.01}}, -32602);
        rejected(waveTwo, token, "play", {{"rate", 0}}, -32602);
        rejected(waveTwo, token, "play", {{"rate", 17}}, -32602);
        rejected(waveTwo, token, "play", {{"rate", 1}, {"audioTimeStretching", "yes"}}, -32602);
        check(session.audioTimeStretchingEnabled(),
              "Rejected playback request changed time stretching");
        success(waveTwo, token, "play",
                {{"playing", false}, {"rate", 0.0625}, {"audioTimeStretching", false}});
        check(session.playbackRate() == 0.0625 && !session.audioTimeStretchingEnabled(),
              "Lower speed bound or playback time-stretch option failed");
        success(waveTwo, token, "play", {{"playing", false}, {"rate", -16}});
        check(session.playbackRate() == -16, "Reverse speed bound failed");
        success(waveTwo, token, "play",
                {{"playing", false}, {"rate", 1}, {"audioTimeStretching", true}});

        const auto sequenceDirectory = fixtures.filePath("descriptor-sequence");
        check(QDir().mkdir(sequenceDirectory), "Cannot create descriptor fixture directory");
        for (int i = 0; i < 300; ++i) {
            QImage frame(16, 12, QImage::Format_RGB32);
            frame.fill(QColor(i % 256, 83, 170));
            check(
                frame.save(QDir(sequenceDirectory)
                               .filePath(QStringLiteral("frame.%1.bmp").arg(i, 4, 10, QChar('0'))),
                           "BMP"),
                "Cannot generate descriptor fixture frame");
        }
        const QJsonObject descriptor{{"directory", sequenceDirectory},
                                     {"baseName", "frame"},
                                     {"first", 0},
                                     {"last", 299},
                                     {"padding", 4},
                                     {"extension", "bmp"},
                                     {"fpsNum", 30000},
                                     {"fpsDen", 1001}};
        const auto sourceCountBefore = session.project().sources.size();
        auto accepted = success(waveTwo, token, "media.sequenceDescriptor", descriptor).toObject();
        check(accepted["accepted"].toBool() && accepted["frameCount"].toInt() == 300,
              "Long sequence descriptor was not accepted");
        check(until([&] { return !session.busy(); }, 15000), "Descriptor import timed out");
        check(session.project().sources.size() == sourceCountBefore + 1,
              "Descriptor did not import exactly one source");
        const auto &longSequence = session.project().sources.last();
        check(longSequence.sequencePaths.size() == 300 && longSequence.info.frameCount() == 300 &&
                  longSequence.info.fps.num == 30000 && longSequence.info.fps.den == 1001,
              "Descriptor order, frame count or rational frame rate changed");
        MediaDecoder sequenceDecoder(longSequence);
        check(sequenceDecoder.frame(0).pixelColor(0, 0) == QColor(0, 83, 170) &&
                  sequenceDecoder.frame(299).pixelColor(0, 0) == QColor(43, 83, 170),
              "Descriptor first/last frames do not match the generated sequence");
        const auto longSequenceId = longSequence.id;
        const auto compactSource =
            success(waveTwo, token, "object.get", {{"id", longSequenceId}}).toObject();
        check(!compactSource.contains("sequencePaths") && !compactSource.contains("paths") &&
                  QJsonDocument(compactSource).toJson(QJsonDocument::Compact).size() < 2048,
              "Source metadata expanded the entire sequence path list");
        const auto compactSources =
            success(waveTwo, token, "objects.list", {{"type", "source"}}).toArray();
        check(QJsonDocument(compactSources).toJson(QJsonDocument::Compact).size() < 8192,
              "Object listing expanded sequence paths");
        success(waveTwo, token, "project.save",
                {{"path", fixtures.filePath("long-sequence.oframe")}});
        success(waveTwo, token, "project.open",
                {{"path", fixtures.filePath("long-sequence.oframe")}});
        check(session.project().source(longSequenceId) &&
                  session.project().source(longSequenceId)->sequencePaths.size() == 300,
              "Project save omitted the full sequence path list");
        const QVector<QJsonObject> invalidDescriptors{
            {{"first", true}},
            {{"first", 0.5}},
            {{"last", -1}},
            {{"last", 100000}},
            {{"padding", 0}},
            {{"padding", 13}},
            {{"baseName", "../frame"}},
            {{"baseName", "frame/child"}},
            {{"baseName", "frame*"}},
            {{"baseName", "frame."}},
            {{"extension", "svg"}},
            {{"extension", "../bmp"}},
            {{"fpsNum", 0}},
            {{"fpsNum", 1000001}},
            {{"fpsNum", 1000000}, {"fpsDen", 1}},
            {{"fpsNum", 1}, {"fpsDen", 1000000}},
            {{"directory", "https://example.invalid/sequence"}},
            {{"directory", "\\\\server\\share"}},
            {{"directory", sequenceDirectory + "/../descriptor-sequence"}},
            {{"unknown", true}}};
        for (const auto &patch : invalidDescriptors) {
            auto invalidDescriptor = descriptor;
            for (auto it = patch.begin(); it != patch.end(); ++it)
                invalidDescriptor[it.key()] = it.value();
            rejected(waveTwo, token, "media.sequenceDescriptor", invalidDescriptor, -32602);
        }
        auto incompleteDescriptor = descriptor;
        incompleteDescriptor.remove("last");
        rejected(waveTwo, token, "media.sequenceDescriptor", incompleteDescriptor, -32602);
        const auto lastImage = QDir(sequenceDirectory).filePath("frame.0299.bmp");
        check(QFile::rename(lastImage, lastImage + ".held"),
              "Cannot hide own synthetic final frame");
        success(waveTwo, token, "media.sequenceDescriptor", descriptor);
        check(until([&] { return !session.busy(); }, 15000), "Missing-frame validation timed out");
        check(session.project().sources.size() == sourceCountBefore + 1 &&
                  !success(waveTwo, token, "status").toObject()["lastError"].toString().isEmpty(),
              "Missing descriptor frame created a partial source or lost the error");
        check(QFile::rename(lastImage + ".held", lastImage),
              "Cannot restore own synthetic final frame");
#ifdef Q_OS_WIN
        const auto sequenceLink = QDir(sequenceDirectory).filePath("trap.0000.bmp");
        if (CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(sequenceLink.utf16()),
                                L"\\\\192.0.2.1\\never-access", 0x2)) {
            auto trap = descriptor;
            trap["baseName"] = "trap";
            trap["last"] = 0;
            success(waveTwo, token, "media.sequenceDescriptor", trap);
            check(until([&] { return !session.busy(); }, 15000),
                  "Reparse-frame validation timed out");
            check(
                session.project().sources.size() == sourceCountBefore + 1 &&
                    !success(waveTwo, token, "status").toObject()["lastError"].toString().isEmpty(),
                "Reparse descriptor frame was accepted");
            check(DeleteFileW(reinterpret_cast<LPCWSTR>(sequenceLink.utf16())),
                  "Cannot remove own synthetic link");
        } else
            std::printf("SKIP descriptor symlink creation: Windows privilege unavailable\n");
#endif
        const auto fooDirectory = fixtures.filePath("foo");
        const auto foobarDirectory = fixtures.filePath("foobar");
        check(QDir().mkdir(fooDirectory) && QDir().mkdir(foobarDirectory),
              "Cannot create reference-check fixture directories");
        const auto referencedFirst = QDir(foobarDirectory).filePath("first.bmp");
        const auto referencedSecond = QDir(fooDirectory).filePath("second.bmp");
        QImage referenceImage(8, 8, QImage::Format_RGB32);
        referenceImage.fill(Qt::green);
        check(referenceImage.save(referencedFirst, "BMP") &&
                  referenceImage.save(referencedSecond, "BMP"),
              "Cannot create reference-check images");
        success(waveTwo, token, "media.import", {{"paths", QJsonArray{referencedFirst}}});
        check(until([&] { return !session.busy(); }, 15000), "Reference fixture import timed out");
        check(!success(waveTwo, token, "media.pathReferenced", {{"directory", fooDirectory}})
                   .toObject()["referenced"]
                   .toBool(),
              "Directory reference query confused foo with sibling foobar");
        check(!success(waveTwo, token, "media.pathReferenced", {{"path", referencedSecond}})
                   .toObject()["referenced"]
                   .toBool(),
              "Unused file was reported as referenced");
        success(waveTwo, token, "media.sequence",
                {{"paths", QJsonArray{referencedFirst, referencedSecond}},
                 {"fpsNum", 24},
                 {"fpsDen", 1}});
        check(until([&] { return !session.busy(); }, 15000),
              "Mixed-directory sequence import timed out");
        const auto pathReference =
            success(waveTwo, token, "media.pathReferenced", {{"path", referencedSecond}})
                .toObject();
        check(pathReference["referenced"].toBool() &&
                  QJsonDocument(pathReference).toJson(QJsonDocument::Compact).size() < 64,
              "Non-first mixed-directory reference was missed or response was not compact");
        check(success(waveTwo, token, "media.pathReferenced", {{"directory", fooDirectory}})
                  .toObject()["referenced"]
                  .toBool(),
              "Mixed-directory sequence reference was missed");
#ifdef Q_OS_WIN
        check(
            success(waveTwo, token, "media.pathReferenced", {{"path", referencedSecond.toUpper()}})
                .toObject()["referenced"]
                .toBool(),
            "Windows path reference comparison was case sensitive");
#endif
        rejected(waveTwo, token, "media.pathReferenced", {}, -32602);
        rejected(waveTwo, token, "media.pathReferenced",
                 {{"path", referencedSecond}, {"directory", fooDirectory}}, -32602);
        rejected(waveTwo, token, "media.pathReferenced",
                 {{"path", "https://example.invalid/image.png"}}, -32602);
        rejected(waveTwo, token, "media.pathReferenced", {{"directory", true}}, -32602);
        rejected(waveTwo, token, "media.pathReferenced",
                 {{"path", referencedSecond}, {"unknown", true}}, -32602);
        waveTwo.disconnectFromHost();
        QTcpSocket waveFour;
        connectLocal(waveFour, server.listeningPort());
        auto timeoutState =
            success(waveFour, token, "maya.register",
                    {{"sessionId", "maya-timeout"}, {"name", "Synthetic timeout"}, {"pid", 4}})
                .toObject()["maya"]
                .toObject();
        check(timeoutState["timeoutSeconds"].toDouble() == .75, "Maya default timeout changed");
        for (const auto &bad : QJsonArray{true, QJsonValue(), ".5", .049, 120.001}) {
            rejected(
                waveFour, token, "maya.configure",
                {{"sessionId", "maya-timeout"},
                 {"config", QJsonObject{{"direction", "bidirectional"}, {"timeoutSeconds", bad}}}},
                -32602);
            auto unchanged = success(waveFour, token, "status", {{"sessionId", "maya-timeout"}})
                                 .toObject()["maya"]
                                 .toObject();
            check(unchanged["direction"] == timeoutState["direction"] &&
                      unchanged["timeoutSeconds"] == timeoutState["timeoutSeconds"] &&
                      unchanged["configRevision"] == timeoutState["configRevision"],
                  "Invalid timeout partially changed Maya configuration");
        }
        for (double value : {.05, 120.0}) {
            success(waveFour, token, "maya.configure",
                    {{"sessionId", "maya-timeout"},
                     {"config", QJsonObject{{"timeoutSeconds", value}}}});
            check(success(waveFour, token, "status", {{"sessionId", "maya-timeout"}})
                          .toObject()["maya"]
                          .toObject()["timeoutSeconds"]
                          .toDouble() == value,
                  "Maya timeout did not round-trip");
        }
        const auto beforeViewer = projectToJson(session.project());
        for (const auto &bad : QVector<QJsonObject>{
                 {{"mode", "wipe"}, {"wipe", .25}, {"wipePivotY", -0.1}},
                 {{"wipeAngleDegrees", 360001}},
                 {{"wipeAngleDegrees", true}},
                 {{"showPictureType", 1}},
                 {{"color", true}},
                 {{"color", QJsonObject{{"hdrToneMapping", false}, {"sourcePeakNits", -1}}}},
                 {{"color", QJsonObject{{"referenceWhiteNits", 0}}}},
                 {{"color", QJsonObject{{"displayPeakNits", 1001}}}},
                 {{"color", QJsonObject{{"mobiusTransition", .951}}}},
                 {{"color", QJsonObject{{"unknown", true}}}}}) {
            rejected(waveFour, token, "viewer.set", bad, -32602);
            check(projectToJson(session.project()) == beforeViewer,
                  "Invalid viewer fields mutated the project");
        }
        success(waveFour, token, "viewer.set",
                {{"mode", "wipe"},
                 {"wipe", .25},
                 {"wipePivotY", .75},
                 {"wipeAngleDegrees", -90},
                 {"showPictureType", true},
                 {"color", QJsonObject{{"hdrToneMapping", true},
                                       {"sourcePeakNits", 1000},
                                       {"referenceWhiteNits", 100},
                                       {"displayPeakNits", 200},
                                       {"mobiusTransition", .4}}}});
        const auto viewerRoundtrip = success(waveFour, token, "viewer.settings").toObject();
        check(viewerRoundtrip["wipe"].toDouble() == .25 &&
                  viewerRoundtrip["wipePivotY"].toDouble() == .75 &&
                  viewerRoundtrip["wipeAngleDegrees"].toDouble() == -90 &&
                  viewerRoundtrip["showPictureType"].toBool() &&
                  viewerRoundtrip["color"].toObject()["displayPeakNits"].toDouble() == 200,
              "Wipe geometry and color controls did not round-trip");
        const auto frameSource = session.project().sources.last();
        const auto frameMetadata =
            success(waveFour, token, "media.frameMetadata", {{"id", frameSource.id}, {"frame", 0}})
                .toObject();
        check(frameMetadata["pictureType"].toString() == "Unknown" &&
                  frameMetadata["keyFrame"].toBool() && frameMetadata["color"].isObject() &&
                  QJsonDocument(frameMetadata).toJson(QJsonDocument::Compact).size() < 2048,
              "Image-frame metadata is not compact or invented a picture type");
        rejected(waveFour, token, "media.frameMetadata", {{"id", frameSource.id}}, -32602);
        rejected(waveFour, token, "media.frameMetadata", {{"id", frameSource.id}, {"frame", -1}},
                 -32602);
        rejected(waveFour, token, "media.frameMetadata",
                 {{"id", frameSource.id}, {"frame", frameSource.info.frameCount()}}, -32602);
        rejected(waveFour, token, "media.frameMetadata", {{"id", "absent"}, {"frame", 0}}, -32602);
        rejected(waveFour, token, "media.frameMetadata", {{"id", frameSource.id}, {"frame", true}},
                 -32602);
        const auto previousExport = success(waveFour, token, "export.status");
        const auto untouchedDirectory = fixtures.filePath("rejected-color-output");
        for (const auto &bad :
             QVector<QJsonObject>{{{"colorMode", "display_converted"}},
                                  {{"colorMode", "preserve_source"}},
                                  {{"colorMode", "unknown"}},
                                  {{"bitDepth", true}},
                                  {{"bitDepth", "16"}},
                                  {{"bitDepth", 9}},
                                  {{"colorMode", "preserve-source"}, {"viewerComposition", true}},
                                  {{"imageFormat", "jpg"}, {"bitDepth", 16}}}) {
            auto params = bad;
            params["kind"] = "images";
            params["path"] = untouchedDirectory;
            params["first"] = 0;
            params["last"] = 0;
            rejected(waveFour, token, "export.start", params, -32602);
            check(success(waveFour, token, "export.status") == previousExport &&
                      !session.exporting() && !QFileInfo::exists(untouchedDirectory),
                  "Invalid color export changed job state or filesystem before validation");
        }
        // Do not turn requested precision into an eight-bit image silently.
        success(waveFour, token, "viewer.assign", {{"pane", 0}, {"id", frameSource.id}});
        const auto highDepthDirectory = fixtures.filePath("api-png16");
        success(waveFour, token, "export.start",
                {{"kind", "images"},
                 {"path", highDepthDirectory},
                 {"first", 0},
                 {"last", 0},
                 {"width", 32},
                 {"height", 32},
                 {"imageFormat", "png"},
                 {"bitDepth", 16},
                 {"colorMode", "display-converted"},
                 {"viewerComposition", false},
                 {"includeAudio", false}});
        check(until([&] { return !session.exporting(); }, 15000), "API PNG16 export timed out");
        check(success(waveFour, token, "export.status").toObject()["state"].toString() ==
                  "complete",
              "API PNG16 export failed");
        const QImage highDepthImage(highDepthDirectory + "/frame_000001.png");
        check(!highDepthImage.isNull() && highDepthImage.depth() == 64,
              "API bitDepth16 did not reach the PNG encoder");
        auto pressureProject = session.project();
        Annotation pressureStroke;
        pressureStroke.id = newId();
        pressureStroke.timelineId = frameSource.id;
        pressureStroke.points = {{.1, .1}, {.5, .5}, {.9, .9}};
        pressureStroke.widthScales = {0, .4, 1};
        pressureProject.annotations.append(pressureStroke);
        const auto pressureJson = projectToJson(pressureProject);
        const auto pressurePath = fixtures.filePath("pressure-contract.oframe");
        auto writeProjectJson = [&](const QJsonObject &value) {
            QFile file(pressurePath);
            check(file.open(QIODevice::WriteOnly), "Cannot write own pressure project fixture");
            file.write(QJsonDocument(value).toJson());
            file.close();
        };
        writeProjectJson(pressureJson);
        success(waveFour, token, "project.open", {{"path", pressurePath}});
        check(session.project().annotations.last().widthScales == pressureStroke.widthScales,
              "Pressure widths were lost during project import");
        QVector<QJsonObject> invalidProjects;
        for (const auto &bad : QJsonArray{true, "wrong", QJsonArray{.2}, QJsonArray{0, true, 1},
                                          QJsonArray{0, -.1, 1}}) {
            auto value = pressureJson;
            auto annotations = value["annotations"].toArray();
            auto annotation = annotations.last().toObject();
            annotation["widthScales"] = bad;
            annotations[annotations.size() - 1] = annotation;
            value["annotations"] = annotations;
            invalidProjects.append(value);
        }
        for (const auto &key : {"color", "pictureTypes", "keyFrames", "frameColors"}) {
            auto value = pressureJson;
            auto sources = value["sources"].toArray();
            auto source = sources[0].toObject();
            auto info = source["info"].toObject();
            info[key] = true;
            source["info"] = info;
            sources[0] = source;
            value["sources"] = sources;
            invalidProjects.append(value);
        }
        {
            auto value = pressureJson;
            auto viewer = value["viewer"].toObject();
            viewer["color"] = true;
            value["viewer"] = viewer;
            invalidProjects.append(value);
        }
        const auto beforeRejectedProject = projectToJson(session.project());
        const auto beforeRejectedPath = session.projectPath();
        for (const auto &value : invalidProjects) {
            writeProjectJson(value);
            rejected(waveFour, token, "project.open", {{"path", pressurePath}}, -32010);
            check(projectToJson(session.project()) == beforeRejectedProject &&
                      session.projectPath() == beforeRejectedPath,
                  "Invalid new project fields partially replaced the project");
        }
        auto legacyProject = pressureJson;
        auto legacyAnnotations = legacyProject["annotations"].toArray();
        for (qsizetype i = 0; i < legacyAnnotations.size(); ++i) {
            auto annotation = legacyAnnotations[i].toObject();
            annotation.remove("widthScales");
            legacyAnnotations[i] = annotation;
        }
        legacyProject["annotations"] = legacyAnnotations;
        auto legacyViewer = legacyProject["viewer"].toObject();
        for (const auto &key : {"wipePivotY", "wipeAngleDegrees", "color", "showPictureType"})
            legacyViewer.remove(key);
        legacyProject["viewer"] = legacyViewer;
        auto legacySources = legacyProject["sources"].toArray();
        for (qsizetype i = 0; i < legacySources.size(); ++i) {
            auto source = legacySources[i].toObject();
            auto info = source["info"].toObject();
            for (const auto &key : {"color", "frameColors", "pictureTypes", "keyFrames"})
                info.remove(key);
            source["info"] = info;
            legacySources[i] = source;
        }
        legacyProject["sources"] = legacySources;
        Project parsedLegacy;
        check(projectFromJson(legacyProject, parsedLegacy, &error), "Legacy JSON parsing failed");
        check(parsedLegacy.sources.last().info.keyFrames.isEmpty() &&
                  parsedLegacy.sources.last().info.pictureTypes.isEmpty(),
              "Legacy parser invented a derived metadata index");
        writeProjectJson(legacyProject);
        success(waveFour, token, "project.open", {{"path", pressurePath}});
        check(session.project().annotations.last().widthScales.isEmpty() &&
                  session.project().viewer.wipePivotY == .5 &&
                  session.project().viewer.wipeAngleDegrees == 0 &&
                  !session.project().viewer.showPictureType,
              "Missing legacy fields did not migrate to stable defaults");
        check(until([&] { return !session.busy(); }, 15000),
              "Legacy metadata rehydration timed out");
        check(!session.dirty(), "Refreshing derived metadata marked a legacy project dirty");
        session.mutateProject("Synthetic absent metadata", [](Project &project) {
            for (auto &source : project.sources) {
                source.info.pictureTypes.clear();
                source.info.keyFrames.clear();
            }
        });
        const auto legacyFrameMetadata =
            success(waveFour, token, "media.frameMetadata", {{"id", frameSource.id}, {"frame", 0}})
                .toObject();
        check(legacyFrameMetadata["pictureType"].toString() == "Unknown" &&
                  legacyFrameMetadata["keyFrame"].isNull(),
              "Legacy metadata invented key-frame information");
        waveFour.disconnectFromHost();
        QTcpSocket oversized;
        connectLocal(oversized, server.listeningPort());
        oversized.write(QByteArray(ApiServer::MaximumRequestBytes + 1, 'x'));
        check(readReply(oversized)["error"].toObject()["code"].toInt() == -32003,
              "Oversized request was accepted");
        QTcpSocket limited;
        connectLocal(limited, server.listeningPort());
        QByteArray pipeline;
        for (int i = 0; i < 121; ++i)
            pipeline += QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                                  {"api", 1},
                                                  {"id", i},
                                                  {"token", token},
                                                  {"method", "echo"},
                                                  {"params", QJsonObject{{"text", "rate"}}}})
                            .toJson(QJsonDocument::Compact) +
                        '\n';
        limited.write(pipeline);
        bool rateLimited = false;
        for (int i = 0; i < 121; ++i) {
            const auto response = readReply(limited);
            if (response["error"].toObject()["code"].toInt() == -32004) {
                rateLimited = true;
                break;
            }
        }
        check(rateLimited, "Rate limiting did not reject the burst");
        server.stop();
        check(!QFileInfo::exists(endpointPath), "Endpoint file remained after shutdown");
        {
            // Inject a monotonic clock without changing the OS clock or waiting
            // four minutes. Exercise real sockets and the actual expiry timer.
            qint64 now = 0;
            ApiServer ttlServer(&session, nullptr, [&] { return now; });
            check(ttlServer.defaultMayaTimeout() == .75 && ttlServer.setDefaultMayaTimeout(5),
                  "Server Maya default timeout could not be set");
            check(!ttlServer.setDefaultMayaTimeout(std::numeric_limits<double>::quiet_NaN()) &&
                      ttlServer.defaultMayaTimeout() == 5,
                  "Invalid default timeout was not atomic");
            check(ttlServer.start(), "TTL test server startup failed");
            QFile ttlEndpoint(ttlServer.endpointPath());
            check(ttlEndpoint.open(QIODevice::ReadOnly), "Cannot read own TTL endpoint");
            const auto ttlToken =
                QJsonDocument::fromJson(ttlEndpoint.readAll()).object()["token"].toString();
            ttlEndpoint.close();
            QTcpSocket longTimeout, defaultTimeout;
            connectLocal(longTimeout, ttlServer.listeningPort());
            connectLocal(defaultTimeout, ttlServer.listeningPort());
            success(longTimeout, ttlToken, "maya.register",
                    {{"sessionId", "long-timeout"},
                     {"name", "Long deadline"},
                     {"pid", 1},
                     {"timeoutSeconds", 120}});
            success(defaultTimeout, ttlToken, "maya.register",
                    {{"sessionId", "default-timeout"}, {"name", "Default deadline"}, {"pid", 2}});
            check(ttlServer.setDefaultMayaTimeout(2),
                  "Cannot change default timeout for future sessions");
            check(success(defaultTimeout, ttlToken, "status", {{"sessionId", "default-timeout"}})
                          .toObject()["maya"]
                          .toObject()["timeoutSeconds"]
                          .toDouble() == 5,
                  "Changing the default overwrote a registered session");
            now = 31001;
            check(until([&] { return ttlServer.mayaSessions().size() == 1; }, 2500),
                  "Default session did not expire");
            check(ttlServer.mayaSessions()[0].toObject()["sessionId"].toString() == "long-timeout",
                  "Long request timeout was cut short by the old session TTL");
            success(longTimeout, ttlToken, "maya.sync", {{"sessionId", "long-timeout"}});
            now += 245001;
            check(until(
                      [&] {
                          return ttlServer.mayaSessions().isEmpty() &&
                                 longTimeout.state() == QAbstractSocket::UnconnectedState;
                      },
                      2500),
                  "Extended idle connection/session did not expire after its deadline allowance");
            ttlServer.stop();
        }
        std::printf("PASS OpenFrame integration: %d assertions\n", passed);
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "FAIL OpenFrame integration: %s\n", failure.what());
        return 1;
    }
}
