#include "LoginPage.h"

#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

LoginPage::LoginPage(ApiClient *api, LocalStore *store, QWidget *parent)
    : QWidget(parent), m_api(api), m_store(store)
{
    setObjectName(QStringLiteral("loginPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(30, 30, 30, 30);
    outer->addStretch();
    auto *center = new QHBoxLayout;
    center->addStretch();
    auto *panel = new QFrame(this);
    panel->setProperty("card", true);
    panel->setObjectName(QStringLiteral("brandCard"));
    panel->setFixedWidth(430);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(36, 32, 36, 32);
    layout->setSpacing(16);
    auto *brand = new QLabel(QStringLiteral("Lumen IM"), panel);
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
    m_login->setProperty("primary", true);
    m_register = new QPushButton(QStringLiteral("创建账号"), panel);
    buttons->addWidget(m_register);
    buttons->addWidget(m_login);
    layout->addLayout(buttons);
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
    const AuthSession saved = m_store->loadSession();
    if (!saved.user.username.isEmpty())
        m_username->setText(saved.user.username.toUpper());
}

void LoginPage::setUsername(const QString &username)
{
    if (!username.isEmpty())
        m_username->setText(username.toUpper());
}

void LoginPage::setBusy(bool busy)
{
    m_login->setEnabled(!busy);
    m_register->setEnabled(!busy);
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
    auto callback = [guarded](bool ok, const AuthSession &session, const QString &error) {
        if (!guarded || guarded->m_closing)
            return;
        guarded->setBusy(false);
        if (!ok) {
            guarded->m_status->setText(error);
            return;
        }
        QString saveError;
        if (!guarded->m_store->saveSession(session, &saveError)) {
            guarded->m_status->setText(QStringLiteral("无法保存会话：") + saveError);
            return;
        }
        guarded->m_password->clear();
        emit guarded->authenticated(session);
    };
    if (registration)
        m_api->registerUser(username, password, std::move(callback));
    else
        m_api->login(username, password, std::move(callback));
}

void LoginPage::shutdown()
{
    m_closing = true;
}
