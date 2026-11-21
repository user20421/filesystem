#pragma once

#include "ApiTypes.h"

#include <QByteArray>
#include <QString>

// Encodes and decodes the Protobuf body carried by MESSAGE envelopes.
class MessagePayloadCodec final {
public:
    static bool encode(const ChatMessage &message, QByteArray *payload, QString *error);
    static bool decode(const QByteArray &payload, ChatMessage *message, QString *error);
};
