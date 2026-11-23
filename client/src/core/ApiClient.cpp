#include "ApiClient.h"

#include "LocalStore.h"
#include "MessagePayloadCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

namespace {
QJsonObject objectValue(const QJsonObject &object, const QString &key)
{
    return object.value(key).toObject();
}

// Accept an optional conventional {"data": ...} envelope in one place.
QJsonObject payloadObject(const QJsonObject &root)
{
    const QJsonValue data = root.value(QStringLiteral("data"));
    return data.isObject() ? data.toObject() : root;
}

QString stringField(const QJsonObject &object, std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QJsonValue value = object.value(QLatin1String(name));
        if (value.isString())
            return value.toString();
        if (value.isDouble())
            return QString::number(value.toVariant().toLongLong());
    }
    return {};
}

qint64 integerField(const QJsonObject &object, std::initializer_list<const char *> names,
                    qint64 fallback = 0)
{
    for (const char *name : names) {
        const QJsonValue value = object.value(QLatin1String(name));
        if (value.isDouble())
            return value.toVariant().toLongLong();
        if (value.isString()) {
            bool ok = false;
            const qint64 result = value.toString().toLongLong(&ok);
            if (ok)
                return result;
        }
    }
    return fallback;
}

User parseUser(QJsonObject object)
{
    object = payloadObject(object);
    if (object.value(QStringLiteral("user")).isObject())
        object = objectValue(object, QStringLiteral("user"));
    User user;
    user.id = stringField(object, {"id", "user_id"});
    user.username = stringField(object, {"username", "name"});
    user.displayName = stringField(object, {"display_name", "displayName", "nickname"});
    if (user.displayName.isEmpty())
        user.displayName = user.username;
    return user;
}

AuthSession parseAuth(const QJsonObject &root)
{
    const QJsonObject object = payloadObject(root);
    AuthSession session;
    const QJsonObject tokens = object.value(QStringLiteral("tokens")).isObject()
        ? objectValue(object, QStringLiteral("tokens")) : object;
    session.accessToken = stringField(tokens, {"access_token", "accessToken"});
    session.refreshToken = stringField(tokens, {"refresh_token", "refreshToken"});
    session.user = parseUser(object);
    return session;
}

QSet<int> parseUploadedChunks(const QJsonObject &object)
{
    QSet<int> result;
    QJsonValue chunks = object.value(QStringLiteral("uploaded_chunks"));
    if (!chunks.isArray())
        chunks = object.value(QStringLiteral("chunks"));
    const QJsonArray array = chunks.toArray();
    for (const QJsonValue &value : array) {
        if (value.isDouble()) {
            result.insert(value.toInt());
        } else if (value.isObject()) {
            const QJsonObject chunk = value.toObject();
            const bool uploaded = chunk.value(QStringLiteral("uploaded")).toBool(true)
                && chunk.value(QStringLiteral("complete")).toBool(true);
            if (uploaded)
                result.insert(static_cast<int>(integerField(chunk, {"index", "chunk_index"}, -1)));
        }
    }
    result.remove(-1);
    return result;
}

FileInitInfo parseFileInit(const QJsonObject &root)
{
    const QJsonObject data = payloadObject(root);
    const QJsonObject object = data.value(QStringLiteral("file")).isObject()
        ? objectValue(data, QStringLiteral("file")) : data;
    FileInitInfo info;
    info.fileId = stringField(object, {"file_id", "id"});
    info.chunkSize = static_cast<int>(integerField(object, {"chunk_size"}, DefaultFileChunkSize));
    info.uploadedChunks = parseUploadedChunks(data);
    if (info.uploadedChunks.isEmpty())
        info.uploadedChunks = parseUploadedChunks(object);
    info.status = stringField(object, {"status", "state"});
    info.complete = object.value(QStringLiteral("complete")).toBool(false)
        || object.value(QStringLiteral("completed")).toBool(false)
        || object.value(QStringLiteral("instant")).toBool(false)
        || info.status == QLatin1String("complete")
        || info.status == QLatin1String("completed")
        || info.status == QLatin1String("instant");
    return info;
}

FileStatus parseFileStatus(const QJsonObject &root)
{
    const QJsonObject data = payloadObject(root);
    const QJsonObject object = data.value(QStringLiteral("file")).isObject()
        ? objectValue(data, QStringLiteral("file")) : data;
    FileStatus status;
    status.fileId = stringField(object, {"file_id", "id"});
    status.fileName = stringField(object, {"original_name", "file_name", "filename", "name"});
    status.size = integerField(object, {"size", "file_size"});
    status.chunkSize = static_cast<int>(integerField(object, {"chunk_size"}, DefaultFileChunkSize));
    // uploaded_chunks is a sibling of file in the real status response.
    status.uploadedChunks = parseUploadedChunks(data);
    if (status.uploadedChunks.isEmpty())
        status.uploadedChunks = parseUploadedChunks(object);
    status.status = stringField(object, {"status", "state"});
    status.complete = object.value(QStringLiteral("complete")).toBool(false)
        || object.value(QStringLiteral("completed")).toBool(false)
        || status.status == QLatin1String("complete")
        || status.status == QLatin1String("completed");
    return status;
}

ChatMessage parseMessage(const QJsonObject &source)
{
    const QJsonObject object = payloadObject(source);
    ChatMessage message;
    message.id = stringField(object, {"message_id", "id"});
    message.conversationId = stringField(object, {"conversation_id"});
    message.senderId = stringField(object, {"sender_id", "from"});
    message.recipientId = stringField(object, {"recipient_id", "to", "peer_id"});
    message.kind = stringField(object, {"type", "kind"});
    message.content = stringField(object, {"text", "content"});
    message.fileId = stringField(object, {"file_id"});
    message.fileName = stringField(object, {"file_name", "filename"});
    message.fileSize = integerField(object, {"size", "file_size"});
    message.timestamp = integerField(object, {"timestamp", "created_at"});
    message.readAt = integerField(object, {"read_at"});

    QJsonObject payload = objectValue(object, QStringLiteral("payload"));
    if (payload.isEmpty() && object.value(QStringLiteral("payload")).isString()) {
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(
            object.value(QStringLiteral("payload")).toString().toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            payload = document.object();
    }
    if (!payload.isEmpty()) {
        if (message.kind.isEmpty())
            message.kind = stringField(payload, {"type", "kind"});
        if (message.content.isEmpty())
            message.content = stringField(payload, {"text", "content"});
        if (message.fileId.isEmpty())
            message.fileId = stringField(payload, {"file_id"});
        if (message.fileName.isEmpty())
            message.fileName = stringField(payload, {"file_name", "filename"});
        if (message.fileSize == 0)
            message.fileSize = integerField(payload, {"size", "file_size"});
    }
    if (message.kind.isEmpty())
        message.kind = QStringLiteral("text");
    return message;
}
} // namespace

ApiRequestHandle::ApiRequestHandle(ApiClient *client)
    : QObject(client), m_client(client) {}

void ApiRequestHandle::abort()
{
    if (!m_completed && !m_cancelled && m_client)
        m_client->cancelRequest(this, true);
}

QByteArray ApiRequestHandle::readAll()
{
    if (!m_reply)
        return {};
    const QByteArray data = m_reply->readAll();
    m_client->scheduleStreamingFinish(this);
    return data;
}

QByteArray ApiRequestHandle::read(qint64 maxSize)
{
    if (!m_reply || maxSize <= 0)
        return {};
    const QByteArray data = m_reply->read(maxSize);
    m_client->scheduleStreamingFinish(this);
    return data;
}

qint64 ApiRequestHandle::bytesAvailable() const
{
    return m_reply ? m_reply->bytesAvailable() : 0;
}

void ApiRequestHandle::setReadPaused(bool paused)
{
    if (m_completed || m_cancelled)
        return;
    m_readPaused = paused;
    if (!paused)
        m_client->scheduleStreamingFinish(this);
}

ApiClient::ApiClient(QObject *parent)
    : QObject(parent), m_codec(createDefaultWireCodec()) {}

ApiClient::~ApiClient()
{
    shutdown();
}

void ApiClient::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    const auto requests = m_requests;
    m_requests.clear();
    m_waitingForRefresh.clear();
    m_refreshing = false;
    m_refreshRequest.clear();
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (ApiRequestHandle *request = guarded.data()) {
            request->m_cancelled = true;
            if (QNetworkReply *reply = request->m_reply.data()) {
                // No completion/progress callback is allowed to re-enter a closing session.
                reply->disconnect();
                reply->abort();
                reply->deleteLater();
                request->m_reply.clear();
            }
            request->disconnect();
            request->deleteLater();
        }
    }
    m_manager.clearAccessCache();
}

void ApiClient::setAuthSession(const AuthSession &session)
{
    const bool unchanged = m_accessToken == session.accessToken
        && m_refreshToken == session.refreshToken && m_session.user.id == session.user.id;
    if (!unchanged)
        invalidateAuthentication();
    m_session = session;
    m_accessToken = session.accessToken;
    m_refreshToken = session.refreshToken;
}

void ApiClient::invalidateAuthentication()
{
    ++m_authEpoch;
    m_loggingOut = false;
    m_refreshing = false;
    m_refreshRequest.clear();
    const auto requests = m_requests;
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (guarded && !guarded->m_completed && !guarded->m_cancelled)
            cancelRequest(guarded.data(), true);
    }
    m_waitingForRefresh.clear();
}

void ApiClient::setAccessToken(const QString &token)
{
    m_accessToken = token;
    m_session.accessToken = token;
}

QUrl ApiClient::webSocketUrl(const QString &ticket) const
{
    QUrl url = m_baseUrl;
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                        : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/ws"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ticket"), ticket);
    url.setQuery(query);
    return url;
}

QString ApiClient::responseError(const QByteArray &body, const QString &fallback)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("error")).isObject()) {
            const QJsonObject apiError = objectValue(object, QStringLiteral("error"));
            const QString message = stringField(apiError, {"message", "code"});
            if (!message.isEmpty())
                return message;
        }
        const QString message = stringField(object, {"error", "message", "detail"});
        if (!message.isEmpty())
            return message;
    }
    return fallback;
}

ApiRequestHandle *ApiClient::request(const QString &method, const QString &path,
                                     const QByteArray &body, const QString &contentType,
                                     const QMap<QString, QString> &headers, RawCallback callback,
                                     bool streaming, int expectedStatus,
                                     bool allowRefresh, bool logoutBody)
{
    if (m_closing || (m_authFailureInProgress && allowRefresh)
        || (m_loggingOut && allowRefresh && !logoutBody))
        return nullptr;
    auto *handle = new ApiRequestHandle(this);
    handle->m_method = method;
    handle->m_path = path;
    handle->m_body = body;
    handle->m_contentType = contentType;
    handle->m_headers = headers;
    handle->m_callback = std::move(callback);
    handle->m_streaming = streaming;
    handle->m_expectedStatus = expectedStatus;
    handle->m_allowRefresh = allowRefresh;
    handle->m_logoutBody = logoutBody;
    handle->m_authEpoch = m_authEpoch;
    m_requests.append(handle);
    startRequest(handle);
    return handle;
}

void ApiClient::startRequest(ApiRequestHandle *handle)
{
    if (!handle || m_closing || handle->m_completed || handle->m_cancelled
        || handle->m_authEpoch != m_authEpoch)
        return;
    QNetworkRequest networkRequest(m_baseUrl.resolved(QUrl(handle->m_path)));
    if (handle->m_allowRefresh && !m_accessToken.isEmpty())
        networkRequest.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    if (!handle->m_contentType.isEmpty())
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, handle->m_contentType);
    for (auto it = handle->m_headers.cbegin(); it != handle->m_headers.cend(); ++it)
        networkRequest.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    // A logout replay must revoke the newly rotated token, not the consumed one.
    const QByteArray body = handle->m_logoutBody
        ? QJsonDocument(QJsonObject{{QStringLiteral("refresh_token"), m_refreshToken}})
            .toJson(QJsonDocument::Compact)
        : handle->m_body;
    QNetworkReply *reply = nullptr;
    if (handle->m_method == QLatin1String("GET"))
        reply = m_manager.get(networkRequest);
    else if (handle->m_method == QLatin1String("DELETE"))
        reply = m_manager.deleteResource(networkRequest);
    else
        reply = m_manager.sendCustomRequest(networkRequest, handle->m_method.toUtf8(), body);
    handle->m_reply = reply;
    if (handle->m_streaming)
        reply->setReadBufferSize(256 * 1024);

    connect(reply, &QNetworkReply::readyRead, handle, [handle, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (handle->m_reply == reply && handle->m_streaming && !handle->m_cancelled
            && !handle->m_readPaused && status >= 200 && status < 300)
            emit handle->readyRead();
    });
    connect(reply, &QNetworkReply::downloadProgress, handle,
            [handle, reply](qint64 received, qint64 total) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (handle->m_reply == reply && handle->m_streaming && !handle->m_cancelled
            && status >= 200 && status < 300)
            emit handle->downloadProgress(received, total);
    });
    QPointer<ApiRequestHandle> guarded(handle);
    connect(reply, &QNetworkReply::finished, this, [this, guarded, reply] {
        if (!guarded || guarded->m_reply != reply) {
            reply->deleteLater();
            return;
        }
        ApiRequestHandle *handle = guarded.data();
        if (m_closing) {
            handle->m_reply.clear();
            reply->deleteLater();
            return;
        }
        finishReply(handle, reply);
    });
}

void ApiClient::finishReply(ApiRequestHandle *handle, QNetworkReply *reply)
{
    if (!handle || !reply || handle->m_completed || handle->m_reply != reply)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool httpOk = status >= 200 && status < 300
        && (handle->m_expectedStatus == 0 || status == handle->m_expectedStatus);
    const bool ok = reply->error() == QNetworkReply::NoError && httpOk;
    if (ok && handle->m_streaming) {
        handle->m_networkFinished = true;
        if (handle->m_readPaused || reply->bytesAvailable() > 0) {
            // Keep the stable handle and final buffered bytes alive until the
            // consumer's bounded writer queue has accepted every byte.
            if (!handle->m_readPaused && reply->bytesAvailable() > 0) {
                QPointer<ApiRequestHandle> guarded(handle);
                QTimer::singleShot(0, handle, [guarded] {
                    if (guarded && !guarded->m_readPaused && !guarded->m_completed)
                        emit guarded->readyRead();
                });
            }
            return;
        }
    }

    // Streaming callers drain readyRead and final bytes themselves. Error
    // bodies are consumed here and are never emitted as downloaded data.
    const QByteArray response = reply->error() == QNetworkReply::OperationCanceledError
        || (handle->m_streaming && reply->error() == QNetworkReply::NoError)
        ? QByteArray{} : reply->readAll();
    QString error;
    if (!ok) {
        const QString fallback = reply->error() == QNetworkReply::NoError
                || reply->errorString().isEmpty()
            ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
        error = responseError(response, fallback);
    }

    if (status == 401 && handle->m_allowRefresh && !handle->m_cancelled
        && handle->m_authEpoch == m_authEpoch
        && handle->m_retryCount == 0 && !m_refreshToken.isEmpty()) {
        ++handle->m_retryCount;
        handle->m_reply.clear();
        reply->deleteLater();
        if (!m_waitingForRefresh.contains(handle))
            m_waitingForRefresh.append(handle);
        beginRefresh();
        return;
    }

    const bool rejectedAfterRetry = status == 401 && handle->m_allowRefresh
        && !handle->m_cancelled && handle->m_authEpoch == m_authEpoch && handle->m_retryCount > 0;
    const bool noRefreshAvailable = status == 401 && handle->m_allowRefresh
        && !handle->m_cancelled && handle->m_authEpoch == m_authEpoch
        && m_refreshToken.isEmpty() && !m_session.user.id.isEmpty();
    finishRequest(handle, ok && !handle->m_cancelled, response,
                  handle->m_cancelled ? QStringLiteral("请求已取消") : error);
    // A refreshed request is replayed exactly once. A second 401 invalidates
    // the session instead of starting another refresh cycle.
    if (rejectedAfterRetry || noRefreshAvailable)
        failAuthentication(error);
}

void ApiClient::scheduleStreamingFinish(ApiRequestHandle *handle)
{
    if (!handle || !handle->m_streaming || !handle->m_networkFinished
        || handle->m_readPaused || handle->m_completed || handle->m_cancelled)
        return;
    QPointer<ApiRequestHandle> guarded(handle);
    QTimer::singleShot(0, handle, [this, guarded] {
        if (!guarded || guarded->m_completed || guarded->m_cancelled
            || guarded->m_readPaused || !guarded->m_networkFinished || !guarded->m_reply)
            return;
        if (guarded->m_reply->bytesAvailable() == 0)
            finishReply(guarded.data(), guarded->m_reply.data());
    });
}

void ApiClient::finishRequest(ApiRequestHandle *handle, bool ok, const QByteArray &body,
                              const QString &error)
{
    if (!handle || handle->m_completed)
        return;
    handle->m_completed = true;
    m_requests.removeAll(handle);
    m_waitingForRefresh.removeAll(handle);
    QPointer<ApiRequestHandle> guarded(handle);
    QPointer<QNetworkReply> reply(handle->m_reply);
    handle->m_callback(ok, body, error);
    if (guarded)
        guarded->deleteLater();
    if (reply)
        reply->deleteLater();
}

void ApiClient::cancelRequest(ApiRequestHandle *handle, bool deliverCallback)
{
    if (!handle || handle->m_completed || handle->m_cancelled)
        return;
    handle->m_cancelled = true;
    m_waitingForRefresh.removeAll(handle);
    if (!handle->m_reply) {
        if (deliverCallback && !m_closing)
            finishRequest(handle, false, {}, QStringLiteral("请求已取消"));
        return;
    }
    if (handle->m_networkFinished) {
        QPointer<QNetworkReply> reply = handle->m_reply;
        handle->m_reply.clear();
        if (reply) {
            reply->disconnect();
            reply->deleteLater();
        }
        if (deliverCallback && !m_closing)
            finishRequest(handle, false, {}, QStringLiteral("请求已取消"));
        return;
    }
    handle->m_reply->abort();
    if (!deliverCallback && handle->m_reply)
        handle->m_reply->disconnect();
}

// beginRefresh 让同一认证 epoch 的并发 401 共享一次刷新，避免多个请求同时轮换 Refresh Token。
void ApiClient::beginRefresh()
{
    if (m_refreshing || m_closing)
        return;
    if (m_refreshToken.isEmpty()) {
        failAuthentication(QStringLiteral("登录会话已过期，请重新登录"));
        return;
    }
    m_refreshing = true;
    // refresh 捕获当前 epoch；显式登录或退出推进 epoch 后，迟到结果不能再覆盖新会话。
    const quint64 epoch = m_authEpoch;
    m_refreshRequest = startRefresh(m_refreshToken, [this, epoch](bool ok, const AuthSession &session,
                                                                 const QString &error) {
        completeRefresh(ok, session, error, epoch);
    }, true);
    if (!m_refreshRequest) {
        m_refreshing = false;
        failAuthentication(QStringLiteral("无法创建登录刷新请求"));
    }
}

// completeRefresh 只接受当前 epoch 的结果，随后原子保存双令牌并允许原请求重放一次；退出或重新登录后的迟到结果必须丢弃。
void ApiClient::completeRefresh(bool ok, const AuthSession &session, const QString &error,
                                quint64 epoch)
{
    if (m_closing || epoch != m_authEpoch)
        return;
    m_refreshRequest.clear();
    m_refreshing = false;
    QString failure = error;
    if (ok && m_sessionStore) {
        QString saveError;
        if (!m_sessionStore->saveSession(session, &saveError))
            failure = QStringLiteral("无法保存刷新后的会话：") + saveError;
    }
    if (!ok || !failure.isEmpty()) {
        failAuthentication(failure.isEmpty() ? QStringLiteral("登录会话刷新失败") : failure);
        return;
    }

    // Do not invalidate requests waiting on this same-epoch refresh. A later
    // explicit login/logout changes the epoch and makes late callbacks inert.
    m_session = session;
    m_accessToken = session.accessToken;
    m_refreshToken = session.refreshToken;
    emit sessionRefreshed(session);
    const auto waiting = m_waitingForRefresh;
    m_waitingForRefresh.clear();
    for (const QPointer<ApiRequestHandle> &guarded : waiting) {
        if (guarded && !guarded->m_cancelled && !guarded->m_completed)
            startRequest(guarded.data());
    }
}

// failAuthentication 使当前 epoch 的请求失效并返回登录页，防止失败回调递归或旧请求重新写回会话。
void ApiClient::failAuthentication(const QString &error)
{
    if (m_authFailureInProgress || m_closing)
        return;
    m_authFailureInProgress = true;
    m_refreshing = false;
    m_refreshRequest.clear();
    m_session = {};
    m_accessToken.clear();
    m_refreshToken.clear();
    if (m_sessionStore)
        m_sessionStore->clearSession();

    const auto waiting = m_waitingForRefresh;
    m_waitingForRefresh.clear();
    const QString message = error.isEmpty() ? QStringLiteral("登录会话已失效，请重新登录") : error;
    for (const QPointer<ApiRequestHandle> &guarded : waiting) {
        if (guarded && !guarded->m_completed)
            finishRequest(guarded.data(), false, {}, message);
    }
    m_authFailureInProgress = false;
    emit authenticationFailed(message);
}

ApiRequestHandle *ApiClient::jsonRequest(const QString &method, const QString &path,
                                         const QJsonObject &json, RawCallback callback,
                                         bool allowRefresh)
{
    return request(method, path, QJsonDocument(json).toJson(QJsonDocument::Compact),
                   QStringLiteral("application/json"), {}, std::move(callback),
                   false, 0, allowRefresh);
}

ApiRequestHandle *ApiClient::registerUser(const QString &username, const QString &password,
                                       AuthCallback callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/register"),
                       {{QStringLiteral("username"), username}, {QStringLiteral("password"), password},
                        {QStringLiteral("display_name"), username}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = parseAuth(QJsonDocument::fromJson(body).object());
            if (session.accessToken.isEmpty())
                callback(false, {}, QStringLiteral("登录响应缺少 access_token"));
            else if (session.user.id.isEmpty())
                callback(false, {}, QStringLiteral("登录响应缺少用户 id"));
            else
                callback(true, session, {});
        } else {
            callback(false, {}, error);
        }
    }, false);
}

ApiRequestHandle *ApiClient::login(const QString &username, const QString &password,
                                AuthCallback callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/login"),
                       {{QStringLiteral("username"), username}, {QStringLiteral("password"), password}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = parseAuth(QJsonDocument::fromJson(body).object());
            if (session.accessToken.isEmpty())
                callback(false, {}, QStringLiteral("登录响应缺少 access_token"));
            else if (session.user.id.isEmpty())
                callback(false, {}, QStringLiteral("登录响应缺少用户 id"));
            else
                callback(true, session, {});
        } else {
            callback(false, {}, error);
        }
    }, false);
}

ApiRequestHandle *ApiClient::refresh(const QString &refreshToken, AuthCallback callback)
{
    return startRefresh(refreshToken, std::move(callback), false);
}

ApiRequestHandle *ApiClient::startRefresh(const QString &refreshToken, AuthCallback callback,
                                          bool internal)
{
    if (m_loggingOut && !internal)
        return nullptr;
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/refresh"),
                       {{QStringLiteral("refresh_token"), refreshToken}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = parseAuth(QJsonDocument::fromJson(body).object());
            if (session.accessToken.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少 access_token"));
            else if (session.refreshToken.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少 refresh_token"));
            else if (session.user.id.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少用户 id"));
            else
                callback(true, session, {});
        } else {
            callback(false, {}, error);
        }
    }, false);
}

// logout 先使旧刷新和等待请求失效，再撤销当前 Refresh Token；若自身遇 401，重放时必须使用轮换后的新令牌。
ApiRequestHandle *ApiClient::logout(OperationCallback callback)
{
    if (m_closing || m_loggingOut)
        return nullptr;

    // Logout is an authentication boundary, not an ordinary queued request.
    // Invalidate every older request (especially independent refreshes and
    // requests waiting for one) before creating the revocation request.
    m_loggingOut = true;
    ++m_authEpoch;
    m_refreshing = false;
    m_refreshRequest.clear();
    const auto requests = m_requests;
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (guarded && !guarded->m_completed && !guarded->m_cancelled)
            cancelRequest(guarded.data(), true);
    }
    m_waitingForRefresh.clear();

    const quint64 epoch = m_authEpoch;
    ApiRequestHandle *handle = request(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/logout"), {},
                   QStringLiteral("application/json"), {},
                   [this, epoch, callback](bool ok, const QByteArray &, const QString &error) {
        if (epoch == m_authEpoch && !ok)
            m_loggingOut = false;
        callback(ok, error);
    }, false, 204, true, true);
    if (!handle)
        m_loggingOut = false;
    return handle;
}

ApiRequestHandle *ApiClient::users(UsersCallback callback)
{
    return jsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/users"), {},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        QList<User> result;
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(body);
        QJsonArray array;
        if (document.isArray())
            array = document.array();
        else {
            const QJsonObject root = payloadObject(document.object());
            array = root.value(QStringLiteral("users")).toArray();
        }
        for (const QJsonValue &value : array) {
            const User user = parseUser(value.toObject());
            if (!user.id.isEmpty())
                result.append(user);
        }
        callback(true, result, {});
    });
}

ApiRequestHandle *ApiClient::presence(PresenceCallback callback)
{
    return jsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/presence"), {},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const QJsonObject data = payloadObject(QJsonDocument::fromJson(body).object());
        QSet<QString> online;
        const QJsonArray ids = data.value(QStringLiteral("online_user_ids")).toArray();
        for (const QJsonValue &value : ids) {
            const QString id = value.isString() ? value.toString()
                                                : QString::number(value.toVariant().toLongLong());
            if (!id.isEmpty())
                online.insert(id);
        }
        callback(true, online, {});
    });
}

ApiRequestHandle *ApiClient::markRead(const QString &peerId, const QString &cursor,
                                   std::function<void(bool, QString)> callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/messages/read"),
                       {{QStringLiteral("peer_id"), peerId},
                        {QStringLiteral("cursor"), cursor.toLongLong()}},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

ApiRequestHandle *ApiClient::wsTicket(TicketCallback callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/ws-ticket"), {},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const QString ticket = stringField(payloadObject(QJsonDocument::fromJson(body).object()),
                                           {"ticket", "ws_ticket"});
        callback(!ticket.isEmpty(), ticket,
                 ticket.isEmpty() ? QStringLiteral("响应缺少 ticket") : QString{});
    });
}

ApiRequestHandle *ApiClient::messages(const QString &peerId, const QString &afterId,
                                   MessagesCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("peer_id"), peerId);
    if (!afterId.isEmpty())
        query.addQueryItem(QStringLiteral("after_id"), afterId);
    const QString path = QStringLiteral("/api/v1/messages?") + query.toString(QUrl::FullyEncoded);
    return jsonRequest(QStringLiteral("GET"), path, {},
                       [this, callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(body);
        QJsonArray array;
        if (document.isArray())
            array = document.array();
        else
            array = payloadObject(document.object()).value(QStringLiteral("messages")).toArray();
        QList<ChatMessage> result;
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            ChatMessage message;
            const QString encoded = stringField(item, {"envelope"});
            if (!encoded.isEmpty()) {
                WireMessage wire;
                QString codecError;
                if (!m_codec->decode(QByteArray::fromBase64(encoded.toUtf8()), &wire, &codecError)) {
                    callback(false, {}, codecError);
                    return;
                }
                if (!MessagePayloadCodec::decode(wire.payload, &message, &codecError)) {
                    callback(false, {}, codecError);
                    return;
                }
                message.id = wire.messageId;
                message.conversationId = wire.conversationId;
                message.senderId = wire.senderId;
                message.recipientId = wire.recipientId;
                message.timestamp = wire.timestamp;
                message.readAt = wire.readAt;
            } else {
                // Backward-compatible adapter for early development servers.
                message = parseMessage(item);
            }
            message.cursor = stringField(item, {"cursor"});
            if (message.readAt == 0)
                message.readAt = integerField(item, {"read_at"});
            result.append(message);
        }
        callback(true, result, {});
    });
}

ApiRequestHandle *ApiClient::unread(UnreadCallback callback)
{
    return jsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/messages/unread"), {},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const QJsonArray array = payloadObject(QJsonDocument::fromJson(body).object())
            .value(QStringLiteral("unread")).toArray();
        QList<UnreadSummary> result;
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            UnreadSummary summary;
            summary.peerId = stringField(item, {"peer_id"});
            summary.unreadCount = static_cast<int>(integerField(item, {"unread_count"}));
            summary.lastMessageAt = integerField(item, {"last_message_at"});
            if (!summary.peerId.isEmpty() && summary.unreadCount > 0)
                result.append(summary);
        }
        callback(true, result, {});
    });
}

ApiRequestHandle *ApiClient::initFile(const QString &peerId, const QString &fileName, qint64 size,
                                   const QString &sha256, FileInitCallback callback)
{
    const QJsonObject json{{QStringLiteral("recipient_id"), peerId},
                           {QStringLiteral("filename"), fileName},
                           {QStringLiteral("size"), size},
                           {QStringLiteral("sha256"), sha256},
                           {QStringLiteral("chunk_size"), DefaultFileChunkSize}};
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/files/init"), json,
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        const FileInitInfo info = parseFileInit(QJsonDocument::fromJson(body).object());
        callback(!info.fileId.isEmpty(), info,
                 info.fileId.isEmpty() ? QStringLiteral("响应缺少 file_id") : QString{});
    });
}

ApiRequestHandle *ApiClient::fileStatus(const QString &fileId, FileStatusCallback callback)
{
    return jsonRequest(QStringLiteral("GET"), QStringLiteral("/api/v1/files/") + fileId, {},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        if (!ok) {
            callback(false, {}, error);
            return;
        }
        callback(true, parseFileStatus(QJsonDocument::fromJson(body).object()), {});
    });
}

ApiRequestHandle *ApiClient::putChunk(const QString &fileId, int index, const QByteArray &data,
                                   const QString &sha256, OperationCallback callback)
{
    return request(QStringLiteral("PUT"),
                   QStringLiteral("/api/v1/files/%1/chunks/%2").arg(fileId).arg(index), data,
                   QStringLiteral("application/octet-stream"),
                   {{QStringLiteral("X-Chunk-SHA256"), sha256}},
                   [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

ApiRequestHandle *ApiClient::completeFile(const QString &fileId, OperationCallback callback)
{
    return jsonRequest(QStringLiteral("POST"),
                       QStringLiteral("/api/v1/files/") + fileId + QStringLiteral("/complete"), {},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

ApiRequestHandle *ApiClient::downloadFile(const QString &fileId, OperationCallback callback,
                                       ProgressCallback progress)
{
    ApiRequestHandle *handle = request(QStringLiteral("GET"),
        QStringLiteral("/api/v1/files/") + fileId + QStringLiteral("/download"), {}, {}, {},
        [callback](bool ok, const QByteArray &, const QString &error) { callback(ok, error); }, true);
    if (handle && progress)
        connect(handle, &ApiRequestHandle::downloadProgress, this, std::move(progress));
    return handle;
}

ApiRequestHandle *ApiClient::deleteFile(const QString &fileId, OperationCallback callback)
{
    return jsonRequest(QStringLiteral("DELETE"), QStringLiteral("/api/v1/files/") + fileId, {},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

ApiRequestHandle *ApiClient::deleteCurrentAccount(OperationCallback callback)
{
    return request(QStringLiteral("DELETE"), QStringLiteral("/api/v1/users/me"), {}, {}, {},
                   [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    }, false, 204);
}
