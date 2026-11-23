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

struct UnreadSummary {
    QString peerId;
    int unreadCount = 0;
    qint64 lastMessageAt = 0;
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

    // Transport delivery is independent from the peer's READ receipt.
    bool pending = false;
    bool acked = false;
    bool failed = false;
    int attemptCount = 0;
    qint64 nextAttemptAt = 0;
    QString deliveryError;

    bool isFile() const { return kind == QStringLiteral("file") || !fileId.isEmpty(); }
};

inline constexpr int DefaultFileChunkSize = 5 * 1024 * 1024;

struct FileInitInfo {
    QString fileId;
    int chunkSize = DefaultFileChunkSize;
    QSet<int> uploadedChunks;
    QString status;
    bool complete = false;
};

struct FileStatus {
    QString fileId;
    QString fileName;
    qint64 size = 0;
    int chunkSize = DefaultFileChunkSize;
    QSet<int> uploadedChunks;
    QString status;
    bool complete = false;
};
