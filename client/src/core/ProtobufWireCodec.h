#pragma once

#include "WireCodec.h"

class ProtobufWireCodec final : public WireCodec {
public:
    bool available() const override { return true; }
    QString unavailableReason() const override { return {}; }
    bool encode(const WireMessage &message, QByteArray *frame, QString *error) const override;
    bool decode(const QByteArray &frame, WireMessage *message, QString *error) const override;
};
