#pragma once

#include "ApiTypes.h"

#include <QJsonObject>

// LocalStore 内部消息 JSON 与合并规则；字段名和旧格式兼容逻辑不属于公共存储接口。
namespace LocalStoreMessageCodec {
QJsonObject messageToJson(const ChatMessage &message);
ChatMessage messageFromJson(const QJsonObject &object);
ChatMessage mergeMessage(const ChatMessage &existing, const ChatMessage &incoming);
} // namespace LocalStoreMessageCodec
