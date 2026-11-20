#include "ApiServer.h"
#include "LocalPathPolicy.h"
#include "core/ReviewSession.h"
#include "media/Exporter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTextDocument>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

namespace openframe {
namespace {
class TextOnlyDocument final : public QTextDocument {
  protected:
    QVariant loadResource(int, const QUrl &) override {
        return {};
    }
};
struct RpcFailure {
    int code;
    QString message;
};
[[noreturn]] void invalid(const QString &message) {
    throw RpcFailure{-32602, message};
}
[[noreturn]] void failed(const QString &message) {
    throw RpcFailure{-32010, message};
}
void keys(const QJsonObject &p, std::initializer_list<const char *> allowed) {
    QSet<QString> names;
    for (const auto key : allowed)
        names.insert(QString::fromLatin1(key));
    for (auto it = p.begin(); it != p.end(); ++it)
        if (!names.contains(it.key()))
            invalid(QStringLiteral("Unknown parameter: %1").arg(it.key()));
}
QString str(const QJsonObject &p, const char *key, bool required = true, int max = 32760) {
    const auto value = p.value(QLatin1String(key));
    if (value.isUndefined() && !required)
        return {};
    if (!value.isString() || value.toString().size() > max ||
        (required && value.toString().isEmpty()))
        invalid(QStringLiteral("Invalid string: %1").arg(QLatin1String(key)));
    return value.toString();
}
double number(const QJsonObject &p, const char *key, double fallback, double minimum,
              double maximum) {
    const auto value = p.value(QLatin1String(key));
    if (value.isUndefined())
        return fallback;
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < minimum ||
        value.toDouble() > maximum)
        invalid(QStringLiteral("Invalid number: %1").arg(QLatin1String(key)));
    return value.toDouble();
}
qint64 integer(const QJsonObject &p, const char *key, qint64 fallback = 0,
               qint64 minimum = -1000000000, qint64 maximum = 1000000000) {
    const double value = number(p, key, static_cast<double>(fallback), static_cast<double>(minimum),
                                static_cast<double>(maximum));
    if (std::trunc(value) != value)
        invalid(QStringLiteral("Integer required: %1").arg(QLatin1String(key)));
    return static_cast<qint64>(value);
}
qint64 exactAudioOffset(const QJsonObject &p) {
    const auto value = p.value("offsetUs");
    qint64 result = 0;
    if (value.isString()) {
        const auto text = value.toString();
        static const QRegularExpression canonical(QStringLiteral("^(0|-?[1-9][0-9]*)$"));
        bool ok = false;
        if (text.size() <= 20 && canonical.match(text).hasMatch())
            result = text.toLongLong(&ok);
        if (!ok)
            invalid("offsetUs must be a canonical signed decimal integer");
    } else {
        if (!value.isDouble())
            invalid("offsetUs must be an integer or a canonical decimal string");
        result = integer(p, "offsetUs", 0, -MaximumAudioOffsetUs, MaximumAudioOffsetUs);
    }
    QString error;
    if (!validateSourceAudioOffset(result, &error))
        invalid(error);
    return result;
}
bool boolean(const QJsonObject &p, const char *key, bool fallback = false) {
    const auto value = p.value(QLatin1String(key));
    if (value.isUndefined())
        return fallback;
    if (!value.isBool())
        invalid(QStringLiteral("Boolean required: %1").arg(QLatin1String(key)));
    return value.toBool();
}
QString checkedPath(const QJsonObject &p, const char *key, bool output = false) {
    QString error;
    const QString path = str(p, key);
    const QString checked = output ? LocalPathPolicy::validateOutput(path, &error)
                                   : LocalPathPolicy::validateInput(path, &error);
    if (checked.isEmpty())
        invalid(error);
    return checked;
}
bool secureEqual(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size())
        return false;
    unsigned char different = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        different |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return different == 0;
}
const QStringList viewerModeNames{"single", "ab", "wipe", "blend", "split", "grid", "single_b"};
const QStringList overlayTypes{"off",     "safeArea", "grid", "center", "dynamicSymmetry",
                               "phiGrid", "custom",   "image"};
const QStringList overlayShapeTypes{"line", "rectangle", "ellipse", "polygon", "text"};
QColor color(const QJsonObject &p, const char *key, const QColor &fallback) {
    if (!p.contains(QLatin1String(key)))
        return fallback;
    const QColor value(str(p, key, true, 128));
    if (!value.isValid())
        invalid(QStringLiteral("Invalid color: %1").arg(QLatin1String(key)));
    return value;
}
QPointF point(const QJsonValue &value, double maximum = 1) {
    if (!value.isArray() || value.toArray().size() != 2)
        invalid("A point must contain two numbers");
    const auto pair = value.toArray();
    return {number({{"v", pair[0]}}, "v", 0, 0, maximum),
            number({{"v", pair[1]}}, "v", 0, 0, maximum)};
}
QJsonArray pointArray(QPointF value) {
    return {value.x(), value.y()};
}
QJsonObject overlayObject(const OverlaySettings &v) {
    QJsonArray shapes;
    for (const auto &shape : v.shapes) {
        QJsonArray points;
        for (const auto &p : shape.points)
            points.append(pointArray(p));
        shapes.append(QJsonObject{{"id", shape.id},
                                  {"type", overlayShapeTypes.value(static_cast<int>(shape.type))},
                                  {"points", points},
                                  {"color", shape.color.name(QColor::HexArgb)},
                                  {"fill", shape.fill.name(QColor::HexArgb)},
                                  {"width", shape.width},
                                  {"opacity", shape.opacity},
                                  {"text", shape.text},
                                  {"fontSize", shape.fontSize},
                                  {"visible", shape.visible}});
    }
    return {{"type", overlayTypes.value(static_cast<int>(v.type))},
            {"color", v.color.name(QColor::HexArgb)},
            {"opacity", v.opacity},
            {"lineWidth", v.lineWidth},
            {"gridColumns", v.gridColumns},
            {"gridRows", v.gridRows},
            {"safeMargins", pointArray(v.safeMargins)},
            {"safeInnerMargins", pointArray(v.safeInnerMargins)},
            {"centerSize", v.centerSize},
            {"shapes", shapes},
            {"imagePath", v.imagePath},
            {"imageRect", QJsonArray{v.imageRect.x(), v.imageRect.y(), v.imageRect.width(),
                                     v.imageRect.height()}},
            {"imageOpacity", v.imageOpacity},
            {"imageKeepAspect", v.imageKeepAspect}};
}
OverlaySettings overlayParams(const QJsonObject &p, OverlaySettings v) {
    keys(p, {"type", "color", "opacity", "lineWidth", "gridColumns", "gridRows", "safeMargins",
             "safeInnerMargins", "centerSize", "shapes", "imagePath", "imageRect", "imageOpacity",
             "imageKeepAspect"});
    if (p.contains("type")) {
        const auto index = overlayTypes.indexOf(str(p, "type", true, 24));
        if (index < 0)
            invalid("Unknown overlay type");
        v.type = static_cast<OverlayType>(index);
    }
    v.color = color(p, "color", v.color);
    v.opacity = number(p, "opacity", v.opacity, 0, 1);
    v.lineWidth = number(p, "lineWidth", v.lineWidth, .1, 64);
    v.gridColumns = static_cast<int>(integer(p, "gridColumns", v.gridColumns, 1, 128));
    v.gridRows = static_cast<int>(integer(p, "gridRows", v.gridRows, 1, 128));
    if (p.contains("safeMargins"))
        v.safeMargins = point(p["safeMargins"], .49);
    if (p.contains("safeInnerMargins"))
        v.safeInnerMargins = point(p["safeInnerMargins"], .49);
    v.centerSize = number(p, "centerSize", v.centerSize, 1, 1024);
    if (p.contains("shapes")) {
        if (!p["shapes"].isArray() || p["shapes"].toArray().size() > 256)
            invalid("shapes must be an array of at most 256 objects");
        v.shapes.clear();
        QSet<QString> ids;
        qsizetype totalPoints = 0;
        for (const auto value : p["shapes"].toArray()) {
            if (!value.isObject())
                invalid("Overlay shape must be an object");
            const auto item = value.toObject();
            keys(item, {"id", "type", "points", "color", "fill", "width", "opacity", "text",
                        "fontSize", "visible"});
            OverlayShape shape;
            shape.id = item.contains("id") ? str(item, "id", true, 128) : newId();
            if (ids.contains(shape.id))
                invalid("Duplicate overlay shape ID");
            ids.insert(shape.id);
            const auto type = overlayShapeTypes.indexOf(str(item, "type", true, 24));
            if (type < 0)
                invalid("Unknown overlay shape type");
            shape.type = static_cast<OverlayShapeType>(type);
            if (!item["points"].isArray())
                invalid("Overlay shape points must be an array");
            const auto points = item["points"].toArray();
            totalPoints += points.size();
            const int minimum = shape.type == OverlayShapeType::Text      ? 1
                                : shape.type == OverlayShapeType::Polygon ? 3
                                                                          : 2;
            if (points.size() < minimum || points.size() > 4096 || totalPoints > 65536 ||
                (shape.type != OverlayShapeType::Polygon && points.size() != minimum))
                invalid("Invalid overlay shape point count");
            for (const auto &value : points)
                shape.points.append(point(value));
            shape.color = color(item, "color", shape.color);
            shape.fill = color(item, "fill", shape.fill);
            shape.width = number(item, "width", shape.width, .1, 64);
            shape.opacity = number(item, "opacity", shape.opacity, 0, 1);
            shape.text = str(item, "text", false, 16384);
            shape.fontSize = static_cast<int>(integer(item, "fontSize", shape.fontSize, 1, 512));
            shape.visible = boolean(item, "visible", true);
            v.shapes.append(shape);
        }
    }
    if (p.contains("imagePath")) {
        v.imagePath = str(p, "imagePath", false);
        if (!v.imagePath.isEmpty())
            v.imagePath = checkedPath(p, "imagePath");
    }
    if (p.contains("imageRect")) {
        if (!p["imageRect"].isArray() || p["imageRect"].toArray().size() != 4)
            invalid("imageRect must contain x, y, width, height");
        const auto rect = p["imageRect"].toArray();
        const auto position = point(QJsonArray{rect[0], rect[1]});
        const auto size = point(QJsonArray{rect[2], rect[3]});
        if (size.x() <= 0 || size.y() <= 0)
            invalid("Overlay image dimensions must be positive");
        v.imageRect = QRectF(position, QSizeF(size.x(), size.y()));
    }
    v.imageOpacity = number(p, "imageOpacity", v.imageOpacity, 0, 1);
    v.imageKeepAspect = boolean(p, "imageKeepAspect", v.imageKeepAspect);
    if (v.type == OverlayType::Image && v.imagePath.isEmpty())
        invalid("Image overlay requires a local imagePath");
    return v;
}
QJsonObject audioTracksObject(const MediaSource &source) {
    QJsonArray tracks;
    for (const auto &track : source.info.audioTracks)
        tracks.append(QJsonObject{{"streamIndex", track.streamIndex},
                                  {"codec", track.codec},
                                  {"title", track.title},
                                  {"language", track.language},
                                  {"sampleRate", track.sampleRate},
                                  {"channels", track.channels},
                                  {"startUs", track.startUs},
                                  {"durationUs", track.durationUs},
                                  {"isDefault", track.isDefault}});
    return {{"selectedAudioTrack", source.selectedAudioTrack}, {"audioTracks", tracks}};
}
QJsonObject sourceObject(const MediaSource &s) {
    return {{"id", s.id},
            {"type", "source"},
            {"name", s.name},
            {"path", s.path},
            {"parent_id", s.folderId},
            {"frame_count", s.info.frameCount()},
            {"fps_num", s.info.fps.num},
            {"fps_den", s.info.fps.den},
            {"missing", s.missing},
            {"selectedAudioTrack", s.selectedAudioTrack},
            {"audioOffsetUs", s.audioOffsetUs},
            {"color", videoColorToJson(s.info.color)},
            {"range_start", s.inFrame},
            {"range_end", s.outFrame},
            {"timecodeOffset", s.timecodeOffset},
            {"timecodeOffsetEnabled", s.timecodeOffsetEnabled}};
}
QJsonObject timelineObject(const Project &p, const Timeline &t) {
    QJsonArray clips;
    for (const auto &c : t.clips)
        clips.append(QJsonObject{{"id", c.id},
                                 {"sourceId", c.sourceId},
                                 {"inFrame", c.inFrame},
                                 {"outFrame", c.outFrame},
                                 {"audioSourceId", c.audioSourceId},
                                 {"audioOffsetUs", c.audioOffsetUs},
                                 {"gain", c.gain},
                                 {"muted", c.muted}});
    return {{"id", t.id},
            {"type", "timeline"},
            {"name", t.name},
            {"clips", clips},
            {"parent_id", t.folderId},
            {"timecodeOffset", t.timecodeOffset},
            {"timecodeOffsetEnabled", t.timecodeOffsetEnabled},
            {"frame_count", p.durationFrames(t.id)},
            {"fps_num", t.fps.num},
            {"fps_den", t.fps.den}};
}
QJsonObject objectById(const Project &p, const QString &id) {
    if (const auto *s = p.source(id))
        return sourceObject(*s);
    if (const auto *t = p.timeline(id))
        return timelineObject(p, *t);
    for (const auto &f : p.folders)
        if (f.id == id)
            return {{"id", f.id}, {"type", "folder"}, {"name", f.name}, {"parent_id", f.parentId}};
    if (!id.isEmpty())
        invalid(QStringLiteral("Object does not exist: %1").arg(id));
    return {};
}
bool knownOwner(const Project &p, const QString &id) {
    return p.source(id) || p.timeline(id);
}
bool hasFolder(const Project &p, const QString &id) {
    return id.isEmpty() || std::any_of(p.folders.cbegin(), p.folders.cend(),
                                       [&](const auto &f) { return f.id == id; });
}
void validateMayaConfig(const QJsonObject &p, bool registration = false) {
    if (registration)
        keys(p, {"sessionId", "name", "pid", "direction", "offset", "multiplier", "fromRangeStart",
                 "mayaRangeStart", "timeoutSeconds"});
    else
        keys(p, {"direction", "offset", "multiplier", "fromRangeStart", "mayaRangeStart",
                 "timeoutSeconds"});
    const auto direction = str(p, "direction", false, 32);
    if (!direction.isEmpty() && direction != "off" && direction != "maya_to_openframe" &&
        direction != "openframe_to_maya" && direction != "bidirectional")
        invalid("Unknown sync direction");
    number(p, "offset", 0, -1e9, 1e9);
    if (number(p, "multiplier", 1, -1000, 1000) == 0)
        invalid("Multiplier must be nonzero");
    boolean(p, "fromRangeStart");
    number(p, "mayaRangeStart", 1, -1e9, 1e9);
    number(p, "timeoutSeconds", .75, .05, 120);
}
qint64 mayaLifetimeMs(const QJsonObject &config) {
    return std::max<qint64>(
        30000,
        static_cast<qint64>(std::ceil(config.value("timeoutSeconds").toDouble(.75) * 2000 + 5000)));
}
bool protectDirectory(const QString &path, QString *error) {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        *error = "Cannot obtain the current user token";
        return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    QByteArray data(static_cast<qsizetype>(bytes), '\0');
    const bool obtained = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes);
    CloseHandle(token);
    if (!obtained) {
        *error = "Cannot determine the current user SID";
        return false;
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) {
        *error = "Cannot convert the user SID";
        return false;
    }
    const QString sddl =
        QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(QString::fromWCharArray(sid));
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr)) {
        *error = "Cannot construct private endpoint permissions";
        return false;
    }
    const QString native = QDir::toNativeSeparators(path);
    const bool ok = SetFileSecurityW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    if (!ok)
        *error = QStringLiteral("Cannot protect endpoint directory (Windows error %1)")
                     .arg(GetLastError());
    return ok;
#else
    if (::chmod(path.toUtf8().constData(), S_IRWXU) != 0) {
        *error = "Cannot protect endpoint directory";
        return false;
    }
    return true;
#endif
}
} // namespace

ApiServer::ApiServer(ReviewSession *session, QObject *parent,
                     std::function<qint64()> monotonicClock)
    : QObject(parent), m_session(session) {
    Q_ASSERT(session);
    m_server.setProxy(QNetworkProxy::NoProxy);
    m_server.setMaxPendingConnections(8);
    connect(&m_server, &QTcpServer::newConnection, this, &ApiServer::acceptConnections);
    connect(session, &ReviewSession::frameChanged, this, [this] {
        ++m_frameRevision;
        ++m_revision;
        m_frameOrigin = m_currentOrigin;
    });
    connect(session, &ReviewSession::projectChanged, this, [this] { ++m_revision; });
    connect(session, &ReviewSession::playbackChanged, this, [this] { ++m_revision; });
    connect(session, &ReviewSession::error, this,
            [this](const QString &message) { m_lastError = message; });
    connect(session, &ReviewSession::exportProgress, this,
            [this](int p) { m_export["progress"] = p; });
    connect(session, &ReviewSession::exportFinished, this,
            [this](bool success, const QString &message) {
                m_export["state"] = success ? "complete" : "failed";
                m_export["success"] = success;
                m_export["message"] = message;
            });
    m_monotonicClock = monotonicClock ? std::move(monotonicClock) : [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    m_housekeeping.setInterval(1000);
    connect(&m_housekeeping, &QTimer::timeout, this, [this] {
        const auto sockets = m_connections.keys();
        const qint64 now = m_monotonicClock();
        for (auto *socket : sockets) {
            const auto *c = m_connections.value(socket);
            const auto timeout =
                c && c->authenticated
                    ? std::max<qint64>(120000, mayaLifetimeMs(m_maya.value(c->mayaId)))
                    : 10000;
            if (c && now - c->activityMs > timeout)
                socket->disconnectFromHost();
        }
        bool changed = false;
        for (auto it = m_maya.begin(); it != m_maya.end();) {
            if (now - m_mayaActivity.value(it.key(), now) > mayaLifetimeMs(it.value())) {
                m_mayaActivity.remove(it.key());
                it = m_maya.erase(it);
                changed = true;
            } else
                ++it;
        }
        if (changed)
            emit clientsChanged();
    });
}
ApiServer::~ApiServer() {
    stop();
}
bool ApiServer::isListening() const {
    return m_server.isListening();
}
quint16 ApiServer::listeningPort() const {
    return m_server.serverPort();
}
QString ApiServer::endpointPath() const {
    return m_endpointPath;
}
QString ApiServer::lastError() const {
    return m_lastError;
}
QString ApiServer::defaultEndpointDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath("OpenFrame/api");
}
void ApiServer::recordError(const QString &message) {
    m_lastError = message;
    emit error(message);
}
bool ApiServer::start(quint16 port) {
    if (isListening())
        return true;
    m_token.clear();
    for (int i = 0; i < 8; ++i) {
        const quint32 value = QRandomGenerator::system()->generate();
        m_token.append(reinterpret_cast<const char *>(&value), sizeof(value));
    }
    m_token = m_token.toHex();
    m_instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Use the numeric IPv4 address, never LocalHost (which can include IPv6).
    if (!m_server.listen(QHostAddress(QStringLiteral("127.0.0.1")), port)) {
        recordError(m_server.errorString());
        m_token.clear();
        return false;
    }
    if (!writeEndpoint()) {
        m_server.close();
        m_token.clear();
        return false;
    }
    m_housekeeping.start();
    emit started();
    return true;
}
void ApiServer::stop() {
    const bool wasListening = isListening();
    m_housekeeping.stop();
    m_server.close();
    const auto sockets = m_connections.keys();
    for (auto *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    qDeleteAll(m_connections);
    m_connections.clear();
    if (!m_endpointPath.isEmpty()) {
        QFile::remove(m_endpointPath);
        m_endpointPath.clear();
    }
    m_token.fill('\0');
    m_token.clear();
    m_maya.clear();
    m_mayaActivity.clear();
    if (wasListening) {
        emit stopped();
        emit clientsChanged();
    }
}
bool ApiServer::writeEndpoint() {
    const QString directory = defaultEndpointDirectory();
    QString error;
    // Validate the nearest existing ancestor before creating each directory.
    const QString base =
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).absolutePath();
    if (LocalPathPolicy::validateDirectory(base, &error).isEmpty()) {
        recordError(error);
        return false;
    }
    QString current = base;
    for (const auto &part : {QStringLiteral("OpenFrame"), QStringLiteral("api")}) {
        current = QDir(current).filePath(part);
        if (LocalPathPolicy::validateDirectoryOutput(current, &error).isEmpty()) {
            recordError(error);
            return false;
        }
        QDir().mkdir(current); // May already exist; validate the directory below.
        if (LocalPathPolicy::validateDirectory(current, &error).isEmpty()) {
            recordError(error);
            return false;
        }
    }
    if (!protectDirectory(directory, &error)) {
        recordError(error);
        return false;
    }
    const QString path = QDir(directory).filePath(QStringLiteral("endpoint-%1-%2.json")
                                                      .arg(QCoreApplication::applicationPid())
                                                      .arg(m_instanceId));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        recordError(file.errorString());
        return false;
    }
#ifndef Q_OS_WIN
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
#endif
    const QJsonObject endpoint{{"api", 1},
                               {"host", "127.0.0.1"},
                               {"port", listeningPort()},
                               {"token", QString::fromLatin1(m_token)},
                               {"pid", QCoreApplication::applicationPid()},
                               {"instance", m_instanceId}};
    const auto bytes = QJsonDocument(endpoint).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        recordError(file.errorString());
        return false;
    }
    m_endpointPath = path;
    return true;
}
void ApiServer::acceptConnections() {
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        if (!socket)
            continue;
        socket->setProxy(QNetworkProxy::NoProxy);
        if (socket->peerAddress().protocol() != QAbstractSocket::IPv4Protocol ||
            socket->peerAddress() != QHostAddress(QStringLiteral("127.0.0.1")) ||
            m_connections.size() >= 8) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *connection = new Connection;
        connection->activityMs = m_monotonicClock();
        connection->rateWindow.start();
        m_connections.insert(socket, connection);
        socket->setReadBufferSize(MaximumRequestBytes + 1);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequests(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            auto *c = m_connections.take(socket);
            if (c && !c->mayaId.isEmpty() && m_maya.contains(c->mayaId)) {
                m_maya[c->mayaId]["connected"] = false;
                emit clientsChanged();
            }
            delete c;
            socket->deleteLater();
        });
    }
}
void ApiServer::readRequests(QTcpSocket *socket) {
    auto *c = m_connections.value(socket);
    if (!c)
        return;
    c->pending += socket->readAll();
    while (true) {
        const qsizetype newline = c->pending.indexOf('\n');
        if (newline < 0)
            break;
        if (newline > MaximumRequestBytes) {
            reject(socket, {}, -32003, "Request too large", true);
            return;
        }
        const QByteArray line = c->pending.left(newline);
        c->pending.remove(0, newline + 1);
        processLine(socket, line);
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
    }
    if (c->pending.size() > MaximumRequestBytes)
        reject(socket, {}, -32003, "Request too large", true);
}
void ApiServer::reply(QTcpSocket *socket, const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > 16 * 1024 * 1024 || socket->bytesToWrite() > 2 * 1024 * 1024) {
        socket->abort();
        return;
    }
    socket->write(bytes);
}
void ApiServer::reject(QTcpSocket *socket, const QJsonValue &id, int code, const QString &message,
                       bool close) {
    reply(socket, {{"jsonrpc", "2.0"},
                   {"id", id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
                   {"error", QJsonObject{{"code", code}, {"message", message}}}});
    if (close)
        socket->disconnectFromHost();
}
void ApiServer::processLine(QTcpSocket *socket, const QByteArray &line) {
    auto *c = m_connections.value(socket);
    if (!c)
        return;
    if (c->rateWindow.elapsed() >= 1000) {
        c->rateWindow.restart();
        c->requests = 0;
    }
    if (++c->requests > 120) {
        reject(socket, {}, -32004, "Rate limit exceeded", true);
        return;
    }
    c->activityMs = m_monotonicClock();
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(line, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        reject(socket, {}, -32700, "Expected one JSON object", true);
        return;
    }
    const auto request = doc.object();
    const auto id = request.value("id");
    try {
        keys(request, {"jsonrpc", "api", "id", "token", "method", "params"});
        if (request.value("jsonrpc") != QJsonValue("2.0") ||
            request.value("api") != QJsonValue(1) || (!id.isString() && !id.isDouble()) ||
            (id.isString() && id.toString().size() > 128) ||
            (id.isDouble() &&
             (!std::isfinite(id.toDouble()) || std::trunc(id.toDouble()) != id.toDouble())))
            throw RpcFailure{-32600, "Invalid JSON-RPC envelope or API version"};
        if (!request.value("token").isString() ||
            !secureEqual(request.value("token").toString().toLatin1(), m_token)) {
            reject(socket, id, -32001, "Authentication failed", true);
            return;
        }
        c->authenticated = true;
        if (!request.value("params").isObject())
            invalid("params must be an object");
        const QString method = str(request, "method", true, 96);
        const auto result = dispatch(method, request.value("params").toObject(), *c);
        reply(socket, {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    } catch (const RpcFailure &failure) {
        reject(socket, id, failure.code, failure.message);
    } catch (const std::exception &) {
        reject(socket, id, -32603, "Internal operation failed");
    }
    m_currentOrigin.clear();
}
QJsonObject ApiServer::status(const QString &mayaId) const {
    const auto &p = m_session->project();
    QJsonObject result{{"api", 1},
                       {"instance", m_instanceId},
                       {"frame", m_session->currentFrame()},
                       {"frameRevision", static_cast<qint64>(m_frameRevision)},
                       {"frameOrigin", m_frameOrigin},
                       {"revision", static_cast<qint64>(m_revision)},
                       {"playing", m_session->playing()},
                       {"autoplay", m_session->autoplay()},
                       {"playbackFrameStep", m_session->playbackFrameStep()},
                       {"rate", m_session->playbackRate()},
                       {"audioTimeStretching", m_session->audioTimeStretchingEnabled()},
                       {"busy", m_session->busy()},
                       {"exporting", m_session->exporting()},
                       {"durationFrames", m_session->durationFrames()},
                       {"rangeIn", p.rangeIn},
                       {"rangeOut", p.rangeOut},
                       {"loopDelayMs", p.loopDelayMs},
                       {"loopHoldEmpty", p.loopHoldEmpty},
                       {"loopDelayActive", m_session->loopDelayActive()},
                       {"activeTimelineId", p.activeTimelineId},
                       {"activeOwnerId", m_session->activeOwnerId()},
                       {"defaultTimelineId", p.defaultTimelineId},
                       {"projectPath", m_session->projectPath()},
                       {"dirty", m_session->dirty()},
                       {"lastError", m_lastError},
                       {"export", m_export}};
    if (!mayaId.isEmpty() && m_maya.contains(mayaId))
        result["maya"] = m_maya.value(mayaId);
    return result;
}
QJsonArray ApiServer::mayaSessions() const {
    QJsonArray result;
    for (auto it = m_maya.cbegin(); it != m_maya.cend(); ++it)
        result.append(it.value());
    return result;
}
double ApiServer::defaultMayaTimeout() const {
    return m_defaultMayaTimeout;
}
bool ApiServer::setDefaultMayaTimeout(double seconds) {
    if (!std::isfinite(seconds) || seconds < .05 || seconds > 120) {
        recordError("Maya timeout must be a finite number in [0.05, 120] seconds");
        return false;
    }
    m_defaultMayaTimeout = seconds;
    return true;
}
bool ApiServer::configureMayaSession(const QString &sessionId, const QJsonObject &config) {
    try {
        validateMayaConfig(config);
        if (!m_maya.contains(sessionId))
            invalid("Maya session is not registered");
        auto &state = m_maya[sessionId];
        for (auto it = config.begin(); it != config.end(); ++it)
            state[it.key()] = it.value();
        state["configRevision"] = state.value("configRevision").toInteger() + 1;
        emit clientsChanged();
        return true;
    } catch (const RpcFailure &e) {
        recordError(e.message);
        return false;
    }
}

QJsonValue ApiServer::dispatch(const QString &method, const QJsonObject &p, Connection &c) {
    if (method == "media.frameMetadata") {
        keys(p, {"id", "frame"});
        const auto id = str(p, "id", true, 128);
        const auto source = m_session->project().source(id);
        if (!source || source->info.frameCount() <= 0)
            invalid("A source with indexed frames is required");
        if (!p.contains("frame"))
            invalid("frame is required");
        const auto frame = integer(p, "frame", 0, 0, source->info.frameCount() - 1);
        return QJsonObject{{"id", id},
                           {"frame", frame},
                           {"pictureType", framePictureType(source->info, frame)},
                           {"keyFrame", frame < source->info.keyFrames.size()
                                            ? QJsonValue(frameIsKeyFrame(source->info, frame))
                                            : QJsonValue(QJsonValue::Null)},
                           {"color", videoColorToJson(frameColorInfo(source->info, frame))}};
    }
    if (method == "window.activate") {
        keys(p, {});
        emit activateRequested();
        return true;
    }
    if (method == "maya.sync") {
        keys(p, {"sessionId", "mayaFrame"});
        const auto id = str(p, "sessionId", true, 128);
        if (id != c.mayaId || !m_maya.contains(id))
            invalid("Maya session is not owned by this connection");
        auto &config = m_maya[id];
        config["lastSeen"] = QDateTime::currentMSecsSinceEpoch();
        m_mayaActivity[id] = m_monotonicClock();
        bool applied = false;
        if (p.contains("mayaFrame")) {
            const double frame = number(p, "mayaFrame", 0, -1e9, 1e9);
            const QString direction = config["direction"].toString();
            if (direction == "maya_to_openframe" || direction == "bidirectional") {
                const bool relative = config["fromRangeStart"].toBool();
                const double mapped =
                    (frame - (relative ? 1.0 : 0.0)) * config["multiplier"].toDouble(1.0) +
                    config["offset"].toDouble() + (relative ? m_session->project().rangeIn : 0);
                m_currentOrigin = id;
                m_session->seek(static_cast<qint64>(std::llround(mapped)), false);
                applied = true;
            }
        }
        auto result = status(id);
        result["syncFrameApplied"] = applied;
        return result;
    }
    if (method == "status") {
        keys(p, {"sessionId"});
        const QString id = str(p, "sessionId", false, 128);
        if (!id.isEmpty()) {
            if (id != c.mayaId || !m_maya.contains(id))
                invalid("Maya session is not owned by this connection");
            m_maya[id]["lastSeen"] = QDateTime::currentMSecsSinceEpoch();
            m_mayaActivity[id] = m_monotonicClock();
        }
        return status(id);
    }
    if (method == "echo") {
        keys(p, {"text"});
        return str(p, "text", false, 4096);
    }
    if (method == "project.get") {
        keys(p, {});
        return projectToJson(m_session->project());
    }
    if (method == "project.new") {
        keys(p, {"empty"});
        const bool empty = boolean(p, "empty");
        m_session->newProject();
        if (empty)
            m_session->mutateProject("Empty project", [](Project &project) {
                project.timelines.clear();
                project.activeTimelineId.clear();
                project.defaultTimelineId.clear();
                project.viewer.assignments.clear();
            });
        return true;
    }
    if (method == "project.defaultTimeline") {
        keys(p, {"id"});
        const auto id = str(p, "id", false, 128);
        if (!id.isEmpty() && !m_session->project().timeline(id))
            invalid("Timeline does not exist");
        m_session->mutateProject("Set default timeline",
                                 [=](Project &project) { project.defaultTimelineId = id; });
        return objectById(m_session->project(), id);
    }
    if (method == "project.open" || method == "project.save" || method == "project.import") {
        keys(p, {"path"});
        const auto path = checkedPath(p, "path", method == "project.save");
        const bool ok = method == "project.open"   ? m_session->openProject(path)
                        : method == "project.save" ? m_session->saveProject(path)
                                                   : m_session->importProject(path);
        if (!ok)
            failed(m_lastError.isEmpty() ? "Project operation failed" : m_lastError);
        return true;
    }
    if (method == "media.sequenceDescriptor") {
        keys(p, {"directory", "baseName", "first", "last", "padding", "extension", "fpsNum",
                 "fpsDen"});
        for (const char *key : {"first", "last", "fpsNum", "fpsDen"})
            if (!p.contains(QLatin1String(key)))
                invalid(QStringLiteral("Required parameter: %1").arg(QLatin1String(key)));
        const auto first = integer(p, "first", 0, -1000000000, 1000000000);
        const auto last = integer(p, "last", 0, -1000000000, 1000000000);
        if (last < first || last - first >= 100000)
            invalid("Sequence must contain 1..100000 consecutive frames");
        const auto padding = static_cast<int>(integer(p, "padding", 4, 1, 12));
        const auto base = str(p, "baseName", true, 128);
        static const QRegularExpression safeBase(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._ -]*$"));
        if (!safeBase.match(base).hasMatch() || base.endsWith('.') || base.endsWith(' '))
            invalid("baseName must be an unambiguous ASCII filename component");
        const auto extension = str(p, "extension", true, 8);
        if (!QStringList{"png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp", "tga", "ppm", "pgm",
                         "pbm"}
                 .contains(extension))
            invalid("Unsupported raster sequence extension");
        const Rational fps{integer(p, "fpsNum", 0, 1, 1000000),
                           integer(p, "fpsDen", 0, 1, 1000000)};
        if (fps.num > fps.den * 1000 || fps.den > fps.num * 1000)
            invalid("Sequence frame rate must be between 0.001 and 1000 fps");
        QString error;
        const auto directory = LocalPathPolicy::validateDirectory(str(p, "directory"), &error);
        if (directory.isEmpty())
            invalid(error);
        if (m_session->busy())
            failed("The session is busy");
        QStringList paths;
        paths.reserve(static_cast<qsizetype>(last - first + 1));
        qint64 pathBytes = 0;
        for (qint64 frame = first; frame <= last; ++frame) {
            const QString digits = QString::number(std::abs(frame)).rightJustified(padding, '0');
            const auto name = base + '.' + (frame < 0 ? "-" : "") + digits + '.' + extension;
            const auto path = QDir(directory).filePath(name);
            pathBytes += static_cast<qint64>(path.size()) * 2;
            if (pathBytes > 16 * 1024 * 1024)
                invalid("Sequence paths exceed the 16 MiB enumeration budget");
            paths.append(path);
        }
        // Avoid blocking the GUI on up to 100000 filesystem checks. The worker
        // checks every expected path before committing one complete source and
        // repeats the guard before decoding images. No directory glob is used.
        m_lastError.clear();
        m_session->importImageSequence(paths, fps);
        return QJsonObject{
            {"accepted", true}, {"busy", m_session->busy()}, {"frameCount", paths.size()}};
    }
    if (method == "media.import" || method == "media.sequence") {
        if (method == "media.import")
            keys(p, {"paths"});
        else
            keys(p, {"paths", "fpsNum", "fpsDen"});
        const auto value = p.value("paths");
        if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 256)
            invalid("paths must contain 1..256 local files");
        QStringList paths;
        for (const auto path : value.toArray()) {
            if (!path.isString())
                invalid("Each media path must be a string");
            QString error;
            const auto local = LocalPathPolicy::validateInput(path.toString(), &error);
            if (local.isEmpty())
                invalid(error);
            paths.append(local);
        }
        if (m_session->busy())
            failed("The session is busy");
        m_lastError.clear();
        if (method == "media.sequence")
            m_session->importImageSequence(paths, Rational{integer(p, "fpsNum", 24, 1, 240000),
                                                           integer(p, "fpsDen", 1, 1, 10000)});
        else
            m_session->importMedia(paths);
        return QJsonObject{{"accepted", true}, {"busy", m_session->busy()}};
    }
    if (method == "media.setAudioOffset") {
        keys(p, {"id", "offsetUs"});
        const auto id = str(p, "id", true, 128);
        const auto offset = exactAudioOffset(p);
        if (!m_session->project().source(id))
            invalid("Source does not exist");
        if (m_session->busy())
            failed("The session is busy");
        m_session->setSourceAudioOffset(id, offset);
        return sourceObject(*m_session->project().source(id));
    }
    if (method == "media.audioTracks" || method == "media.selectAudioTrack") {
        if (method == "media.audioTracks")
            keys(p, {"id"});
        else
            keys(p, {"id", "streamIndex"});
        const auto id = str(p, "id", true, 128);
        const auto source = m_session->project().source(id);
        if (!source)
            invalid("Source does not exist");
        if (method == "media.audioTracks")
            return audioTracksObject(*source);
        if (!p.contains("streamIndex"))
            invalid("streamIndex is required");
        const int index = static_cast<int>(integer(p, "streamIndex", -1, -1, 1000000));
        auto candidate = *source;
        QString error;
        if (!selectSourceAudioTrack(candidate, index, &error))
            invalid(error);
        if (m_session->busy())
            failed("The session is busy");
        m_session->setSourceAudioTrack(id, index);
        return sourceObject(*m_session->project().source(id));
    }
    if (method == "media.relink") {
        keys(p, {"id", "path"});
        const auto id = str(p, "id", true, 128);
        if (!m_session->project().source(id))
            invalid("Source does not exist");
        m_session->relinkSource(id, checkedPath(p, "path"));
        return true;
    }
    if (method == "media.pathReferenced") {
        keys(p, {"path", "directory"});
        const bool directory = p.contains("directory");
        if (directory == p.contains("path"))
            invalid("Exactly one of path or directory is required");
        QString error;
        const auto target = directory
                                ? LocalPathPolicy::validateDirectory(str(p, "directory"), &error)
                                : LocalPathPolicy::validateInput(str(p, "path"), &error);
        if (target.isEmpty())
            invalid(error);
        // Pending import/export snapshots can reference paths not represented by
        // the current project yet. Cleanup clients must retry after these finish.
        if (m_session->busy() || m_session->exporting())
            failed("Path references are unavailable while media work is active");
#ifdef Q_OS_WIN
        constexpr auto pathCase = Qt::CaseInsensitive;
#else
        constexpr auto pathCase = Qt::CaseSensitive;
#endif
        const auto normalize = [](const QString &path) {
            return QDir::cleanPath(QDir::fromNativeSeparators(path));
        };
        const auto exact = normalize(target);
        const auto prefix = exact.endsWith('/') ? exact : exact + '/';
        const auto matches = [&](const QString &path) {
            if (path.isEmpty())
                return false;
            const auto candidate = normalize(path);
            return candidate.compare(exact, pathCase) == 0 ||
                   (directory && candidate.startsWith(prefix, pathCase));
        };
        bool referenced = false;
        for (const auto &source : m_session->project().sources) {
            if (matches(source.path) ||
                std::any_of(source.sequencePaths.cbegin(), source.sequencePaths.cend(), matches)) {
                referenced = true;
                break;
            }
        }
        return QJsonObject{{"referenced", referenced}};
    }
    if (method == "objects.list") {
        keys(p, {"type"});
        const auto type = str(p, "type", false, 16);
        QJsonArray result;
        if (type != "" && type != "source" && type != "timeline" && type != "folder")
            invalid("Unknown object type");
        const auto &project = m_session->project();
        if (type.isEmpty() || type == "source")
            for (const auto &s : project.sources)
                result.append(sourceObject(s));
        if (type.isEmpty() || type == "timeline")
            for (const auto &t : project.timelines)
                result.append(timelineObject(project, t));
        if (type.isEmpty() || type == "folder")
            for (const auto &f : project.folders)
                result.append(objectById(project, f.id));
        return result;
    }
    if (method == "object.get") {
        keys(p, {"id"});
        return objectById(m_session->project(), str(p, "id", false, 128));
    }
    if (method == "folder.add") {
        keys(p, {"name", "parentId"});
        const QString name = str(p, "name", false, 256), parent = str(p, "parentId", false, 128);
        if (!hasFolder(m_session->project(), parent))
            invalid("Parent folder does not exist");
        const QString id = newId();
        m_session->mutateProject("Add folder", [=](Project &project) {
            project.folders.append({id, name.isEmpty() ? "Folder" : name, parent});
        });
        return objectById(m_session->project(), id);
    }
    if (method == "timeline.add") {
        keys(p, {"name", "parentId"});
        const auto parent = str(p, "parentId", false, 128);
        if (!hasFolder(m_session->project(), parent))
            invalid("Parent folder does not exist");
        const auto id = m_session->addTimeline(str(p, "name", false, 256));
        if (!parent.isEmpty())
            m_session->mutateProject("Set timeline folder", [=](Project &project) {
                for (auto &t : project.timelines)
                    if (t.id == id)
                        t.folderId = parent;
            });
        return objectById(m_session->project(), id);
    }
    if (method == "timeline.activate") {
        keys(p, {"id"});
        const auto id = str(p, "id", true, 128);
        if (!m_session->project().timeline(id))
            invalid("Timeline does not exist");
        m_session->setActiveTimeline(id);
        return true;
    }
    if (method == "timeline.insert") {
        keys(p, {"timelineId", "sourceId", "index", "inFrame", "outFrame"});
        const auto tid = str(p, "timelineId", true, 128), sid = str(p, "sourceId", true, 128);
        const auto *source = m_session->project().source(sid);
        if (!m_session->project().timeline(tid) || !source)
            invalid("Timeline or source does not exist");
        const int index = static_cast<int>(integer(p, "index", -1, -1, 1000000));
        const auto in = integer(p, "inFrame", source->inFrame, 0),
                   out = integer(p, "outFrame", source->outFrame, -1);
        if ((out >= 0 && out < in) || in >= source->info.frameCount() ||
            out >= source->info.frameCount())
            invalid("Invalid source range");
        const auto id = newId();
        m_session->mutateProject("Insert timeline clip", [=](Project &project) {
            for (auto &t : project.timelines)
                if (t.id == tid) {
                    Clip clip;
                    clip.id = id;
                    clip.sourceId = sid;
                    clip.inFrame = in;
                    clip.outFrame = out;
                    t.clips.insert(index < 0 || index > t.clips.size() ? t.clips.size() : index,
                                   clip);
                }
        });
        return QJsonObject{{"id", id}};
    }
    if (method == "timeline.removeClip" || method == "timeline.moveClip" ||
        method == "timeline.updateClip") {
        if (method == "timeline.removeClip")
            keys(p, {"timelineId", "index"});
        else if (method == "timeline.moveClip")
            keys(p, {"timelineId", "index", "toIndex"});
        else
            keys(p, {"timelineId", "index", "inFrame", "outFrame", "audioSourceId", "audioOffsetUs",
                     "gain", "muted"});
        const auto id = str(p, "timelineId", true, 128);
        const auto *timeline = m_session->project().timeline(id);
        const int index = static_cast<int>(integer(p, "index", -1, -1, 1000000));
        if (!timeline || index < 0 || index >= timeline->clips.size())
            invalid("Clip index is invalid");
        Clip clip = timeline->clips[index];
        const auto *source = m_session->project().source(clip.sourceId);
        const int to =
            static_cast<int>(integer(p, "toIndex", index, 0, timeline->clips.size() - 1));
        if (method == "timeline.updateClip") {
            clip.inFrame = integer(p, "inFrame", clip.inFrame, 0);
            clip.outFrame = integer(p, "outFrame", clip.outFrame, -1);
            if (clip.outFrame >= 0 && clip.outFrame < clip.inFrame)
                invalid("Clip range is reversed");
            if (source && (clip.inFrame >= source->info.frameCount() ||
                           clip.outFrame >= source->info.frameCount()))
                invalid("Clip range exceeds source");
            if (p.contains("audioSourceId")) {
                clip.audioSourceId = str(p, "audioSourceId", false, 128);
                if (!clip.audioSourceId.isEmpty() &&
                    !m_session->project().source(clip.audioSourceId))
                    invalid("Audio source does not exist");
            }
            clip.audioOffsetUs =
                integer(p, "audioOffsetUs", clip.audioOffsetUs, -1000000000000LL, 1000000000000LL);
            clip.gain = number(p, "gain", clip.gain, 0, 10);
            clip.muted = boolean(p, "muted", clip.muted);
        }
        m_session->mutateProject("Edit timeline clip", [=](Project &project) {
            for (auto &t : project.timelines)
                if (t.id == id) {
                    if (method == "timeline.removeClip")
                        t.clips.removeAt(index);
                    else if (method == "timeline.moveClip")
                        t.clips.move(index, to);
                    else
                        t.clips[index] = clip;
                }
        });
        return true;
    }
    if (method == "object.update") {
        keys(p, {"id", "name", "parentId", "inFrame", "outFrame"});
        const auto id = str(p, "id", true, 128);
        objectById(m_session->project(), id);
        const auto name = str(p, "name", false, 256), parent = str(p, "parentId", false, 128);
        if (!hasFolder(m_session->project(), parent) || parent == id)
            invalid("Invalid parent folder");
        const auto *source = m_session->project().source(id);
        const auto in = integer(p, "inFrame", source ? source->inFrame : 0, 0);
        const auto out = integer(p, "outFrame", source ? source->outFrame : -1, -1);
        if ((p.contains("inFrame") || p.contains("outFrame")) &&
            (!source || (out >= 0 && out < in) || in >= source->info.frameCount() ||
             out >= source->info.frameCount()))
            invalid("Invalid source trim range");
        QString ancestor = parent;
        for (int remaining = m_session->project().folders.size() + 1;
             !ancestor.isEmpty() && remaining > 0; --remaining) {
            if (ancestor == id)
                invalid("Folder cycle is forbidden");
            QString next;
            for (const auto &f : m_session->project().folders)
                if (f.id == ancestor)
                    next = f.parentId;
            ancestor = next;
        }
        m_session->mutateProject("Update object", [=](Project &project) {
            for (auto &s : project.sources)
                if (s.id == id) {
                    if (p.contains("name"))
                        s.name = name;
                    if (p.contains("parentId"))
                        s.folderId = parent;
                    s.inFrame = in;
                    s.outFrame = out;
                }
            for (auto &t : project.timelines)
                if (t.id == id) {
                    if (p.contains("name"))
                        t.name = name;
                    if (p.contains("parentId"))
                        t.folderId = parent;
                }
            for (auto &f : project.folders)
                if (f.id == id) {
                    if (p.contains("name"))
                        f.name = name;
                    if (p.contains("parentId"))
                        f.parentId = parent;
                }
        });
        return objectById(m_session->project(), id);
    }
    if (method == "object.remove") {
        keys(p, {"id", "force"});
        const auto id = str(p, "id", true, 128);
        const bool force = boolean(p, "force");
        objectById(m_session->project(), id);
        if (m_session->project().source(id) && !force)
            for (const auto &t : m_session->project().timelines)
                for (const auto &clip : t.clips)
                    if (clip.sourceId == id || clip.audioSourceId == id)
                        failed("Source is used by a timeline; force is required");
        for (const auto &f : m_session->project().folders)
            if (f.parentId == id)
                failed("Folder is not empty");
        for (const auto &s : m_session->project().sources)
            if (s.folderId == id)
                failed("Folder is not empty");
        for (const auto &t : m_session->project().timelines)
            if (t.folderId == id)
                failed("Folder is not empty");
        m_session->mutateProject("Remove object", [=](Project &project) {
            project.sources.removeIf([&](const auto &s) { return s.id == id; });
            project.timelines.removeIf([&](const auto &t) { return t.id == id; });
            project.folders.removeIf([&](const auto &f) { return f.id == id; });
            for (auto &t : project.timelines) {
                t.clips.removeIf([&](const auto &clip) { return clip.sourceId == id; });
                for (auto &clip : t.clips)
                    if (clip.audioSourceId == id)
                        clip.audioSourceId.clear();
            }
            project.bookmarks.removeIf([&](const auto &b) { return b.timelineId == id; });
            project.notes.removeIf([&](const auto &n) { return n.timelineId == id; });
            project.annotations.removeIf([&](const auto &a) { return a.timelineId == id; });
            for (auto &assignment : project.viewer.assignments)
                if (assignment == id)
                    assignment.clear();
            if (project.activeTimelineId == id)
                project.activeTimelineId =
                    project.timelines.isEmpty() ? QString() : project.timelines.first().id;
            if (project.defaultTimelineId == id)
                project.defaultTimelineId.clear();
        });
        return true;
    }
    if (method == "seek") {
        keys(p, {"frame", "audio", "origin", "fromRangeStart", "ignoreLoopState"});
        if (!p.contains("frame"))
            invalid("frame is required");
        auto frame = integer(p, "frame");
        if (boolean(p, "fromRangeStart"))
            frame += m_session->project().rangeIn;
        m_currentOrigin = str(p, "origin", false, 128);
        if (!m_currentOrigin.isEmpty() && m_currentOrigin != c.mayaId)
            invalid("Origin is not owned by this connection");
        const bool audio = boolean(p, "audio"), ignoreLoop = boolean(p, "ignoreLoopState");
        const bool oldLoop = m_session->loopOnStepScrub(),
                   oldLock = m_session->lockStepScrubToRange();
        if (ignoreLoop) {
            m_session->setLoopOnStepScrub(false);
            m_session->setLockStepScrubToRange(false);
        }
        m_session->seek(frame, audio);
        if (ignoreLoop) {
            m_session->setLoopOnStepScrub(oldLoop);
            m_session->setLockStepScrubToRange(oldLock);
        }
        return m_session->currentFrame();
    }
    if (method == "audio.timeStretch") {
        keys(p, {"enabled"});
        const bool enabled = boolean(p, "enabled", m_session->audioTimeStretchingEnabled());
        if (p.contains("enabled"))
            m_session->setAudioTimeStretchingEnabled(enabled);
        return QJsonObject{{"enabled", m_session->audioTimeStretchingEnabled()}};
    }
    if (method == "play") {
        keys(p, {"playing", "rate", "audioTimeStretching"});
        const bool playing = boolean(p, "playing", true);
        const double rate = number(p, "rate", m_session->playbackRate(), -16, 16);
        const bool stretch =
            boolean(p, "audioTimeStretching", m_session->audioTimeStretchingEnabled());
        if (std::abs(rate) < 0.0625)
            invalid("Absolute playback rate must be between 1/16 and 16");
        if (p.contains("audioTimeStretching"))
            m_session->setAudioTimeStretchingEnabled(stretch);
        m_session->setPlaybackRate(rate);
        if (playing)
            m_session->play();
        else
            m_session->pause();
        return true;
    }
    if (method == "range.set") {
        keys(p, {"first", "last", "loop", "loopDelayMs", "loopHoldEmpty"});
        const auto first = integer(p, "first", m_session->project().rangeIn, 0),
                   last = integer(p, "last", m_session->project().rangeOut, -1);
        if (last >= 0 && last < first)
            invalid("Range end precedes start");
        const auto loop = str(p, "loop", false, 16);
        if (!loop.isEmpty() && loop != "once" && loop != "loop" && loop != "pingpong")
            invalid("Unknown loop mode");
        const auto delay = static_cast<int>(
            integer(p, "loopDelayMs", m_session->project().loopDelayMs, 0, 600000));
        const auto empty = boolean(p, "loopHoldEmpty", m_session->project().loopHoldEmpty);
        m_session->setRange(first, last);
        if (p.contains("loopDelayMs") || p.contains("loopHoldEmpty"))
            m_session->setLoopDelay(delay, empty);
        if (!loop.isEmpty())
            m_session->setLoopMode(loop == "once"   ? LoopMode::Once
                                   : loop == "loop" ? LoopMode::Loop
                                                    : LoopMode::PingPong);
        return QJsonArray{m_session->project().rangeIn, m_session->project().rangeOut};
    }
    if (method == "timecode.get" || method == "timecode.set") {
        if (method == "timecode.get")
            keys(p, {"id"});
        else
            keys(p, {"id", "offset", "enabled"});
        const QString id = p.contains("id") ? str(p, "id", true, 128) : m_session->activeOwnerId();
        if (!knownOwner(m_session->project(), id))
            invalid("Timecode owner does not exist");
        if (method == "timecode.set") {
            const auto offset = integer(p, "offset");
            const bool enabled = boolean(p, "enabled", true);
            m_session->mutateProject("Set timecode offset", [=](Project &project) {
                for (auto &s : project.sources)
                    if (s.id == id) {
                        s.timecodeOffset = offset;
                        s.timecodeOffsetEnabled = enabled;
                    }
                for (auto &t : project.timelines)
                    if (t.id == id) {
                        t.timecodeOffset = offset;
                        t.timecodeOffsetEnabled = enabled;
                    }
            });
        }
        const auto object = objectById(m_session->project(), id);
        return QJsonObject{{"offset", object.value("timecodeOffset")},
                           {"enabled", object.value("timecodeOffsetEnabled")}};
    }
    if (method == "viewer.get") {
        keys(p, {"pane"});
        const auto pane = integer(p, "pane", 0, 0, 3);
        const auto &assignments = m_session->project().viewer.assignments;
        return objectById(m_session->project(), pane < assignments.size()
                                                    ? assignments[static_cast<int>(pane)]
                                                    : QString());
    }
    if (method == "viewer.assign") {
        keys(p, {"pane", "id"});
        const int pane = static_cast<int>(integer(p, "pane", 0, 0, 3));
        const auto id = str(p, "id", false, 128);
        if (!id.isEmpty() && !knownOwner(m_session->project(), id))
            invalid("Viewer assignment does not exist");
        m_session->assignViewer(pane, id);
        return objectById(m_session->project(), id);
    }
    if (method == "overlays.get" || method == "overlays.set") {
        if (method == "overlays.get")
            keys(p, {});
        else {
            auto settings = m_session->project().viewer;
            settings.overlay = overlayParams(p, settings.overlay);
            settings.overlayGrid = settings.overlay.type == OverlayType::Grid;
            settings.overlaySafe = settings.overlay.type == OverlayType::SafeArea;
            settings.overlayCenter = settings.overlay.type == OverlayType::Center;
            QString error;
            if (!validateViewerSettings(settings, &error))
                invalid(error);
            m_session->setViewerSettings(settings);
        }
        return overlayObject(m_session->project().viewer.overlay);
    }
    if (method == "viewer.settings") {
        keys(p, {});
        const auto &viewer = m_session->project().viewer;
        return QJsonObject{
            {"mode", viewerModeNames.value(static_cast<int>(viewer.mode))},
            {"activePane", viewer.activePane},
            {"wipe", viewer.wipe},
            {"wipePivotY", viewer.wipePivotY},
            {"wipeAngleDegrees", viewer.wipeAngleDegrees},
            {"showPictureType", viewer.showPictureType},
            {"color", QJsonObject{{"hdrToneMapping", viewer.color.hdrToneMapping},
                                  {"sourcePeakNits", viewer.color.sourcePeakNits},
                                  {"referenceWhiteNits", viewer.color.referenceWhiteNits},
                                  {"displayPeakNits", viewer.color.displayPeakNits},
                                  {"mobiusTransition", viewer.color.mobiusTransition}}},
            {"panZoom", QJsonObject{{"zoom", viewer.panZoom.zoom},
                                    {"panX", viewer.panZoom.pan.x()},
                                    {"panY", viewer.panZoom.pan.y()},
                                    {"enabled", viewer.panZoom.enabled}}},
            {"overlay", overlayObject(viewer.overlay)}};
    }
    if (method == "viewer.set") {
        keys(p, {"mode", "splitVertical", "wipe", "opacity", "showOverlay", "showAnnotations",
                 "pane", "zoom", "panX", "panY", "panZoomEnabled", "rotation", "flipHorizontal",
                 "flipVertical", "wipePivotY", "wipeAngleDegrees", "color", "showPictureType"});
        auto settings = m_session->project().viewer;
        const auto mode = str(p, "mode", false, 24);
        if (!mode.isEmpty()) {
            const auto at = viewerModeNames.indexOf(mode == "active" ? QStringLiteral("ab") : mode);
            if (at < 0)
                invalid("Unknown viewer mode");
            settings.mode = static_cast<ViewerMode>(at);
        }
        settings.wipe = number(p, "wipe", settings.wipe, 0, 1);
        settings.wipePivotY = number(p, "wipePivotY", settings.wipePivotY, 0, 1);
        settings.wipeAngleDegrees =
            number(p, "wipeAngleDegrees", settings.wipeAngleDegrees, -360000, 360000);
        settings.showPictureType = boolean(p, "showPictureType", settings.showPictureType);
        if (p.contains("color")) {
            if (!p["color"].isObject())
                invalid("color must be an object");
            const auto values = p["color"].toObject();
            keys(values, {"hdrToneMapping", "sourcePeakNits", "referenceWhiteNits",
                          "displayPeakNits", "mobiusTransition"});
            auto &color = settings.color;
            color.hdrToneMapping = boolean(values, "hdrToneMapping", color.hdrToneMapping);
            color.sourcePeakNits = number(values, "sourcePeakNits", color.sourcePeakNits, 0, 10000);
            color.referenceWhiteNits =
                number(values, "referenceWhiteNits", color.referenceWhiteNits, 1, 1000);
            color.displayPeakNits =
                number(values, "displayPeakNits", color.displayPeakNits, 1, 1000);
            color.mobiusTransition =
                number(values, "mobiusTransition", color.mobiusTransition, 0, .95);
        }
        settings.opacity = number(p, "opacity", settings.opacity, 0, 1);
        settings.showOverlay = boolean(p, "showOverlay", settings.showOverlay);
        settings.showAnnotations = boolean(p, "showAnnotations", settings.showAnnotations);
        settings.splitVertical = boolean(p, "splitVertical", settings.splitVertical);
        const int pane = static_cast<int>(integer(p, "pane", activeViewerPane(settings), 0, 3));
        settings.transforms.resize(4);
        auto &transform = settings.transforms[pane];
        settings.panZoom.zoom = number(p, "zoom", settings.panZoom.zoom, 0.01, 64);
        settings.panZoom.pan = {number(p, "panX", settings.panZoom.pan.x(), -100, 100),
                                number(p, "panY", settings.panZoom.pan.y(), -100, 100)};
        settings.panZoom.enabled = boolean(p, "panZoomEnabled", settings.panZoom.enabled);
        transform.rotation =
            static_cast<int>(integer(p, "rotation", transform.rotation, -360, 360));
        transform.flipHorizontal = boolean(p, "flipHorizontal", transform.flipHorizontal);
        transform.flipVertical = boolean(p, "flipVertical", transform.flipVertical);
        QString error;
        if (!validateViewerSettings(settings, &error))
            invalid(error);
        m_session->setViewerSettings(settings);
        return true;
    }
    if (method == "bookmarks.list") {
        keys(p, {"id"});
        const auto id = str(p, "id", false, 128);
        QJsonArray result;
        for (const auto &b : m_session->project().bookmarks)
            if (id.isEmpty() || b.timelineId == id)
                result.append(QJsonObject{{"id", b.id},
                                          {"ownerId", b.timelineId},
                                          {"frame", b.frame},
                                          {"endFrame", b.endFrame},
                                          {"name", b.name},
                                          {"text", b.text}});
        return result;
    }
    if (method == "annotations.list") {
        keys(p, {"ownerId"});
        const auto owner = str(p, "ownerId", false, 128);
        QJsonArray result;
        for (const auto &a : m_session->project().annotations)
            if (owner.isEmpty() || a.timelineId == owner)
                result.append(QJsonObject{{"id", a.id},
                                          {"ownerId", a.timelineId},
                                          {"frame", a.frame},
                                          {"endFrame", a.endFrame},
                                          {"layer", a.layer},
                                          {"text", a.text},
                                          {"visible", a.visible},
                                          {"hold", a.hold}});
        return result;
    }
    if (method == "bookmarks.add") {
        keys(p, {"ownerId", "frames", "endFrame", "name", "text"});
        const auto owner =
            p.contains("ownerId") ? str(p, "ownerId", true, 128) : m_session->activeOwnerId();
        if (!knownOwner(m_session->project(), owner))
            invalid("Bookmark owner does not exist");
        if (!p.value("frames").isArray() || p.value("frames").toArray().size() > 4096)
            invalid("frames must be an array of at most 4096 integers");
        QVector<qint64> frames;
        for (const auto value : p.value("frames").toArray())
            frames.append(integer({{"v", value}}, "v", 0, 0));
        const auto last = integer(p, "endFrame", -1, -1);
        const auto name = str(p, "name", false, 256), text = str(p, "text", false, 16384);
        int added = 0;
        m_session->mutateProject("Add bookmarks", [&](Project &project) {
            for (auto frame : frames) {
                const bool exists = std::any_of(
                    project.bookmarks.cbegin(), project.bookmarks.cend(),
                    [&](const auto &b) { return b.timelineId == owner && b.frame == frame; });
                if (!exists) {
                    Bookmark b;
                    b.id = newId();
                    b.timelineId = owner;
                    b.frame = frame;
                    b.endFrame = std::max(frame, last);
                    b.name = name;
                    b.text = text;
                    project.bookmarks.append(b);
                    ++added;
                }
            }
        });
        return added;
    }
    if (method == "bookmarks.clear") {
        keys(p, {"ownerId", "bookmarks", "annotations", "notes"});
        const auto owner = str(p, "ownerId", true, 128);
        if (!knownOwner(m_session->project(), owner))
            invalid("Owner does not exist");
        const bool bookmarks = boolean(p, "bookmarks", true),
                   annotations = boolean(p, "annotations", true), notes = boolean(p, "notes", true);
        m_session->mutateProject("Clear bookmarks", [&](Project &project) {
            if (bookmarks)
                project.bookmarks.removeIf([&](const auto &b) { return b.timelineId == owner; });
            if (annotations)
                project.annotations.removeIf([&](const auto &a) { return a.timelineId == owner; });
            if (notes)
                project.notes.removeIf([&](const auto &n) { return n.timelineId == owner; });
        });
        return true;
    }
    if (method == "notes.list") {
        keys(p, {"ownerId", "plainText"});
        const auto owner = str(p, "ownerId", false, 128);
        QJsonArray result;
        const bool plainText = boolean(p, "plainText");
        for (const auto &n : m_session->project().notes)
            if (owner.isEmpty() || n.timelineId == owner) {
                QString text = n.text;
                if (plainText) {
                    TextOnlyDocument document;
                    document.setMarkdown(text);
                    text = document.toPlainText();
                }
                result.append(QJsonObject{{"id", n.id},
                                          {"ownerId", n.timelineId},
                                          {"frame", n.frame},
                                          {"text", text},
                                          {"author", n.author},
                                          {"color", n.color.name(QColor::HexArgb)},
                                          {"created", n.created.toString(Qt::ISODateWithMs)}});
            }
        return result;
    }
    if (method == "notes.add") {
        keys(p, {"ownerId", "notes", "author", "overwrite"});
        const auto owner = str(p, "ownerId", true, 128), author = str(p, "author", false, 256);
        if (!knownOwner(m_session->project(), owner))
            invalid("Note owner does not exist");
        if (!p.value("notes").isArray() || p.value("notes").toArray().size() > 1024)
            invalid("notes must be an array of at most 1024 records");
        QVector<Note> incoming;
        QSet<QString> explicitColors;
        for (const auto value : p.value("notes").toArray()) {
            if (!value.isObject())
                invalid("Note must be an object");
            const auto n = value.toObject();
            keys(n, {"frame", "text", "color"});
            Note note;
            note.id = newId();
            note.timelineId = owner;
            note.frame = integer(n, "frame", 0, 0);
            note.text = str(n, "text", false, 32760);
            note.author = author;
            if (n.contains("color")) {
                note.color = QColor(str(n, "color", true, 32));
                if (!note.color.isValid())
                    invalid("Invalid note color");
                explicitColors.insert(note.id);
            }
            incoming.append(note);
        }
        const bool overwrite = boolean(p, "overwrite");
        int changed = 0;
        m_session->mutateProject("Add notes", [&](Project &project) {
            for (const auto &note : incoming) {
                auto existing =
                    std::find_if(project.notes.begin(), project.notes.end(), [&](const auto &n) {
                        return n.timelineId == owner && n.frame == note.frame;
                    });
                if (existing == project.notes.end()) {
                    project.notes.append(note);
                    ++changed;
                } else if (overwrite) {
                    existing->text = note.text;
                    existing->author = note.author;
                    if (explicitColors.contains(note.id))
                        existing->color = note.color;
                    ++changed;
                }
            }
        });
        return changed;
    }
    if (method == "notes.importCsv" || method == "notes.exportCsv") {
        keys(p, {"path"});
        const auto path = checkedPath(p, "path", method == "notes.exportCsv");
        const bool ok = method == "notes.importCsv" ? m_session->loadNotesCsv(path)
                                                    : m_session->saveNotesCsv(path);
        if (!ok)
            failed(m_lastError);
        return true;
    }
    if (method == "history.undo" || method == "history.redo") {
        keys(p, {});
        if (method == "history.undo")
            m_session->undoStack()->undo();
        else
            m_session->undoStack()->redo();
        return true;
    }
    if (method == "export.start") {
        keys(p, {"path",
                 "kind",
                 "timelineId",
                 "first",
                 "last",
                 "width",
                 "height",
                 "fpsNum",
                 "fpsDen",
                 "colorMode",
                 "bitDepth",
                 "viewerComposition",
                 "includeAnnotations",
                 "includeAudio",
                 "videoCodec",
                 "audioCodec",
                 "imageFormat",
                 "crf",
                 "frameStep",
                 "bookmarksOnly",
                 "frameNumbering",
                 "sequenceStart",
                 "sequencePadding",
                 "sequenceBaseName",
                 "publishSequenceIntoExistingDirectory",
                 "annotationBackground"});
        if (m_session->exporting() || m_session->busy())
            failed("An operation is already running");
        ExportSettings settings;
        const auto kind = str(p, "kind", false, 24);
        if (!kind.isEmpty() && kind != "video" && kind != "audio" && kind != "images")
            invalid("Unknown export kind");
        settings.kind = kind == "audio"    ? ExportKind::Audio
                        : kind == "images" ? ExportKind::ImageSequence
                                           : ExportKind::Video;
        settings.publishSequenceIntoExistingDirectory =
            boolean(p, "publishSequenceIntoExistingDirectory");
        if (settings.publishSequenceIntoExistingDirectory) {
            if (settings.kind != ExportKind::ImageSequence)
                invalid("Existing-directory publication requires an image sequence");
            QString error;
            settings.path = LocalPathPolicy::validateDirectory(str(p, "path"), &error);
            if (settings.path.isEmpty())
                invalid(error);
        } else
            settings.path = checkedPath(p, "path", true);
        settings.timelineId = str(p, "timelineId", false, 128);
        if (!settings.timelineId.isEmpty() && !m_session->project().timeline(settings.timelineId))
            invalid("Timeline does not exist");
        settings.firstFrame = integer(p, "first", m_session->project().rangeIn, 0);
        settings.lastFrame = integer(p, "last", m_session->project().rangeOut, -1);
        if (settings.lastFrame >= 0 && settings.lastFrame < settings.firstFrame)
            invalid("Export range is reversed");
        settings.size = QSize(static_cast<int>(integer(p, "width", 1920, 16, 8192)),
                              static_cast<int>(integer(p, "height", 1080, 16, 8192)));
        if (p.contains("fpsNum") != p.contains("fpsDen"))
            invalid("fpsNum and fpsDen must be provided together");
        if (p.contains("fpsNum")) {
            settings.outputFps = {integer(p, "fpsNum", 0, 1, 1000000),
                                  integer(p, "fpsDen", 0, 1, 1000000)};
            if (settings.outputFps.num > settings.outputFps.den * 1000 ||
                settings.outputFps.den > settings.outputFps.num * 1000)
                invalid("Output frame rate must be between 0.001 and 1000 fps");
        }
        settings.viewerComposition = boolean(p, "viewerComposition", true);
        settings.includeAnnotations = boolean(p, "includeAnnotations", true);
        settings.includeAudio = boolean(p, "includeAudio", true);
        const auto colorMode =
            p.contains("colorMode") ? str(p, "colorMode", true, 32) : QString("display-converted");
        if (colorMode != "display-converted" && colorMode != "preserve-source")
            invalid("Unknown export colorMode");
        settings.colorMode = colorMode == "preserve-source" ? ExportColorMode::PreserveSource
                                                            : ExportColorMode::DisplayConverted;
        settings.bitDepth = static_cast<int>(integer(p, "bitDepth", 8, 0, 16));
        if (settings.bitDepth != 0 && settings.bitDepth != 8 && settings.bitDepth != 10 &&
            settings.bitDepth != 12 && settings.bitDepth != 16)
            invalid("bitDepth must be 0, 8, 10, 12 or 16");
        settings.crf = static_cast<int>(integer(p, "crf", 18, 0, 51));
        settings.frameStep = static_cast<int>(integer(p, "frameStep", 1, 1, 1000000));
        settings.bookmarksOnly = boolean(p, "bookmarksOnly");
        if (settings.bookmarksOnly && settings.kind != ExportKind::ImageSequence)
            invalid("Bookmarks-only export requires an image sequence");
        if (p.contains("frameNumbering"))
            settings.frameNumbering = str(p, "frameNumbering", true, 32);
        if (!QStringList{"zero_based", "one_based", "match_frame", "custom"}.contains(
                settings.frameNumbering))
            invalid("Unknown sequence numbering mode");
        settings.sequenceStart = integer(p, "sequenceStart", 1);
        settings.sequencePadding = static_cast<int>(integer(p, "sequencePadding", 6, 1, 12));
        if (p.contains("sequenceBaseName"))
            settings.sequenceBaseName = str(p, "sequenceBaseName", true, 128);
        static const QRegularExpression badName(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
        if (badName.match(settings.sequenceBaseName).hasMatch() ||
            settings.sequenceBaseName.endsWith('.') || settings.sequenceBaseName.endsWith(' ') ||
            settings.sequenceBaseName == "." || settings.sequenceBaseName == "..")
            invalid("Sequence base name must be an unambiguous filename component");
        if (p.contains("annotationBackground")) {
            if (!p["annotationBackground"].isArray() ||
                p["annotationBackground"].toArray().size() != 4)
                invalid("annotationBackground must contain four RGBA integers");
            const auto rgba = p["annotationBackground"].toArray();
            int components[4];
            for (int i = 0; i < 4; ++i)
                components[i] = static_cast<int>(integer({{"v", rgba[i]}}, "v", 0, 0, 255));
            settings.overrideAnnotationBackground = true;
            settings.annotationBackground =
                QColor(components[0], components[1], components[2], components[3]);
        }
        if (p.contains("videoCodec"))
            settings.videoCodec = str(p, "videoCodec", true, 32);
        if (p.contains("audioCodec"))
            settings.audioCodec = str(p, "audioCodec", true, 32);
        if (p.contains("imageFormat"))
            settings.imageFormat = str(p, "imageFormat", true, 16);
        if (!QStringList{"libx264", "ffv1", "mpeg4", "rawvideo", "h264_mf"}.contains(
                settings.videoCodec) ||
            !QStringList{"aac", "pcm_s16le", "flac", "libopus"}.contains(settings.audioCodec) ||
            !QStringList{"png", "jpg", "bmp", "tiff"}.contains(settings.imageFormat))
            invalid("Unsupported codec or image format");
        QString exportError;
        if (!validateExportSettings(m_session->project(), settings, &exportError))
            invalid(exportError);
        const auto jobId = newId();
        m_export = {
            {"jobId", jobId}, {"state", "running"}, {"progress", 0}, {"path", settings.path}};
        m_session->exportAsync(settings);
        return m_export;
    }
    if (method == "export.status") {
        keys(p, {});
        return m_export;
    }
    if (method == "export.cancel") {
        keys(p, {});
        m_session->cancelExport();
        return true;
    }
    if (method == "maya.register") {
        validateMayaConfig(p, true);
        const auto id = str(p, "sessionId", true, 128);
        const auto name = str(p, "name", true, 256);
        integer(p, "pid", 0, 0, 1000000000);
        if (!c.mayaId.isEmpty() && c.mayaId != id)
            invalid("Only one Maya session per connection");
        if (m_maya.contains(id) && m_maya[id].value("connected").toBool() && c.mayaId != id)
            failed("Maya session ID is already connected");
        QJsonObject config{{"direction", "off"},
                           {"offset", 0},
                           {"multiplier", 1},
                           {"fromRangeStart", false},
                           {"mayaRangeStart", 1},
                           {"configRevision", 0},
                           {"timeoutSeconds", m_defaultMayaTimeout}};
        if (m_maya.contains(id))
            config = m_maya[id];
        else
            for (auto it = p.begin(); it != p.end(); ++it)
                config[it.key()] = it.value();
        config["sessionId"] = id;
        config["name"] = name;
        config["pid"] = p.value("pid");
        config["connected"] = true;
        config["lastSeen"] = QDateTime::currentMSecsSinceEpoch();
        m_mayaActivity[id] = m_monotonicClock();
        m_maya[id] = config;
        c.mayaId = id;
        emit clientsChanged();
        return status(id);
    }
    if (method == "maya.configure") {
        keys(p, {"sessionId", "config"});
        const auto id = str(p, "sessionId", true, 128);
        if (!p.value("config").isObject())
            invalid("config must be an object");
        validateMayaConfig(p.value("config").toObject());
        if (!configureMayaSession(id, p.value("config").toObject()))
            failed(m_lastError);
        return m_maya[id];
    }
    if (method == "maya.sessions") {
        keys(p, {});
        return mayaSessions();
    }
    if (method == "maya.unregister") {
        keys(p, {});
        if (!c.mayaId.isEmpty()) {
            m_maya.remove(c.mayaId);
            m_mayaActivity.remove(c.mayaId);
            c.mayaId.clear();
            emit clientsChanged();
        }
        return true;
    }
    throw RpcFailure{-32601, "Method not found"};
}
} // namespace openframe
