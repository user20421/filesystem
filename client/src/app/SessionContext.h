#pragma once

#include "ApiClient.h"
#include "LocalStore.h"
#include "WireCodec.h"

#include <QLockFile>
#include <QObject>
#include <memory>

// SessionContext 的 dataDir 是共享 base；普通用户登录后才绑定 users/<规范化用户名>。
// Direct 仅供独立 ROOT 压测使用，保持其既有单目录会话布局。
class SessionContext final : public QObject {
    Q_OBJECT
public:
    enum class StorageMode { UserBase, Direct };

    explicit SessionContext(const QString &dataDir, const QUrl &serverUrl,
                            const QString &preferredUsername = {},
                            StorageMode mode = StorageMode::UserBase,
                            QObject *parent = nullptr);
    SessionContext(const QString &dataDir, const QUrl &serverUrl,
                   const QString &preferredUsername, QObject *parent)
        : SessionContext(dataDir, serverUrl, preferredUsername, StorageMode::UserBase, parent) {}
    ~SessionContext() override;

    bool open(QString *error = nullptr);
    bool isOpen() const { return m_open; }
    QString dataDir() const { return m_baseDir; }
    QString preferredUsername() const { return m_preferredUsername; }
    LocalStore *store() const { return m_store; }
    ApiClient *api() const { return m_api; }
    WireCodec *codec() const { return m_codec.get(); }
    const AuthSession &session() const { return m_session; }

    // 后端用户名是唯一身份来源。activateUser 成功后才能创建聊天页。
    bool activateUser(const AuthSession &session, bool migrateLegacy = false,
                      QString *error = nullptr);
    void closeUser();
    bool hasActiveUser() const { return m_store != nullptr; }
    QString userDataDir() const { return m_store ? m_store->root().absolutePath() : QString{}; }

    AuthSession sessionToRestore() const;
    bool hasLegacyData() const;
    bool legacyMigrationNeedsConfirmation() const;

    static QString normalizedUsername(const QString &username);
    static QString userDirectoryName(const QString &username);
    static QString userDataPath(const QString &baseDir, const QString &username);

    void setSession(const AuthSession &session);
    void shutdown();

private:
    bool migrateLegacyData(const QString &userDir, QString *error) const;
    bool migratePrefixedUserData(const QString &userDir, const AuthSession &session,
                                 bool hasAnotherSource, QString *error) const;
    bool lockStore(LocalStore *store, bool direct, QString *error);

    QString m_baseDir;
    LocalStore *m_store = nullptr;
    ApiClient *m_api;
    std::unique_ptr<WireCodec> m_codec;
    std::unique_ptr<QLockFile> m_lock;
    QString m_preferredUsername;
    AuthSession m_session;
    StorageMode m_mode;
    bool m_open = false;
    bool m_userLocked = false;
    bool m_closing = false;
};
