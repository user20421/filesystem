#include "LoginPage.h"

#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

LoginPage::LoginPage(ApiClient *api, QWidget *parent)
    : QWidget(parent), m_api(api)
{
    setObjectName(QStringLiteral("loginPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 20, 20, 20);
    outer->addStretch();
    auto *center = new QHBoxLayout;
    center->addStretch();
    auto *panel = new QFrame(this);
    panel->setProperty("card", true);
    panel->setObjectName(QStringLiteral("brandCard"));
    panel->setFixedWidth(430);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(32, 18, 32, 18);
    layout->setSpacing(10);
    auto *brand = new QLabel(QStringLiteral("即时通讯与文件传输系统"), panel);
    brand->setObjectName(QStringLiteral("brandTitle"));
    brand->setAlignment(Qt::AlignCenter);
    layout->addWidget(brand);
    auto *subtitle = new QLabel(QStringLiteral("安全、专注的团队沟通"), panel);
    subtitle->setObjectName(QStringLiteral("mutedLabel"));
    subtitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(subtitle);
    auto *server = new QLabel(QStringLiteral("服务器  ·  %1").arg(api->serverUrl().toString()), panel);
    server->setObjectName(QStringLiteral("serverLabel"));
    server->setWordWrap(true);
    layout->addWidget(server);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);
    m_username = new QLineEdit(panel);
    m_username->setObjectName(QStringLiteral("usernameEdit"));
    m_username->setFixedHeight(38);
    m_username->setPlaceholderText(QStringLiteral("请输入用户名（自动转为大写）"));
    connect(m_username, &QLineEdit::textEdited, this, [this](const QString &text) {
        const QString upper = text.toUpper();
        if (text == upper)
            return;
        const int position = m_username->cursorPosition();
        m_username->setText(upper);
        m_username->setCursorPosition(position);
    });
    m_password = new QLineEdit(panel);
    m_password->setObjectName(QStringLiteral("passwordEdit"));
    m_password->setFixedHeight(38);
    m_password->setPlaceholderText(QStringLiteral("6~128 个 UTF-8 字节"));
    m_password->setEchoMode(QLineEdit::Password);
    // Local simulation convenience only; never use a prefilled password in production.
    m_password->setText(QStringLiteral("123456"));
    form->addRow(QStringLiteral("用户名"), m_username);
    form->addRow(QStringLiteral("密码"), m_password);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout;
    m_login = new QPushButton(QStringLiteral("登录"), panel);
    m_login->setObjectName(QStringLiteral("loginButton"));
    m_login->setFixedHeight(38);
    m_login->setProperty("primary", true);
    m_register = new QPushButton(QStringLiteral("创建账号"), panel);
    m_register->setObjectName(QStringLiteral("registerButton"));
    m_register->setFixedHeight(38);
    buttons->addWidget(m_register);
    buttons->addWidget(m_login);
    layout->addLayout(buttons);
    m_systemTest = new QPushButton(QStringLiteral("系统测试"), panel);
    m_systemTest->setObjectName(QStringLiteral("openSystemTestButton"));
    m_systemTest->setProperty("navigation", true);
    m_systemTest->setFixedHeight(30);
    layout->addWidget(m_systemTest);
    connect(m_systemTest, &QPushButton::clicked, this, [this] {
        m_username->setText(QStringLiteral("ROOT"));
        m_password->setText(QStringLiteral("123456"));
        authenticate(false);
    });
    m_status = new QLabel(panel);
    m_status->setObjectName(QStringLiteral("loginStatus"));
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(24);
    layout->addWidget(m_status);
    center->addWidget(panel);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch();

    connect(m_login, &QPushButton::clicked, this, [this] { authenticate(false); });
    connect(m_register, &QPushButton::clicked, this, [this] { authenticate(true); });
    connect(m_password, &QLineEdit::returnPressed, this, [this] { authenticate(false); });
}

void LoginPage::setUsername(const QString &username)
{
    if (!username.isEmpty())
        m_username->setText(username.toUpper());
}

void LoginPage::showError(const QString &error)
{
    if (!m_closing)
        m_status->setText(error);
}

void LoginPage::setBusy(bool busy)
{
    m_login->setEnabled(!busy);
    m_register->setEnabled(!busy);
    m_systemTest->setEnabled(!busy);
    m_username->setEnabled(!busy);
    m_password->setEnabled(!busy);
}

void LoginPage::authenticate(bool registration)
{
    if (m_closing)
        return;
    const QString username = m_username->text().trimmed().toUpper();
    m_username->setText(username);
    const QString password = m_password->text();
    if (username.isEmpty() || password.isEmpty()) {
        m_status->setText(QStringLiteral("请输入用户名和密码"));
        return;
    }
    const qsizetype passwordBytes = password.toUtf8().size();
    if (passwordBytes < 6 || passwordBytes > 128) {
        m_status->setText(QStringLiteral("密码必须为 6~128 个 UTF-8 字节"));
        return;
    }
    setBusy(true);
    m_status->setText(registration ? QStringLiteral("正在创建安全会话…") : QStringLiteral("正在登录…"));
    QPointer<LoginPage> guarded(this);
    ApiClient::AuthCallback deliver = [guarded](bool ok, const AuthSession &session,
                                                const QString &error) {
        if (!guarded || guarded->m_closing)
            return;
        guarded->setBusy(false);
        if (!ok) {
            guarded->m_status->setText(error);
            return;
        }
        // Keep the local demo convenience password available after logout too.
        guarded->m_password->setText(QStringLiteral("123456"));
        emit guarded->authenticated(session);
    };
    if (m_authenticationInterceptor
        && m_authenticationInterceptor(username, password, registration, deliver))
        return;
    if (registration)
        m_api->registerUser(username, password, deliver);
    else
        m_api->login(username, password, deliver);
}

void LoginPage::shutdown()
{
    m_closing = true;
}
