#pragma once

#include "ApiTypes.h"
#include "WireCodec.h"

#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <memory>

// The only module that knows REST paths and JSON response field names.
class ApiClient final : public QObject {
    Q_OBJECT
public:
    using AuthCallback = std::function<void(bool, AuthSession, QString)>;
    using UsersCallback = std::function<void(bool, QList<User>, QString)>;
    using PresenceCallback = std::function<void(bool, QSet<QString>, QString)>;
    using MessagesCallback = std::function<void(bool, QList<ChatMessage>, QString)>;
    using TicketCallback = std::function<void(bool, QString, QString)>;
    using FileInitCallback = std::function<void(bool, FileInitInfo, QString)>;
    using FileStatusCallback = std::function<void(bool, FileStatus, QString)>;
    using OperationCallback = std::function<void(bool, QString)>;
    using ProgressCallback = std::function<void(qint64, qint64)>;

    explicit ApiClient(QObject *parent = nullptr);
    ~ApiClient() override;

    // Idempotently aborts all outstanding HTTP work without delivering UI callbacks.
    void shutdown();
    bool isClosing() const { return m_closing; }
    int pendingRequestCount() const { return static_cast<int>(m_replies.size()); }

    void setServerUrl(const QUrl &url) { m_baseUrl = url; }
    QUrl serverUrl() const { return m_baseUrl; }
    QUrl webSocketUrl(const QString &ticket) const;

    void setAccessToken(const QString &token) { m_accessToken = token; }
    QString accessToken() const { return m_accessToken; }

    QNetworkReply *registerUser(const QString &username, const QString &password,
                                AuthCallback callback);
    QNetworkReply *login(const QString &username, const QString &password,
                         AuthCallback callback);
    QNetworkReply *refresh(const QString &refreshToken, AuthCallback callback);
    QNetworkReply *users(UsersCallback callback);
    QNetworkReply *presence(PresenceCallback callback);
    QNetworkReply *markRead(const QString &peerId, const QString &cursor,
                            std::function<void(bool, QString)> callback);
    QNetworkReply *wsTicket(TicketCallback callback);
    QNetworkReply *messages(const QString &peerId, const QString &afterId,
                            MessagesCallback callback);

    QNetworkReply *initFile(const QString &peerId, const QString &fileName, qint64 size,
                            const QString &sha256, FileInitCallback callback);
    QNetworkReply *fileStatus(const QString &fileId, FileStatusCallback callback);
    QNetworkReply *putChunk(const QString &fileId, int index, const QByteArray &data,
                            const QString &sha256, OperationCallback callback);
    QNetworkReply *completeFile(const QString &fileId, OperationCallback callback);
    // The caller consumes readyRead; successful bodies are never buffered by
    // ApiClient.  Any final buffered bytes remain available in the callback.
    QNetworkReply *downloadFile(const QString &fileId, OperationCallback callback,
                                ProgressCallback progress = {});
    QNetworkReply *deleteFile(const QString &fileId, OperationCallback callback);
    QNetworkReply *deleteCurrentAccount(OperationCallback callback);

private:
    using RawCallback = std::function<void(bool, QByteArray, QString)>;
    QNetworkReply *request(const QString &method, const QString &path,
                           const QByteArray &body, const QString &contentType,
                           const QMap<QString, QString> &headers, RawCallback callback,
                           bool streaming = false, int expectedStatus = 0);
    QNetworkReply *jsonRequest(const QString &method, const QString &path,
                               const QJsonObject &json, RawCallback callback);
    static QString responseError(const QByteArray &body, const QString &fallback);

    QNetworkAccessManager m_manager;
    QUrl m_baseUrl{QStringLiteral("http://127.0.0.1:8080")};
    QString m_accessToken;
    std::unique_ptr<WireCodec> m_codec;
    QList<QPointer<QNetworkReply>> m_replies;
    bool m_closing = false;
};
