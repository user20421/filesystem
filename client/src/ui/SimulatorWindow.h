#pragma once

#include "RootStressSession.h"

#include <QMainWindow>
#include <QUrl>

struct AuthSession;
class LoginPage;
class SessionContext;
class QStackedWidget;
class SystemTestPage;

// Single-purpose stress tool: authenticate ROOT, then show only SystemTestPage.
class SimulatorWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SimulatorWindow(const QString &dataDir = {}, const QUrl &serverUrl = {},
                             QWidget *parent = nullptr);
    ~SimulatorWindow() override;

    bool isOpen() const;
    QString startupError() const { return m_root ? m_root->startupError() : QString{}; }
    bool isAuthenticated() const { return m_systemTest != nullptr; }
    SystemTestPage *systemTestPage() const { return m_systemTest; }
    SessionContext *context() const { return m_root ? m_root->context() : nullptr; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void showSystemTest(const AuthSession &session);
    void shutdown();

    RootStressSession *m_root = nullptr;
    QStackedWidget *m_stack = nullptr;
    LoginPage *m_login = nullptr;
    SystemTestPage *m_systemTest = nullptr;
    bool m_closing = false;
};
