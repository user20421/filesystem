#include "ProtobufWireCodec.h"

#include "envelope.qpb.h"
#include "messages.qpb.h"

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
    envelope.setCursor(message.type == WireMessage::Type::Read ? message.cursor.toULongLong() : 0);
    envelope.setReadAt(message.readAt);

    QProtobufSerializer serializer;
    switch (message.type) {
    case WireMessage::Type::Message:
        envelope.setPayload(message.payload);
        break;
    case WireMessage::Type::Ack: {
        im::v1::AckPayload payload;
        payload.setCursor(message.cursor.toULongLong());
        envelope.setPayload(payload.serialize(&serializer));
        break;
    }
    case WireMessage::Type::Error: {
        im::v1::ErrorPayload payload;
        payload.setCode(message.errorCode);
        payload.setMessage(message.errorMessage);
        envelope.setPayload(payload.serialize(&serializer));
        break;
    }
    default:
        // READ, PING and PONG carry metadata in Envelope and have no payload.
        envelope.setPayload({});
        break;
    }

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
        if (error)
            *error = QStringLiteral("无法解析 Protobuf Envelope：") + serializer.lastErrorString();
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
    message->readAt = static_cast<qint64>(envelope.readAt());

    switch (message->type) {
    case WireMessage::Type::Ack: {
        im::v1::AckPayload payload;
        if (!payload.deserialize(&serializer, envelope.payload())) {
            if (error)
                *error = QStringLiteral("无法解析 AckPayload：") + serializer.lastErrorString();
            return false;
        }
        message->cursor = payload.cursor() == 0 ? QString{} : QString::number(payload.cursor());
        break;
    }
    case WireMessage::Type::Error: {
        im::v1::ErrorPayload payload;
        if (!payload.deserialize(&serializer, envelope.payload())) {
            if (error)
                *error = QStringLiteral("无法解析 ErrorPayload：") + serializer.lastErrorString();
            return false;
        }
        message->errorCode = payload.code();
        message->errorMessage = payload.message();
        break;
    }
    case WireMessage::Type::Read:
        message->cursor = envelope.cursor() == 0 ? QString{} : QString::number(envelope.cursor());
        break;
    default:
        message->cursor.clear();
        break;
    }
    return true;
}
