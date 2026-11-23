#include "WireCodec.h"

#include "ProtobufWireCodec.h"

std::unique_ptr<WireCodec> createDefaultWireCodec()
{
    return std::make_unique<ProtobufWireCodec>();
}
