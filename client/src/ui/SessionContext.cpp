#include "SessionContext.h"

#include "SessionPage.h"

SessionContext::SessionContext(const QString &dataDir, const QUrl &serverUrl,
                               const QString &preferredUsername, QObject *parent)
    : QObject(parent), m_store(new LocalStore(dataDir, this)), m_api(new ApiClient(this)),
      m_codec(createDefaultWireCodec()), m_lock(m_store->path(QStringLiteral("config/client.lock"))),
      m_preferredUsername(preferredUsername)
{
    m_api->setServerUrl(serverUrl);
    m_api->setSessionStore(m_store);
    connect(m_api, &ApiClient::sessionRefreshed, this,
            [this](const AuthSession &session) { setSession(session); });
    connect(m_api, &ApiClient::authenticationFailed, this, [this](const QString &) {
        m_session = {};
    });
    m_lock.setStaleLockTime(30000);
}

SessionContext::~SessionContext()
{
    shutdown();
}

bool SessionContext::open(QString *error)
{
    if (m_open)
        return true;
    if (m_closing) {
        if (error)
            *error = QStringLiteral("会话已经关闭");
        return false;
    }
    if (!m_store->initialize(error))
        return false;
    if (!m_lock.tryLock(0)) {
        if (error)
            *error = QStringLiteral("该数据目录已被使用：%1").arg(dataDir());
        return false;
    }
    m_open = true;
    return true;
}

void SessionContext::setSession(const AuthSession &session)
{
    m_session = session;
    m_api->setAuthSession(session);
}

void SessionContext::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_page)
        m_page->shutdown();
    m_api->shutdown();
    if (m_open)
        m_lock.unlock();
    m_open = false;
}
