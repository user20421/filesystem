#pragma once

#include "ApiTypes.h"

#include <QDir>
#include <QJsonArray>
#include <QObject>
#include <QSet>

class LocalStore final : public QObject {
    Q_OBJECT
public:
    explicit LocalStore(QString dataDir, QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);
    QDir root() const { return m_root; }
    QString path(const QString &name) const;

    int concurrency() const;
    void setConcurrency(int value);

    bool saveSession(const AuthSession &session, QString *error = nullptr);
    AuthSession loadSession() const;
    void clearSession();

    // Shared by sibling per-user data directories; contains only IDs/usernames.
    QSet<QString> knownAccountIds() const;
    bool rememberAccount(const User &user, QString *error = nullptr) const;
    bool forgetAccount(const QString &userId, QString *error = nullptr) const;

    // Returns true when a new message or new server metadata was persisted.
    bool appendMessage(const QString &peerId, const ChatMessage &message);
    bool markMessagesRead(const QString &peerId, const QString &senderId,
                          const QString &cursor, qint64 readAt);
    QList<ChatMessage> messages(const QString &peerId, const QString &afterId = {}) const;

    QString uniqueDownloadPath(const QString &originalName) const;
    static QString sanitizeFileName(const QString &originalName);

private:
    QString messagesPath() const;
    QString knownAccountsPath() const;
    QJsonArray readKnownAccounts() const;
    bool writeKnownAccounts(const QJsonArray &accounts, QString *error = nullptr) const;
    QJsonObject readMessageDocument() const;
    bool writeJson(const QString &path, const QJsonObject &object, QString *error = nullptr) const;

    QDir m_root;
    bool m_valid = false;
};
