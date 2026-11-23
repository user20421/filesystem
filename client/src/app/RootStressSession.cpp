#include "RootStressSession.h"

#include "SessionContext.h"

#include <QDir>
#include <QPointer>

RootStressSession::RootStressSession(const QString &dataDir, const QUrl &serverUrl,
                                     QObject *parent)
    : QObject(parent), m_config(StressRunConfig::forProject())
{
    if (serverUrl.isValid() && !serverUrl.isEmpty())
        m_config.server = serverUrl.toString();
    m_storeDir = dataDir.trimmed().isEmpty()
        ? QDir(m_config.projectRoot).filePath(QStringLiteral("runtime/simulator"))
        : dataDir;
    // LoginPage 只需要初始 ApiClient；真正的 Direct store 在取得全局进入锁后重新打开。
    m_context = std::make_unique<SessionContext>(m_storeDir, QUrl(m_config.server),
        QStringLiteral("ROOT"), SessionContext::StorageMode::Direct);
}

RootStressSession::~RootStressSession()
{
    shutdown();
}

bool RootStressSession::isOpen() const
{
    return !m_closing && m_context != nullptr;
}

void RootStressSession::releaseLock()
{
    if (m_systemTestLock) {
        if (m_systemTestLock->isLocked())
            m_systemTestLock->unlock();
        m_systemTestLock.reset();
    }
}

void RootStressSession::releaseAttempt()
{
    if (m_preparer) {
        StressRunController *preparer = m_preparer;
        m_preparer = nullptr;
        preparer->stopAndWait();
        preparer->deleteLater();
    }
    if (m_context) {
        SessionContext *oldContext = m_context.release();
        oldContext->setSession({});
        oldContext->shutdown();
        // 登录失败回调可能仍在 ApiClient 栈上；解除引用和锁后延迟销毁。
        oldContext->deleteLater();
    }
    releaseLock();
    m_loginInProgress = false;
}

// 顺序固定为：全局进入锁 -> Setup -> 真实 ROOT 登录。
// 任一失败都销毁本次 Context/准备器并释放锁，下一次点击可从头重试。
void RootStressSession::login(const QString &username, const QString &password,
                              ApiClient::AuthCallback callback)
{
    if (m_closing) {
        callback(false, {}, QStringLiteral("压测会话已经关闭"));
        return;
    }
    if (m_loginInProgress) {
        callback(false, {}, QStringLiteral("ROOT 登录或环境准备正在进行，请稍候"));
        return;
    }

    releaseAttempt();
    m_startupError.clear();
    const QString runtimeDir = QDir(m_config.projectRoot).filePath(QStringLiteral("runtime"));
    if (!QDir().mkpath(runtimeDir)) {
        m_startupError = QStringLiteral("无法创建系统测试运行目录：%1").arg(runtimeDir);
        callback(false, {}, m_startupError);
        return;
    }
    const QString lockPath = QDir(runtimeDir).filePath(QStringLiteral("system-test.lock"));
    m_systemTestLock = std::make_unique<QLockFile>(lockPath);
    m_systemTestLock->setStaleLockTime(30000);
    if (!m_systemTestLock->tryLock(0)) {
        m_startupError = QStringLiteral("系统测试正在其他客户端中运行，请稍后重试。");
        releaseLock();
        callback(false, {}, m_startupError);
        return;
    }

    m_context = std::make_unique<SessionContext>(m_storeDir, QUrl(m_config.server),
        QStringLiteral("ROOT"), SessionContext::StorageMode::Direct);
    if (!m_context->open(&m_startupError)) {
        const QString error = m_startupError;
        releaseAttempt();
        callback(false, {}, error);
        return;
    }

    m_context->setSession({});
    m_loginInProgress = true;
    auto *preparer = new StressRunController(m_config, this);
    m_preparer = preparer;
    QPointer<RootStressSession> guarded(this);
    QPointer<StressRunController> guardedPreparer(preparer);
    connect(preparer, &StressRunController::runFinished, this,
        [guarded, guardedPreparer, username, password,
         callback = std::move(callback)](bool prepared, const QString &prepareError) mutable {
            if (!guarded || guarded->m_closing)
                return;
            if (guardedPreparer)
                guardedPreparer->deleteLater();
            if (guarded->m_preparer == guardedPreparer.data())
                guarded->m_preparer = nullptr;
            if (!prepared) {
                guarded->m_startupError = prepareError;
                guarded->releaseAttempt();
                callback(false, {}, prepareError);
                return;
            }
            if (!guarded->m_context) {
                guarded->releaseAttempt();
                callback(false, {}, QStringLiteral("压测会话在环境准备后不可用"));
                return;
            }
            guarded->m_context->api()->login(username, password,
                [guarded, callback = std::move(callback)](
                    bool ok, const AuthSession &session, const QString &error) mutable {
                    if (!guarded || guarded->m_closing)
                        return;
                    if (!ok) {
                        guarded->m_startupError = error;
                        guarded->releaseAttempt();
                        callback(false, {}, error);
                        return;
                    }
                    if (session.user.username.compare(QStringLiteral("ROOT"),
                                                      Qt::CaseInsensitive) != 0) {
                        const QString message = QStringLiteral("压测后端未返回 ROOT 会话");
                        guarded->m_startupError = message;
                        guarded->releaseAttempt();
                        callback(false, {}, message);
                        return;
                    }
                    QString saveError;
                    if (!guarded->m_context->store()->saveSession(session, &saveError)) {
                        const QString message = QStringLiteral("无法保存压测会话：") + saveError;
                        guarded->m_startupError = message;
                        guarded->releaseAttempt();
                        callback(false, {}, message);
                        return;
                    }
                    guarded->m_context->setSession(session);
                    guarded->m_loginInProgress = false;
                    callback(true, session, {});
                });
        });
    if (!preparer->prepare()) {
        m_startupError = QStringLiteral("无法启动系统测试环境准备");
        releaseAttempt();
        callback(false, {}, m_startupError);
    }
}

void RootStressSession::clearSession()
{
    releaseAttempt();
}

void RootStressSession::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    releaseAttempt();
}
