#include "MessagePayloadCodec.h"

#include "messages.qpb.h"

#include <QtProtobuf/QProtobufSerializer>

bool MessagePayloadCodec::encode(const ChatMessage &message, QByteArray *payload, QString *error)
{
    im::v1::MessagePayload body;
    if (message.isFile()) {
        im::v1::FileNotification file;
        file.setFileId(message.fileId);
        file.setFilename(message.fileName);
        file.setSize(message.fileSize);
        body.setFile(file);
    } else {
        body.setText(message.content);
    }

    QProtobufSerializer serializer;
    *payload = body.serialize(&serializer);
    if (serializer.lastError() != QAbstractProtobufSerializer::Error::None) {
        payload->clear();
        if (error)
            *error = QStringLiteral("MessagePayload 序列化失败：") + serializer.lastErrorString();
        return false;
    }
    return true;
}

bool MessagePayloadCodec::decode(const QByteArray &payload, ChatMessage *message, QString *error)
{
    im::v1::MessagePayload body;
    QProtobufSerializer serializer;
    if (!body.deserialize(&serializer, payload)) {
        if (error)
            *error = QStringLiteral("无法解析 MessagePayload：") + serializer.lastErrorString();
        return false;
    }

    ChatMessage decoded;
    switch (body.contentField()) {
    case im::v1::MessagePayload::ContentFields::Text:
        decoded.kind = QStringLiteral("text");
        decoded.content = body.text();
        break;
    case im::v1::MessagePayload::ContentFields::File: {
        const im::v1::FileNotification file = body.file();
        decoded.kind = QStringLiteral("file");
        decoded.fileId = file.fileId();
        decoded.fileName = file.filename();
        decoded.fileSize = static_cast<qint64>(file.size());
        break;
    }
    default:
        if (error)
            *error = QStringLiteral("MessagePayload 缺少文本或文件通知");
        return false;
    }
    *message = decoded;
    return true;
}
