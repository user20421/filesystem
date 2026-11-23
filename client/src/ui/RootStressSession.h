#pragma once

#include "ApiClient.h"
#include "StressRunController.h"

#include <QLockFile>
#include <QObject>
#include <QUrl>
#include <memory>

class SessionContext;

// RootStressSession 复用独立 simulator 和集成页面的 ROOT 鉴权逻辑。
// 所有入口共享 runtime/system-test.lock；进入锁后才 Setup 和真实登录。
class RootStressSession final : public QObject {
public:
    RootStressSession(const QString &dataDir, const QUrl &serverUrl = {},
                      QObject *parent = nullptr);
    ~RootStressSession() override;

    bool isOpen() const;
    QString startupError() const { return m_startupError; }
    SessionContext *context() const { return m_context.get(); }
    const StressRunConfig &config() const { return m_config; }
    void login(const QString &username, const QString &password,
               ApiClient::AuthCallback callback);
    // 调用方必须先停止 runner；本函数销毁失败/旧 Context 并释放进入锁。
    void clearSession();
    void shutdown();

private:
    void releaseAttempt();
    void releaseLock();

    StressRunConfig m_config;
    QString m_storeDir;
    std::unique_ptr<SessionContext> m_context;
    StressRunController *m_preparer = nullptr;
    std::unique_ptr<QLockFile> m_systemTestLock;
    QString m_startupError;
    bool m_loginInProgress = false;
    bool m_closing = false;
};
