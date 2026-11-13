#pragma once

#include "SessionContext.h"

#include <QWidget>
#include <memory>

class LoginPage;
class MainWindow;
class QStackedWidget;

// Embeds login and authenticated chat without a modal dialog. Hiding this page
// does not stop its socket, timers or transfers; only shutdown() does.
class SessionPage final : public QWidget {
    Q_OBJECT
public:
    explicit SessionPage(std::unique_ptr<SessionContext> context, QWidget *parent = nullptr);
    ~SessionPage() override;

    SessionContext *context() const { return m_context.get(); }
    MainWindow *chatPage() const { return m_chat; }
    bool isAuthenticated() const { return m_chat != nullptr; }
    void startSession(const AuthSession &session, bool online = true);
    void startDemo(const AuthSession &session);
    void setActive(bool active);
    void shutdown();
    bool isClosing() const { return m_closing; }

signals:
    void titleChanged();
    void connectionStateChanged();
    void unreadCountChanged(int count);
    void exitRequested();

private:
    std::unique_ptr<SessionContext> m_context;
    QStackedWidget *m_stack;
    LoginPage *m_login;
    MainWindow *m_chat = nullptr;
    bool m_active = true;
    bool m_closing = false;
};
