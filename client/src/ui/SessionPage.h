#pragma once

#include "SessionContext.h"

#include <QWidget>
#include <memory>

class LoginPage;
class MainWindow;
class QStackedWidget;
class RootStressSession;
class SystemTestPage;

// SessionPage 在同一窗口管理登录页、聊天页和 ROOT 压测页，普通会话与压测会话使用不同 Context。
// 隐藏页面不会自动停止连接和传输，只有 shutdown 才执行完整清理。
class SessionPage final : public QWidget {
    Q_OBJECT
public:
    explicit SessionPage(std::unique_ptr<SessionContext> context, QWidget *parent = nullptr);
    ~SessionPage() override;

    SessionContext *context() const { return m_context.get(); }
    MainWindow *chatPage() const { return m_chat; }
    SystemTestPage *systemTestPage() const { return m_systemTest; }
    bool isAuthenticated() const { return m_chat != nullptr; }
    void startSession(const AuthSession &session, bool online = true);
    void setActive(bool active);
    void shutdown();
    bool isClosing() const { return m_closing; }

signals:
    void titleChanged();
    void exitRequested();
    void stressModeChanged(bool active);

private:
    void handleAuthenticationFailed(const QString &error);
    void handleLogout();
    void returnToLogin(const QString &error);
    void showSystemTest(const AuthSession &session);
    void leaveSystemTest();

    std::unique_ptr<SessionContext> m_context;
    QStackedWidget *m_stack;
    LoginPage *m_login;
    MainWindow *m_chat = nullptr;
    RootStressSession *m_rootStress = nullptr;
    QWidget *m_stressContainer = nullptr;
    SystemTestPage *m_systemTest = nullptr;
    bool m_pendingRootAuthentication = false;
    bool m_active = true;
    bool m_closing = false;
};
