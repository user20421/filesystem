// 本地消息 JSON 编解码及 ACK/ERROR/恢复合并规则，字段 schema 保持不变。
#include "LocalStoreMessageCodec.h"

namespace LocalStoreMessageCodec {
QJsonObject messageToJson(const ChatMessage &message)
{
    return {{QStringLiteral("message_id"), message.id},
            {QStringLiteral("cursor"), message.cursor},
            {QStringLiteral("conversation_id"), message.conversationId},
            {QStringLiteral("sender_id"), message.senderId},
            {QStringLiteral("recipient_id"), message.recipientId},
            {QStringLiteral("type"), message.kind},
            {QStringLiteral("text"), message.content},
            {QStringLiteral("file_id"), message.fileId},
            {QStringLiteral("file_name"), message.fileName},
            {QStringLiteral("size"), message.fileSize},
            {QStringLiteral("timestamp"), message.timestamp},
            {QStringLiteral("read_at"), message.readAt},
            {QStringLiteral("pending"), message.pending},
            {QStringLiteral("acked"), message.acked},
            {QStringLiteral("failed"), message.failed},
            {QStringLiteral("attempt_count"), message.attemptCount},
            {QStringLiteral("next_attempt_at"), message.nextAttemptAt},
            {QStringLiteral("delivery_error"), message.deliveryError}};
}

ChatMessage messageFromJson(const QJsonObject &object)
{
    ChatMessage message;
    message.id = object.value(QStringLiteral("message_id")).toString();
    message.cursor = object.value(QStringLiteral("cursor")).toString();
    message.conversationId = object.value(QStringLiteral("conversation_id")).toString();
    message.senderId = object.value(QStringLiteral("sender_id")).toString();
    message.recipientId = object.value(QStringLiteral("recipient_id")).toString();
    message.kind = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    message.content = object.value(QStringLiteral("text")).toString();
    message.fileId = object.value(QStringLiteral("file_id")).toString();
    message.fileName = object.value(QStringLiteral("file_name")).toString();
    message.fileSize = object.value(QStringLiteral("size")).toVariant().toLongLong();
    message.timestamp = object.value(QStringLiteral("timestamp")).toVariant().toLongLong();
    message.readAt = object.value(QStringLiteral("read_at")).toVariant().toLongLong();
    message.pending = object.value(QStringLiteral("pending")).toBool(false);
    message.acked = object.value(QStringLiteral("acked")).toBool(false);
    message.failed = object.value(QStringLiteral("failed")).toBool(false);
    message.attemptCount = object.value(QStringLiteral("attempt_count")).toInt(0);
    message.nextAttemptAt = object.value(QStringLiteral("next_attempt_at")).toVariant().toLongLong();
    message.deliveryError = object.value(QStringLiteral("delivery_error")).toString();
    return message;
}

ChatMessage mergeMessage(const ChatMessage &existing, const ChatMessage &incoming)
{
    ChatMessage update = incoming;
    bool incomingCursorOk = false;
    bool existingCursorOk = false;
    const qulonglong incomingCursor = incoming.cursor.toULongLong(&incomingCursorOk);
    const qulonglong existingCursor = existing.cursor.toULongLong(&existingCursorOk);
    if (update.cursor.isEmpty()
        || (incomingCursorOk && existingCursorOk && existingCursor > incomingCursor))
        update.cursor = existing.cursor;
    if (update.conversationId.isEmpty())
        update.conversationId = existing.conversationId;
    if (update.senderId.isEmpty())
        update.senderId = existing.senderId;
    if (update.recipientId.isEmpty())
        update.recipientId = existing.recipientId;
    if (update.kind.isEmpty()
        || (existing.isFile() && incoming.fileId.isEmpty() && incoming.content.isEmpty()))
        update.kind = existing.kind;
    if (update.content.isEmpty())
        update.content = existing.content;
    if (update.fileId.isEmpty())
        update.fileId = existing.fileId;
    if (update.fileName.isEmpty())
        update.fileName = existing.fileName;
    if (update.fileSize == 0)
        update.fileSize = existing.fileSize;
    if (update.timestamp == 0)
        update.timestamp = existing.timestamp;
    update.readAt = qMax(update.readAt, existing.readAt);
    update.attemptCount = qMax(update.attemptCount, existing.attemptCount);

    // ACK 终止传输重试；ERROR 在重启恢复、迟到 ACK 或权威历史覆盖前保持终态。
    if (existing.acked || incoming.acked) {
        update.acked = true;
        update.pending = false;
        update.failed = false;
        update.nextAttemptAt = 0;
        update.deliveryError.clear();
    } else if (existing.failed || incoming.failed) {
        update.acked = false;
        update.pending = false;
        update.failed = true;
        update.nextAttemptAt = 0;
        if (!incoming.failed || incoming.deliveryError.isEmpty())
            update.deliveryError = existing.deliveryError;
    } else {
        update.acked = false;
        update.failed = false;
        update.pending = existing.pending || incoming.pending;
        if (incoming.nextAttemptAt <= 0 || incoming.attemptCount < existing.attemptCount)
            update.nextAttemptAt = existing.nextAttemptAt;
        if (incoming.deliveryError.isEmpty())
            update.deliveryError = existing.deliveryError;
    }
    return update;
}
} // namespace LocalStoreMessageCodec
