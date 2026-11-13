#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>

struct User {
    QString id;
    QString username;
    QString displayName;
    bool online = false;
    bool presenceKnown = false;

    QString label() const { return (displayName.isEmpty() ? username : displayName).toUpper(); }
};

struct AuthSession {
    QString accessToken;
    QString refreshToken;
    User user;
};

struct ChatMessage {
    QString id;
    QString cursor;
    QString conversationId;
    QString senderId;
    QString recipientId;
    QString kind = QStringLiteral("text");
    QString content;
    QString fileId;
    QString fileName;
    qint64 fileSize = 0;
    qint64 timestamp = 0;
    qint64 readAt = 0;

    bool isFile() const { return kind == QStringLiteral("file") || !fileId.isEmpty(); }
};

struct FileInitInfo {
    QString fileId;
    int chunkSize = 4 * 1024 * 1024;
    QSet<int> uploadedChunks;
};

struct FileStatus {
    QString fileId;
    QString fileName;
    qint64 size = 0;
    int chunkSize = 4 * 1024 * 1024;
    QSet<int> uploadedChunks;
    QString status;
    bool complete = false;
};

// Payloads carried by Envelope.payload are JSON until proto/im/v1/messages.proto
// is finalized.  Keeping this adapter here avoids UI dependencies on field names.
inline QJsonObject textPayload(const QString &text)
{
    return {{QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), text}};
}

inline QJsonObject filePayload(const QString &fileId, const QString &name, qint64 size)
{
    return {{QStringLiteral("type"), QStringLiteral("file")},
            {QStringLiteral("file_id"), fileId},
            {QStringLiteral("file_name"), name},
            {QStringLiteral("size"), size}};
}
