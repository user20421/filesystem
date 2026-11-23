#pragma once

#include "ApiTypes.h"

#include <QDir>
#include <QObject>
#include <QSet>

// LocalStore 管理单用户目录下的会话、消息投影和 outbox。
// 原子写与按 message_id 合并用于防止重复和历史回退，pending/failed 消息不会被历史裁剪淘汰。
class LocalStore final : public QObject {
    Q_OBJECT
public:
    explicit LocalStore(QString dataDir, QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);
    QDir root() const { return m_root; }
    QString path(const QString &name) const;

    bool saveSession(const AuthSession &session, QString *error = nullptr);
    AuthSession loadSession() const;
    void clearSession();

    // Returns true only when a new projection was successfully persisted.
    // An unchanged duplicate returns false with an empty error.
    bool appendMessage(const QString &peerId, const ChatMessage &message,
                       QString *error = nullptr);
    bool markMessagesRead(const QString &peerId, const QString &senderId,
                          const QString &cursor, qint64 readAt, QString *error = nullptr);
    QList<ChatMessage> messages(const QString &peerId, const QString &afterId = {}) const;

    // Outbox entries are selected by the authenticated sender, never by a
    // contact selected in the UI. Failed entries remain available for restart.
    QList<ChatMessage> outbox(const QString &senderId) const;
    bool findMessage(const QString &messageId, const QString &senderId,
                     ChatMessage *message, QString *peerId = nullptr) const;
    bool recoverOutbox(const QString &senderId, qint64 nextAttemptAt,
                       QString *error = nullptr);

    QString uniqueDownloadPath(const QString &originalName) const;
    static QString sanitizeFileName(const QString &originalName);

private:
    QString messagesPath() const;
    QJsonObject readMessageDocument() const;
    bool writeJson(const QString &path, const QJsonObject &object, QString *error = nullptr) const;

    QDir m_root;
    bool m_valid = false;
};
