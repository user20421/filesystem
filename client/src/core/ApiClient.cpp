#include "ApiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkRequest>
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
    info.chunkSize = static_cast<int>(integerField(object, {"chunk_size"}, 4 * 1024 * 1024));
    info.uploadedChunks = parseUploadedChunks(data);
    if (info.uploadedChunks.isEmpty())
        info.uploadedChunks = parseUploadedChunks(object);
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
    status.chunkSize = static_cast<int>(integerField(object, {"chunk_size"}, 4 * 1024 * 1024));
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
    const auto replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply> &guarded : replies) {
        if (QNetworkReply *reply = guarded.data()) {
            // No completion/progress callback is allowed to re-enter a closing session.
            reply->disconnect();
            reply->abort();
            reply->deleteLater();
        }
    }
    m_manager.clearAccessCache();
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

QNetworkReply *ApiClient::request(const QString &method, const QString &path,
                                  const QByteArray &body, const QString &contentType,
                                  const QMap<QString, QString> &headers, RawCallback callback,
                                  bool streaming, int expectedStatus)
{
    if (m_closing)
        return nullptr;
    QNetworkRequest request(m_baseUrl.resolved(QUrl(path)));
    if (!m_accessToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    if (!contentType.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    QNetworkReply *reply = nullptr;
    if (method == QLatin1String("GET"))
        reply = m_manager.get(request);
    else if (method == QLatin1String("DELETE"))
        reply = m_manager.deleteResource(request);
    else
        reply = m_manager.sendCustomRequest(request, method.toUtf8(), body);

    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, callback, streaming, expectedStatus]() {
        m_replies.removeAll(reply);
        if (m_closing) {
            reply->deleteLater();
            return;
        }
        // Streaming callers drain readyRead and any final bytes themselves.
        // Error bodies are still read here for centralized error adaptation.
        const QByteArray response = streaming && reply->error() == QNetworkReply::NoError
            ? QByteArray{} : reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = status >= 200 && status < 300
            && (expectedStatus == 0 || status == expectedStatus);
        const bool ok = reply->error() == QNetworkReply::NoError && httpOk;
        QString error;
        if (!ok) {
            const QString fallback = reply->error() == QNetworkReply::NoError
                    || reply->errorString().isEmpty()
                ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
            error = responseError(response, fallback);
        }
        QPointer<QNetworkReply> guardedReply(reply);
        callback(ok, response, error);
        // A completion callback may close its session and destroy the reply.
        if (guardedReply)
            guardedReply->deleteLater();
    });
    return reply;
}

QNetworkReply *ApiClient::jsonRequest(const QString &method, const QString &path,
                                      const QJsonObject &json, RawCallback callback)
{
    return request(method, path, QJsonDocument(json).toJson(QJsonDocument::Compact),
                   QStringLiteral("application/json"), {}, std::move(callback));
}

QNetworkReply *ApiClient::registerUser(const QString &username, const QString &password,
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
    });
}

QNetworkReply *ApiClient::login(const QString &username, const QString &password,
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
    });
}

QNetworkReply *ApiClient::refresh(const QString &refreshToken, AuthCallback callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/refresh"),
                       {{QStringLiteral("refresh_token"), refreshToken}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = parseAuth(QJsonDocument::fromJson(body).object());
            if (session.accessToken.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少 access_token"));
            else if (session.user.id.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少用户 id"));
            else
                callback(true, session, {});
        } else {
            callback(false, {}, error);
        }
    });
}

QNetworkReply *ApiClient::users(UsersCallback callback)
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

QNetworkReply *ApiClient::presence(PresenceCallback callback)
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

QNetworkReply *ApiClient::markRead(const QString &peerId, const QString &cursor,
                                   std::function<void(bool, QString)> callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/messages/read"),
                       {{QStringLiteral("peer_id"), peerId},
                        {QStringLiteral("cursor"), cursor.toLongLong()}},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

QNetworkReply *ApiClient::wsTicket(TicketCallback callback)
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

QNetworkReply *ApiClient::messages(const QString &peerId, const QString &afterId,
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
                const QJsonDocument payload = QJsonDocument::fromJson(wire.payload);
                message = parseMessage(payload.isObject() ? payload.object()
                                                         : textPayload(QString::fromUtf8(wire.payload)));
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

QNetworkReply *ApiClient::initFile(const QString &peerId, const QString &fileName, qint64 size,
                                   const QString &sha256, FileInitCallback callback)
{
    const QJsonObject json{{QStringLiteral("recipient_id"), peerId},
                           {QStringLiteral("filename"), fileName},
                           {QStringLiteral("size"), size},
                           {QStringLiteral("sha256"), sha256},
                           {QStringLiteral("chunk_size"), 4 * 1024 * 1024}};
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

QNetworkReply *ApiClient::fileStatus(const QString &fileId, FileStatusCallback callback)
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

QNetworkReply *ApiClient::putChunk(const QString &fileId, int index, const QByteArray &data,
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

QNetworkReply *ApiClient::completeFile(const QString &fileId, OperationCallback callback)
{
    return jsonRequest(QStringLiteral("POST"),
                       QStringLiteral("/api/v1/files/") + fileId + QStringLiteral("/complete"), {},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

QNetworkReply *ApiClient::downloadFile(const QString &fileId, OperationCallback callback,
                                       ProgressCallback progress)
{
    QNetworkReply *reply = request(QStringLiteral("GET"),
        QStringLiteral("/api/v1/files/") + fileId + QStringLiteral("/download"), {}, {}, {},
        [callback](bool ok, const QByteArray &, const QString &error) { callback(ok, error); }, true);
    if (progress)
        connect(reply, &QNetworkReply::downloadProgress, this, std::move(progress));
    return reply;
}

QNetworkReply *ApiClient::deleteFile(const QString &fileId, OperationCallback callback)
{
    return jsonRequest(QStringLiteral("DELETE"), QStringLiteral("/api/v1/files/") + fileId, {},
                       [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    });
}

QNetworkReply *ApiClient::deleteCurrentAccount(OperationCallback callback)
{
    return request(QStringLiteral("DELETE"), QStringLiteral("/api/v1/users/me"), {}, {}, {},
                   [callback](bool ok, const QByteArray &, const QString &error) {
        callback(ok, error);
    }, false, 204);
}
