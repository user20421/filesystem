#include "SimulatorWindow.h"

#include "LoginPage.h"
#include "RootStressSession.h"
#include "SessionContext.h"
#include "SystemTestPage.h"

#include <QCloseEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>

SimulatorWindow::SimulatorWindow(const QString &dataDir, const QUrl &serverUrl, QWidget *parent)
    : QMainWindow(parent), m_root(new RootStressSession(dataDir, serverUrl, this))
{
    setObjectName(QStringLiteral("simulatorWindow"));
    setWindowTitle(QStringLiteral("ROOT 登录 · 系统压力测试"));
    resize(1280, 820);
    if (!m_root->isOpen())
        return;

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("simulatorStack"));
    setCentralWidget(m_stack);
    m_login = new LoginPage(m_root->context()->api(), m_stack);
    m_login->setAuthenticationInterceptor(
        [this](const QString &username, const QString &password, bool registration,
               ApiClient::AuthCallback callback) {
            if (registration)
                return false;
            m_root->login(username, password, std::move(callback));
            return true;
        });
    m_login->setUsername(QStringLiteral("ROOT"));
    if (auto *username = m_login->findChild<QLineEdit *>(QStringLiteral("usernameEdit"))) {
        username->setReadOnly(true);
        username->setToolTip(QStringLiteral("压测工具固定使用 ROOT 账号"));
    }
    if (auto *password = m_login->findChild<QLineEdit *>(QStringLiteral("passwordEdit")))
        password->setText(QStringLiteral("123456"));
    if (auto *registration = m_login->findChild<QPushButton *>(QStringLiteral("registerButton")))
        registration->hide();
    m_stack->addWidget(m_login);
    connect(m_login, &LoginPage::authenticated, this, &SimulatorWindow::showSystemTest);
}

SimulatorWindow::~SimulatorWindow()
{
    shutdown();
}

bool SimulatorWindow::isOpen() const
{
    return m_root && m_root->isOpen();
}

void SimulatorWindow::showSystemTest(const AuthSession &session)
{
    if (m_closing || m_systemTest || session.user.username.toUpper() != QLatin1String("ROOT"))
        return;
    m_systemTest = new SystemTestPage(m_root->config(), m_stack);
    m_stack->addWidget(m_systemTest);
    m_stack->setCurrentWidget(m_systemTest);
    m_stack->removeWidget(m_login);
    m_login->shutdown();
    m_login->deleteLater();
    m_login = nullptr;
    setWindowTitle(QStringLiteral("ROOT · 系统压力测试"));
}

void SimulatorWindow::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_login)
        m_login->shutdown();
    if (m_systemTest)
        m_systemTest->shutdown();
    if (m_root)
        m_root->shutdown();
}

void SimulatorWindow::closeEvent(QCloseEvent *event)
{
    shutdown();
    QMainWindow::closeEvent(event);
}
