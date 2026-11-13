#include "WireCodec.h"

#ifdef CLIENT_HAS_PROTOBUF
#include "ProtobufWireCodec.h"
#endif

QString UnsupportedWireCodec::unavailableReason() const
{
    return QStringLiteral("Qt Protobuf 编解码器未配置；请按 client/README.md 使用 qt_add_protobuf 重新构建");
}

bool UnsupportedWireCodec::encode(const WireMessage &, QByteArray *, QString *error) const
{
    if (error)
        *error = unavailableReason();
    return false;
}

bool UnsupportedWireCodec::decode(const QByteArray &, WireMessage *, QString *error) const
{
    if (error)
        *error = unavailableReason();
    return false;
}

std::unique_ptr<WireCodec> createDefaultWireCodec()
{
#ifdef CLIENT_HAS_PROTOBUF
    return std::make_unique<ProtobufWireCodec>();
#else
    return std::make_unique<UnsupportedWireCodec>();
#endif
}
