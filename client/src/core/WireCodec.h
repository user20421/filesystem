#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

struct WireMessage {
    enum class Type : int { Unspecified = 0, Message = 1, Ack = 2, Error = 3, Ping = 4, Pong = 5, Read = 6 };

    quint32 version = 2;
    Type type = Type::Message;
    QString messageId;
    QString conversationId;
    QString senderId;
    QString recipientId;
    QString deviceId;
    qint64 timestamp = 0;
    QByteArray payload;
    QString cursor;
    qint64 readAt = 0;
    QString errorCode;
    QString errorMessage;
};

class WireCodec {
public:
    virtual ~WireCodec() = default;
    virtual bool available() const = 0;
    virtual QString unavailableReason() const = 0;
    virtual bool encode(const WireMessage &message, QByteArray *frame, QString *error) const = 0;
    virtual bool decode(const QByteArray &frame, WireMessage *message, QString *error) const = 0;
};

std::unique_ptr<WireCodec> createDefaultWireCodec();
