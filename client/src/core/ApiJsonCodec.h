#pragma once

#include "ApiTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <initializer_list>

// ApiJsonCodec 只负责 REST JSON 字段适配，不持有网络、认证或 UI 状态。
// 早期开发服务器的字段别名和 legacy 消息分支也集中保留在这里。
namespace ApiJsonCodec {
QJsonObject payloadObject(const QJsonObject &root);
QString stringField(const QJsonObject &object, std::initializer_list<const char *> names);
qint64 integerField(const QJsonObject &object, std::initializer_list<const char *> names,
                    qint64 fallback = 0);
User parseUser(QJsonObject object);
AuthSession parseAuth(const QJsonObject &root);
FileInitInfo parseFileInit(const QJsonObject &root);
FileStatus parseFileStatus(const QJsonObject &root);
ChatMessage parseMessage(const QJsonObject &source);
QString responseError(const QByteArray &body, const QString &fallback);
} // namespace ApiJsonCodec
