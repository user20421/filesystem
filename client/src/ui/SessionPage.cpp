#include "SessionPage.h"

#include "LoginPage.h"
#include "MainWindow.h"

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
    m_stack->addWidget(m_login);
    connect(m_login, &LoginPage::authenticated, this, [this](const AuthSession &session) {
        startSession(session);
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
    // Fresh authentication and restored sessions share this user's private registry.
    if (online)
        m_context->store()->rememberAccount(session.user);
    m_chat = new MainWindow(m_context->api(), m_context->store(), m_context->codec(),
                            session, online, m_stack);
    m_stack->addWidget(m_chat);
    m_stack->setCurrentWidget(m_chat);
    m_chat->setActive(m_active);
    connect(m_chat, &MainWindow::connectionStateChanged, this,
            [this](const QString &) { emit connectionStateChanged(); });
    connect(m_chat, &MainWindow::unreadCountChanged, this, &SessionPage::unreadCountChanged);
    connect(m_chat, &MainWindow::logoutCompleted, this, [this] { handleLogout(); });
    connect(m_chat, &MainWindow::exitRequested, this, &SessionPage::exitRequested);
    emit titleChanged();
    emit connectionStateChanged();
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
    emit connectionStateChanged();
    emit unreadCountChanged(0);
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
    if (m_context)
        m_context->shutdown();
}
