#pragma once

#include "StressRunController.h"

#include <QMainWindow>
#include <QUrl>
#include <memory>

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
    QString startupError() const { return m_startupError; }
    bool isAuthenticated() const { return m_systemTest != nullptr; }
    SystemTestPage *systemTestPage() const { return m_systemTest; }
    SessionContext *context() const { return m_context.get(); }
    bool savePreview(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void showSystemTest(const AuthSession &session);
    void shutdown();

    StressRunConfig m_config;
    std::unique_ptr<SessionContext> m_context;
    QStackedWidget *m_stack = nullptr;
    LoginPage *m_login = nullptr;
    SystemTestPage *m_systemTest = nullptr;
    QString m_startupError;
    bool m_closing = false;
};
