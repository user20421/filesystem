#include "SessionPage.h"

#include "LoginPage.h"
#include "MainWindow.h"
#include "RootStressSession.h"
#include "SystemTestPage.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
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
    const AuthSession saved = m_context->sessionToRestore();
    m_login = new LoginPage(m_context->api(), m_stack);
    m_login->setUsername(m_context->preferredUsername().isEmpty()
        ? saved.user.username : m_context->preferredUsername());
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
    if (!saved.accessToken.isEmpty())
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
    bool migrateLegacy = false;
    if (!session.user.username.trimmed().isEmpty()
        && m_context->legacyMigrationNeedsConfirmation()) {
        const auto answer = QMessageBox::question(m_login, QStringLiteral("迁移旧版本数据"),
            QStringLiteral("检测到旧数据目录中的 config/downloads，但旧会话没有有效身份。\n"
                           "是否把这些数据迁移到 %1 的独立用户目录？\n"
                           "选择“否”会保留旧数据并使用新的空目录；目标非空时禁止合并。")
                .arg(SessionContext::normalizedUsername(session.user.username)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        migrateLegacy = answer == QMessageBox::Yes;
    }
    QString activationError;
    if (!m_context->activateUser(session, migrateLegacy, &activationError)) {
        m_login->showError(QStringLiteral("无法初始化用户目录：") + activationError);
        m_stack->setCurrentWidget(m_login);
        return;
    }
    QString saveError;
    if (!m_context->store()->saveSession(m_context->session(), &saveError)) {
        m_context->closeUser();
        m_login->showError(QStringLiteral("无法保存会话：") + saveError);
        m_stack->setCurrentWidget(m_login);
        return;
    }
    m_chat = new MainWindow(m_context->api(), m_context->store(), m_context->codec(),
                            m_context->session(), online, m_stack);
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
    // 先停止聊天、WebSocket 和传输，再解绑并删除 store，最后释放用户目录锁。
    if (m_chat) {
        MainWindow *chat = m_chat;
        m_chat = nullptr;
        chat->shutdown();
        m_stack->removeWidget(chat);
        chat->deleteLater();
    }
    m_context->closeUser();
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
