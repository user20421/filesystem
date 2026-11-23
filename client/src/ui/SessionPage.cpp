#include "SessionPage.h"

#include "LoginPage.h"
#include "MainWindow.h"
#include "RootStressSession.h"
#include "SystemTestPage.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

SessionPage::SessionPage(std::unique_ptr<SessionContext> context, QWidget *parent)
    : QWidget(parent), m_context(std::move(context))
{
    setObjectName(QStringLiteral("sessionPage"));
    m_context->m_page = this;
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack);
    m_login = new LoginPage(m_context->api(), m_context->store(), m_stack);
    m_login->setUsername(m_context->preferredUsername());
    m_login->setAuthenticationInterceptor(
        [this](const QString &username, const QString &password, bool registration,
               ApiClient::AuthCallback callback) {
            m_pendingRootAuthentication = !registration
                && username.compare(QStringLiteral("ROOT"), Qt::CaseInsensitive) == 0;
            if (!m_pendingRootAuthentication)
                return false;
            if (!m_rootStress) {
                const QString stressDir = QDir(m_context->dataDir()).filePath(
                    QStringLiteral("root-stress"));
                m_rootStress = new RootStressSession(stressDir, {}, this);
            }
            m_rootStress->login(username, password, std::move(callback));
            return true;
        });
    m_stack->addWidget(m_login);
    connect(m_login, &LoginPage::authenticated, this, [this](const AuthSession &session) {
        if (m_pendingRootAuthentication) {
            m_pendingRootAuthentication = false;
            showSystemTest(session);
        } else {
            startSession(session);
        }
    });
    connect(m_context->api(), &ApiClient::authenticationFailed, this,
            [this](const QString &error) { handleAuthenticationFailed(error); });
    const AuthSession saved = m_context->store()->loadSession();
    if (!saved.accessToken.isEmpty() && !saved.user.id.isEmpty())
        startSession(saved);
}

SessionPage::~SessionPage()
{
    shutdown();
    if (m_context)
        m_context->m_page = nullptr;
}

void SessionPage::startSession(const AuthSession &session, bool online)
{
    if (m_closing || m_chat)
        return;
    m_context->setSession(session);
    m_chat = new MainWindow(m_context->api(), m_context->store(), m_context->codec(),
                            session, online, m_stack);
    m_stack->addWidget(m_chat);
    m_stack->setCurrentWidget(m_chat);
    m_chat->setActive(m_active);
    connect(m_chat, &MainWindow::logoutCompleted, this, [this] { handleLogout(); });
    connect(m_chat, &MainWindow::exitRequested, this, &SessionPage::exitRequested);
    emit titleChanged();
}

void SessionPage::showSystemTest(const AuthSession &session)
{
    if (m_closing || m_systemTest || !m_rootStress
        || session.user.username.compare(QStringLiteral("ROOT"), Qt::CaseInsensitive) != 0)
        return;

    m_stressContainer = new QWidget(m_stack);
    m_stressContainer->setObjectName(QStringLiteral("integratedStressContainer"));
    auto *layout = new QVBoxLayout(m_stressContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *navigation = new QHBoxLayout;
    navigation->setContentsMargins(12, 8, 12, 8);
    auto *back = new QPushButton(QStringLiteral("返回普通登录"), m_stressContainer);
    back->setObjectName(QStringLiteral("returnToLoginButton"));
    navigation->addWidget(back);
    auto *server = new QLabel(QStringLiteral("独立压测后端 · %1")
        .arg(m_rootStress->config().server), m_stressContainer);
    server->setObjectName(QStringLiteral("mutedLabel"));
    navigation->addWidget(server);
    navigation->addStretch();
    layout->addLayout(navigation);
    m_systemTest = new SystemTestPage(m_rootStress->config(), m_stressContainer);
    layout->addWidget(m_systemTest, 1);
    m_stack->addWidget(m_stressContainer);
    m_stack->setCurrentWidget(m_stressContainer);
    connect(back, &QPushButton::clicked, this, &SessionPage::leaveSystemTest);
    emit stressModeChanged(true);
    emit titleChanged();
}

void SessionPage::leaveSystemTest()
{
    if (m_closing || !m_stressContainer)
        return;
    QWidget *container = m_stressContainer;
    m_stressContainer = nullptr;
    m_systemTest->shutdown();
    m_systemTest = nullptr;
    m_rootStress->clearSession();
    m_stack->removeWidget(container);
    container->deleteLater();
    m_login->showError({});
    m_stack->setCurrentWidget(m_login);
    emit stressModeChanged(false);
    emit titleChanged();
}

void SessionPage::handleAuthenticationFailed(const QString &error)
{
    returnToLogin(QStringLiteral("登录会话已失效：") + error);
}

void SessionPage::handleLogout()
{
    returnToLogin({});
}

void SessionPage::returnToLogin(const QString &error)
{
    if (m_closing)
        return;
    m_context->setSession({});
    m_context->store()->clearSession();
    if (m_chat) {
        MainWindow *chat = m_chat;
        m_chat = nullptr;
        chat->shutdown();
        m_stack->removeWidget(chat);
        chat->deleteLater();
    }
    m_login->showError(error);
    m_stack->setCurrentWidget(m_login);
    emit titleChanged();
}

void SessionPage::setActive(bool active)
{
    m_active = active;
    if (m_chat)
        m_chat->setActive(active);
}

void SessionPage::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    if (m_login)
        m_login->shutdown();
    if (m_chat)
        m_chat->shutdown();
    if (m_systemTest)
        m_systemTest->shutdown();
    if (m_rootStress)
        m_rootStress->shutdown();
    if (m_context)
        m_context->shutdown();
}
