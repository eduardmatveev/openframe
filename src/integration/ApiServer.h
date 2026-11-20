#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <functional>
#include <QTimer>

class QTcpSocket;
namespace openframe {
class ReviewSession;

class ApiServer final : public QObject {
    Q_OBJECT
  public:
    explicit ApiServer(ReviewSession *session, QObject *parent = nullptr,
                       std::function<qint64()> monotonicClock = {});
    ~ApiServer() override;
    bool start(quint16 port = 0);
    void stop();
    bool isListening() const;
    quint16 listeningPort() const;
    QString endpointPath() const;
    QString lastError() const;
    QJsonArray mayaSessions() const;
    bool configureMayaSession(const QString &sessionId, const QJsonObject &config);
    double defaultMayaTimeout() const;
    bool setDefaultMayaTimeout(double seconds);
    static QString defaultEndpointDirectory();
    static constexpr qsizetype MaximumRequestBytes = 64 * 1024;

  signals:
    void started();
    void stopped();
    void error(const QString &message);
    void clientsChanged();
    void activateRequested();

  private:
    struct Connection {
        QByteArray pending;
        qint64 activityMs = 0;
        QElapsedTimer rateWindow;
        int requests = 0;
        bool authenticated = false;
        QString mayaId;
    };
    ReviewSession *m_session;
    QTcpServer m_server;
    QTimer m_housekeeping;
    QHash<QTcpSocket *, Connection *> m_connections;
    QHash<QString, QJsonObject> m_maya;
    QHash<QString, qint64> m_mayaActivity;
    std::function<qint64()> m_monotonicClock;
    double m_defaultMayaTimeout = .75;
    QString m_endpointPath;
    QString m_lastError;
    QByteArray m_token;
    QString m_instanceId;
    QString m_currentOrigin;
    QString m_frameOrigin;
    quint64 m_revision = 0;
    quint64 m_frameRevision = 0;
    QJsonObject m_export;

    void acceptConnections();
    void readRequests(QTcpSocket *socket);
    void processLine(QTcpSocket *socket, const QByteArray &line);
    void reply(QTcpSocket *socket, const QJsonObject &object);
    void reject(QTcpSocket *socket, const QJsonValue &id, int code, const QString &message,
                bool close = false);
    QJsonValue dispatch(const QString &method, const QJsonObject &params, Connection &connection);
    QJsonObject status(const QString &mayaId = {}) const;
    bool writeEndpoint();
    void recordError(const QString &message);
};
} // namespace openframe
