// 普通 REST endpoint 适配；JSON 字段解码统一委托 ApiJsonCodec。
#include "ApiClient.h"

#include "ApiJsonCodec.h"

#include <QJsonArray>
#include <QJsonDocument>

ApiRequestHandle *ApiClient::registerUser(const QString &username, const QString &password,
                                       AuthCallback callback)
{
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/register"),
                       {{QStringLiteral("username"), username}, {QStringLiteral("password"), password},
                        {QStringLiteral("display_name"), username}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = ApiJsonCodec::parseAuth(QJsonDocument::fromJson(body).object());
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
            session = ApiJsonCodec::parseAuth(QJsonDocument::fromJson(body).object());
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
            const QJsonObject root = ApiJsonCodec::payloadObject(document.object());
            array = root.value(QStringLiteral("users")).toArray();
        }
        for (const QJsonValue &value : array) {
            const User user = ApiJsonCodec::parseUser(value.toObject());
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
        const QJsonObject data = ApiJsonCodec::payloadObject(QJsonDocument::fromJson(body).object());
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
        const QString ticket = ApiJsonCodec::stringField(ApiJsonCodec::payloadObject(QJsonDocument::fromJson(body).object()),
                                           {"ticket", "ws_ticket"});
        callback(!ticket.isEmpty(), ticket,
                 ticket.isEmpty() ? QStringLiteral("响应缺少 ticket") : QString{});
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
        const FileInitInfo info = ApiJsonCodec::parseFileInit(QJsonDocument::fromJson(body).object());
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
        callback(true, ApiJsonCodec::parseFileStatus(QJsonDocument::fromJson(body).object()), {});
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
