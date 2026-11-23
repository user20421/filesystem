#pragma once

#include "ApiClient.h"
#include "LocalStore.h"
#include "WireCodec.h"

#include <QLockFile>
#include <QObject>
#include <memory>

class SessionPage;

// SessionContext 独立持有一个用户的目录锁、令牌、存储、API 和编解码器，任何资源都不得跨用户共享。
class SessionContext final : public QObject {
    Q_OBJECT
public:
    explicit SessionContext(const QString &dataDir, const QUrl &serverUrl,
                            const QString &preferredUsername = {}, QObject *parent = nullptr);
    ~SessionContext() override;

    bool open(QString *error = nullptr);
    bool isOpen() const { return m_open; }
    QString dataDir() const { return m_store->root().absolutePath(); }
    QString preferredUsername() const { return m_preferredUsername; }
    LocalStore *store() const { return m_store; }
    ApiClient *api() const { return m_api; }
    WireCodec *codec() const { return m_codec.get(); }
    const AuthSession &session() const { return m_session; }
    void setSession(const AuthSession &session);
    void shutdown();

private:
    friend class SessionPage;
    LocalStore *m_store;
    ApiClient *m_api;
    std::unique_ptr<WireCodec> m_codec;
    QLockFile m_lock;
    QString m_preferredUsername;
    AuthSession m_session;
    SessionPage *m_page = nullptr;
    bool m_open = false;
    bool m_closing = false;
};
