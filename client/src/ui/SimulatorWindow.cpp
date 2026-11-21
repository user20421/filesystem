#include "SimulatorWindow.h"

#include "LoginPage.h"
#include "SessionContext.h"
#include "SystemTestPage.h"

#include <QCloseEvent>
#include <QDir>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>

SimulatorWindow::SimulatorWindow(const QString &dataDir, const QUrl &serverUrl, QWidget *parent)
    : QMainWindow(parent), m_config(StressRunConfig::forProject())
{
    setObjectName(QStringLiteral("simulatorWindow"));
    setWindowTitle(QStringLiteral("ROOT 登录 · 系统压力测试"));
    resize(1280, 820);
    if (serverUrl.isValid() && !serverUrl.isEmpty())
        m_config.server = serverUrl.toString();
    const QString storeDir = dataDir.trimmed().isEmpty()
        ? QDir(m_config.projectRoot).filePath(QStringLiteral("runtime/simulator"))
        : dataDir;
    m_context = std::make_unique<SessionContext>(storeDir, QUrl(m_config.server),
                                                 QStringLiteral("ROOT"));
    if (!m_context->open(&m_startupError))
        return;

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("simulatorStack"));
    setCentralWidget(m_stack);
    m_login = new LoginPage(m_context->api(), m_context->store(), m_stack);
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
    return m_context && m_context->isOpen();
}

void SimulatorWindow::showSystemTest(const AuthSession &session)
{
    if (m_closing || m_systemTest || session.user.username.toUpper() != QLatin1String("ROOT"))
        return;
    m_context->setSession(session);
    m_systemTest = new SystemTestPage(m_config, m_stack);
    m_stack->addWidget(m_systemTest);
    m_stack->setCurrentWidget(m_systemTest);
    m_stack->removeWidget(m_login);
    m_login->shutdown();
    m_login->deleteLater();
    m_login = nullptr;
    setWindowTitle(QStringLiteral("ROOT · 系统压力测试"));
}

bool SimulatorWindow::savePreview(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QPixmap preview = grab();
    return !preview.isNull() && preview.save(path, "PNG");
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
    if (m_context)
        m_context->shutdown();
}

void SimulatorWindow::closeEvent(QCloseEvent *event)
{
    shutdown();
    QMainWindow::closeEvent(event);
}
