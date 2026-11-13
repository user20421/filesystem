#pragma once

#include "ApiClient.h"
#include "LocalStore.h"
#include "WireCodec.h"

#include <QLockFile>
#include <QObject>
#include <memory>

class SessionPage;

// Owns every piece of per-user state. No ApiClient, token, store, codec or lock
// is shared between tabs or with another SessionContext.
class SessionContext final : public QObject {
    Q_OBJECT
public:
    explicit SessionContext(const QString &dataDir, const QUrl &serverUrl,
                            const QString &preferredUsername = {}, QObject *parent = nullptr);
    ~SessionContext() override;

    bool open(QString *error = nullptr);
    bool isOpen() const { return m_open; }
    bool isClosing() const { return m_closing; }
    QString dataDir() const { return m_store->root().absolutePath(); }
    QString preferredUsername() const { return m_preferredUsername; }
    LocalStore *store() const { return m_store; }
    ApiClient *api() const { return m_api; }
    WireCodec *codec() const { return m_codec.get(); }
    const AuthSession &session() const { return m_session; }
    void setSession(const AuthSession &session);
    SessionPage *page() const { return m_page; }
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
