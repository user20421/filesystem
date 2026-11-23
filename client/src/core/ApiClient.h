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

class ApiClient;
class LocalStore;

// ApiRequestHandle 是稳定的逻辑请求句柄：刷新令牌后底层 QNetworkReply 可被替换，调用方仍持有同一对象。
// 取消、流式读取和背压都通过该句柄转发，避免重试后误操作旧连接。
class ApiRequestHandle final : public QObject {
    Q_OBJECT
public:
    void abort();
    QByteArray readAll();
    QByteArray read(qint64 maxSize);
    qint64 bytesAvailable() const;
    // Flow-control pause only; it does not cancel or complete the request.
    void setReadPaused(bool paused);

signals:
    void readyRead();
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);

private:
    friend class ApiClient;
    ApiRequestHandle(ApiClient *client);

    ApiClient *m_client;
    QPointer<QNetworkReply> m_reply;
    QString m_method;
    QString m_path;
    QByteArray m_body;
    QString m_contentType;
    QMap<QString, QString> m_headers;
    std::function<void(bool, QByteArray, QString)> m_callback;
    bool m_streaming = false;
    bool m_allowRefresh = true;
    bool m_logoutBody = false;
    bool m_cancelled = false;
    bool m_completed = false;
    bool m_readPaused = false;
    bool m_networkFinished = false;
    quint64 m_authEpoch = 0;
    int m_expectedStatus = 0;
    int m_retryCount = 0;
};

// ApiClient 集中管理 REST 路径、JSON 字段、认证状态和请求生命周期，UI 不直接拼接网络协议细节。
class ApiClient final : public QObject {
    Q_OBJECT
public:
    using AuthCallback = std::function<void(bool, AuthSession, QString)>;
    using UsersCallback = std::function<void(bool, QList<User>, QString)>;
    using PresenceCallback = std::function<void(bool, QSet<QString>, QString)>;
    using MessagesCallback = std::function<void(bool, QList<ChatMessage>, QString)>;
    using UnreadCallback = std::function<void(bool, QList<UnreadSummary>, QString)>;
    using TicketCallback = std::function<void(bool, QString, QString)>;
    using FileInitCallback = std::function<void(bool, FileInitInfo, QString)>;
    using FileStatusCallback = std::function<void(bool, FileStatus, QString)>;
    using OperationCallback = std::function<void(bool, QString)>;
    using ProgressCallback = std::function<void(qint64, qint64)>;
    using RawCallback = std::function<void(bool, QByteArray, QString)>;

    explicit ApiClient(QObject *parent = nullptr);
    ~ApiClient() override;

    // Idempotently aborts all outstanding HTTP work without delivering UI callbacks.
    void shutdown();
    bool isClosing() const { return m_closing; }
    int pendingRequestCount() const { return static_cast<int>(m_requests.size()); }

    void setServerUrl(const QUrl &url) { m_baseUrl = url; }
    QUrl serverUrl() const { return m_baseUrl; }
    QUrl webSocketUrl(const QString &ticket) const;

    void setSessionStore(LocalStore *store) { m_sessionStore = store; }
    void setAuthSession(const AuthSession &session);
    void setAccessToken(const QString &token);
    QString accessToken() const { return m_accessToken; }
    QString refreshToken() const { return m_refreshToken; }

    ApiRequestHandle *registerUser(const QString &username, const QString &password,
                                   AuthCallback callback);
    ApiRequestHandle *login(const QString &username, const QString &password,
                            AuthCallback callback);
    ApiRequestHandle *refresh(const QString &refreshToken, AuthCallback callback);
    ApiRequestHandle *logout(OperationCallback callback);
    ApiRequestHandle *users(UsersCallback callback);
    ApiRequestHandle *presence(PresenceCallback callback);
    ApiRequestHandle *markRead(const QString &peerId, const QString &cursor,
                               std::function<void(bool, QString)> callback);
    ApiRequestHandle *wsTicket(TicketCallback callback);
    ApiRequestHandle *messages(const QString &peerId, const QString &afterId,
                               MessagesCallback callback);
    ApiRequestHandle *unread(UnreadCallback callback);

    ApiRequestHandle *initFile(const QString &peerId, const QString &fileName, qint64 size,
                               const QString &sha256, FileInitCallback callback);
    ApiRequestHandle *fileStatus(const QString &fileId, FileStatusCallback callback);
    ApiRequestHandle *putChunk(const QString &fileId, int index, const QByteArray &data,
                               const QString &sha256, OperationCallback callback);
    ApiRequestHandle *completeFile(const QString &fileId, OperationCallback callback);
    // The caller consumes readyRead on the stable handle; successful bodies are
    // never buffered by ApiClient. Final buffered bytes remain available.
    ApiRequestHandle *downloadFile(const QString &fileId, OperationCallback callback,
                                   ProgressCallback progress = {});
    ApiRequestHandle *deleteFile(const QString &fileId, OperationCallback callback);
    ApiRequestHandle *deleteCurrentAccount(OperationCallback callback);

signals:
    void sessionRefreshed(const AuthSession &session);
    void authenticationFailed(const QString &error);

private:
    friend class ApiRequestHandle;
    ApiRequestHandle *request(const QString &method, const QString &path,
                              const QByteArray &body, const QString &contentType,
                              const QMap<QString, QString> &headers, RawCallback callback,
                              bool streaming = false, int expectedStatus = 0,
                              bool allowRefresh = true, bool logoutBody = false);
    ApiRequestHandle *jsonRequest(const QString &method, const QString &path,
                                  const QJsonObject &json, RawCallback callback,
                                  bool allowRefresh = true);
    void startRequest(ApiRequestHandle *request);
    void finishReply(ApiRequestHandle *request, QNetworkReply *reply);
    void scheduleStreamingFinish(ApiRequestHandle *request);
    void finishRequest(ApiRequestHandle *request, bool ok, const QByteArray &body,
                       const QString &error);
    void cancelRequest(ApiRequestHandle *request, bool deliverCallback);
    ApiRequestHandle *startRefresh(const QString &refreshToken, AuthCallback callback,
                                   bool internal);
    void invalidateAuthentication();
    void beginRefresh();
    void completeRefresh(bool ok, const AuthSession &session, const QString &error,
                         quint64 epoch);
    void failAuthentication(const QString &error);
    static QString responseError(const QByteArray &body, const QString &fallback);

    QNetworkAccessManager m_manager;
    QUrl m_baseUrl{QStringLiteral("http://127.0.0.1:8080")};
    QString m_accessToken;
    QString m_refreshToken;
    AuthSession m_session;
    LocalStore *m_sessionStore = nullptr;
    std::unique_ptr<WireCodec> m_codec;
    QList<QPointer<ApiRequestHandle>> m_requests;
    QList<QPointer<ApiRequestHandle>> m_waitingForRefresh;
    QPointer<ApiRequestHandle> m_refreshRequest;
    bool m_refreshing = false;
    bool m_authFailureInProgress = false;
    bool m_loggingOut = false;
    bool m_closing = false;
    // Explicit login/logout advances this fence so late refresh callbacks cannot revive old auth.
    quint64 m_authEpoch = 1;
};
