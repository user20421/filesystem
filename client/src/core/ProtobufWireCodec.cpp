#include "ProtobufWireCodec.h"

#include "envelope.qpb.h"

#include <QtProtobuf/QProtobufSerializer>

bool ProtobufWireCodec::encode(const WireMessage &message, QByteArray *frame, QString *error) const
{
    im::v1::Envelope envelope;
    envelope.setVersion(message.version);
    envelope.setType(static_cast<im::v1::Envelope::Type>(static_cast<int>(message.type)));
    envelope.setMessageId(message.messageId);
    envelope.setConversationId(message.conversationId);
    envelope.setSenderId(message.senderId);
    envelope.setRecipientId(message.recipientId);
    envelope.setDeviceId(message.deviceId);
    envelope.setTimestamp(message.timestamp);
    envelope.setPayload(message.payload);
    envelope.setCursor(message.cursor.toULongLong());
    envelope.setReadAt(message.readAt);

    QProtobufSerializer serializer;
    *frame = envelope.serialize(&serializer);
    if (serializer.lastError() != QAbstractProtobufSerializer::Error::None) {
        frame->clear();
        if (error)
            *error = QStringLiteral("Protobuf Envelope 序列化失败：") + serializer.lastErrorString();
        return false;
    }
    return true;
}

bool ProtobufWireCodec::decode(const QByteArray &frame, WireMessage *message, QString *error) const
{
    im::v1::Envelope envelope;
    QProtobufSerializer serializer;
    if (!envelope.deserialize(&serializer, frame)) {
        if (error) {
            *error = QStringLiteral("无法解析 Protobuf Envelope：") + serializer.lastErrorString();
        }
        return false;
    }
    message->version = envelope.version();
    message->type = static_cast<WireMessage::Type>(static_cast<int>(envelope.type()));
    message->messageId = envelope.messageId();
    message->conversationId = envelope.conversationId();
    message->senderId = envelope.senderId();
    message->recipientId = envelope.recipientId();
    message->deviceId = envelope.deviceId();
    message->timestamp = static_cast<qint64>(envelope.timestamp());
    message->payload = envelope.payload();
    message->cursor = envelope.cursor() == 0 ? QString{} : QString::number(envelope.cursor());
    message->readAt = static_cast<qint64>(envelope.readAt());
    return true;
}
