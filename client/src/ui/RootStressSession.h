#pragma once

#include "ApiClient.h"
#include "StressRunController.h"

#include <QObject>
#include <QUrl>
#include <memory>

class SessionContext;

// RootStressSession 复用独立 simulator 和集成页面的 ROOT 鉴权逻辑。
// ROOT 使用独立后端、Context 和会话目录，绝不覆盖普通聊天账号的令牌或本地状态。
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
    void clearSession();
    void shutdown();

private:
    StressRunConfig m_config;
    std::unique_ptr<SessionContext> m_context;
    QString m_startupError;
    bool m_closing = false;
};
