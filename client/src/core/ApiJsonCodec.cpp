#include "ApiJsonCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace ApiJsonCodec {
namespace {
QJsonObject objectValue(const QJsonObject &object, const QString &key)
{
    return object.value(key).toObject();
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
} // namespace

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
                    qint64 fallback)
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

QString responseError(const QByteArray &body, const QString &fallback)
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
} // namespace ApiJsonCodec
