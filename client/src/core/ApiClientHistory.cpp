// 消息历史与未读摘要 endpoint，保留 Protobuf envelope 和 legacy JSON 双分支。
#include "ApiClient.h"

#include "ApiJsonCodec.h"
#include "MessagePayloadCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

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
            array = ApiJsonCodec::payloadObject(document.object()).value(QStringLiteral("messages")).toArray();
        QList<ChatMessage> result;
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            ChatMessage message;
            const QString encoded = ApiJsonCodec::stringField(item, {"envelope"});
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
                // 兼容早期开发服务器的 legacy JSON 消息。
                message = ApiJsonCodec::parseMessage(item);
            }
            message.cursor = ApiJsonCodec::stringField(item, {"cursor"});
            if (message.readAt == 0)
                message.readAt = ApiJsonCodec::integerField(item, {"read_at"});
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
        const QJsonArray array = ApiJsonCodec::payloadObject(QJsonDocument::fromJson(body).object())
            .value(QStringLiteral("unread")).toArray();
        QList<UnreadSummary> result;
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            UnreadSummary summary;
            summary.peerId = ApiJsonCodec::stringField(item, {"peer_id"});
            summary.unreadCount = static_cast<int>(ApiJsonCodec::integerField(item, {"unread_count"}));
            summary.lastMessageAt = ApiJsonCodec::integerField(item, {"last_message_at"});
            if (!summary.peerId.isEmpty() && summary.unreadCount > 0)
                result.append(summary);
        }
        callback(true, result, {});
    });
}
