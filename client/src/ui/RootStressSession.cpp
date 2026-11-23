#include "RootStressSession.h"

#include "SessionContext.h"

#include <QDir>

RootStressSession::RootStressSession(const QString &dataDir, const QUrl &serverUrl,
                                     QObject *parent)
    : QObject(parent), m_config(StressRunConfig::forProject())
{
    if (serverUrl.isValid() && !serverUrl.isEmpty())
        m_config.server = serverUrl.toString();
    const QString storeDir = dataDir.trimmed().isEmpty()
        ? QDir(m_config.projectRoot).filePath(QStringLiteral("runtime/simulator"))
        : dataDir;
    // ROOT 使用独立目录和 SessionContext，其 ApiClient、令牌及本地存储均不与普通聊天会话共享。
    m_context = std::make_unique<SessionContext>(storeDir, QUrl(m_config.server),
                                                 QStringLiteral("ROOT"));
    m_context->open(&m_startupError);
}

RootStressSession::~RootStressSession()
{
    shutdown();
}

bool RootStressSession::isOpen() const
{
    return m_context && m_context->isOpen();
}

// login 通过独立压测后端验证 ROOT 并保存到独立目录，失败时不得进入压测页或修改普通会话。
void RootStressSession::login(const QString &username, const QString &password,
                              ApiClient::AuthCallback callback)
{
    if (m_closing || !isOpen()) {
        callback(false, {}, m_startupError.isEmpty()
            ? QStringLiteral("压测会话不可用") : m_startupError);
        return;
    }
    clearSession();
    QPointer<RootStressSession> guarded(this);
    m_context->api()->login(username, password,
        [guarded, callback = std::move(callback)](bool ok, const AuthSession &session,
                                                  const QString &error) mutable {
            if (!guarded || guarded->m_closing)
                return;
            if (!ok) {
                callback(false, {}, error);
                return;
            }
            if (session.user.username.compare(QStringLiteral("ROOT"), Qt::CaseInsensitive) != 0) {
                callback(false, {}, QStringLiteral("压测后端未返回 ROOT 会话"));
                return;
            }
            QString saveError;
            if (!guarded->m_context->store()->saveSession(session, &saveError)) {
                callback(false, {}, QStringLiteral("无法保存压测会话：") + saveError);
                return;
            }
            guarded->m_context->setSession(session);
            callback(true, session, {});
        });
}

void RootStressSession::clearSession()
{
    if (m_context)
        m_context->setSession({});
}

void RootStressSession::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_context)
        m_context->shutdown();
}
