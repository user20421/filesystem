#include "SimulatorWindow.h"

#include "MainWindow.h"
#include "SessionPage.h"
#include "SystemTestPage.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTimer>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

SimulatorWindow::SimulatorWindow(const QString &dataRoot, const QUrl &serverUrl, QWidget *parent)
    : QMainWindow(parent)
{
    Q_UNUSED(dataRoot) // The simulator always stores users below runtime/users.
    setObjectName(QStringLiteral("simulatorWindow"));
    setWindowTitle(QStringLiteral("即时通讯和文件传输工具"));
    resize(1280, 820);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("sessionTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setUsesScrollButtons(true);
    setCentralWidget(m_tabs);

    m_add = new QToolButton(this);
    m_add->setObjectName(QStringLiteral("addTabButton"));
    m_add->setText(QStringLiteral("+"));
    m_add->setToolTip(QStringLiteral("新建用户标签  Ctrl+T"));
    m_tabs->setCornerWidget(m_add, Qt::TopRightCorner);
    connect(m_add, &QToolButton::clicked, this, [this] { addSession(); });

    auto *newTab = new QShortcut(QKeySequence::AddTab, this);
    connect(newTab, &QShortcut::activated, this, [this] { addSession(); });
    auto *closeTab = new QShortcut(QKeySequence::Close, this);
    connect(closeTab, &QShortcut::activated, this, [this] {
        closePage(qobject_cast<SessionPage *>(m_tabs->currentWidget()));
    });
    auto *nextTab = new QShortcut(QKeySequence::NextChild, this);
    connect(nextTab, &QShortcut::activated, this, [this] {
        if (m_tabs->count() > 0)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    auto *previousTab = new QShortcut(QKeySequence::PreviousChild, this);
    connect(previousTab, &QShortcut::activated, this, [this] {
        if (m_tabs->count() > 0)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
    });

    m_systemTest = new SystemTestPage(m_tabs);
    m_tabs->addTab(m_systemTest, QStringLiteral("系统测试"));
    m_tabs->addTab(createWelcomePage(), QStringLiteral("欢迎"));
    for (int index = 0; index < 2; ++index)
        m_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
    const QString root = QDir::current().filePath(QStringLiteral("runtime/users"));
    m_root->setText(QDir::cleanPath(QDir(root).absolutePath()));
    m_server->setText((serverUrl.isValid() && !serverUrl.isEmpty())
        ? serverUrl.toString() : qEnvironmentVariable("SECURE_IM_SERVER_URL",
            QStringLiteral("http://127.0.0.1:8080")));
    m_nextNumber = nextUserNumber(m_root->text());

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closePage(qobject_cast<SessionPage *>(m_tabs->widget(index)));
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { activateCurrentTab(); });
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (m_restoringTabMove || (from >= 2 && to >= 2))
            return;
        m_restoringTabMove = true;
        m_tabs->tabBar()->moveTab(to, from); // Keep system test and welcome pinned first.
        m_restoringTabMove = false;
        activateCurrentTab();
    });
}

QWidget *SimulatorWindow::createWelcomePage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("loginPage"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(40, 40, 40, 40);
    outer->addStretch();
    auto *center = new QHBoxLayout;
    center->addStretch();

    auto *panel = new QFrame(page);
    panel->setProperty("card", true);
    panel->setFixedWidth(560);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(42, 36, 42, 36);
    layout->setSpacing(18);
    auto *title = new QLabel(QStringLiteral("一个窗口，多个独立身份"), panel);
    title->setObjectName(QStringLiteral("brandTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto *description = new QLabel(QStringLiteral(
        "每个标签都有独立的登录、实时连接、消息历史和文件传输。\n切换标签不会中断后台会话。"), panel);
    description->setObjectName(QStringLiteral("mutedLabel"));
    description->setAlignment(Qt::AlignCenter);
    layout->addWidget(description);
    m_root = new QLineEdit(panel);
    m_root->setObjectName(QStringLiteral("dataRootEdit"));
    m_root->setReadOnly(true);
    m_root->setToolTip(QStringLiteral("数据目录固定为 runtime/users"));
    m_server = new QLineEdit(panel);
    m_server->setObjectName(QStringLiteral("serverUrlEdit"));
    m_server->setReadOnly(true);
    m_server->setToolTip(QStringLiteral("本地演示服务器固定为 127.0.0.1:8080"));
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("数据目录"), m_root);
    form->addRow(QStringLiteral("服务器"), m_server);
    layout->addLayout(form);
    auto *one = new QPushButton(QStringLiteral("新增用户"), panel);
    one->setProperty("primary", true);
    layout->addWidget(one);
    auto *hint = new QLabel(QStringLiteral("Ctrl+T 新建  ·  Ctrl+W 关闭  ·  Ctrl+Tab 切换"), panel);
    hint->setObjectName(QStringLiteral("mutedLabel"));
    hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(hint);
    center->addWidget(panel);
    center->addStretch();
    outer->addLayout(center);
    outer->addStretch();
    connect(one, &QPushButton::clicked, this, [this] { addSession(); });
    return page;
}

QUrl SimulatorWindow::serverUrl() const
{
    return QUrl(m_server->text().trimmed());
}

int SimulatorWindow::nextUserNumber(const QString &root) const
{
    int highest = 0;
    const QDir directory(root);
    const QStringList entries = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QRegularExpression pattern(QStringLiteral("^user_(\\d+)$"));
    for (const QString &entry : entries) {
        const QRegularExpressionMatch match = pattern.match(entry);
        if (match.hasMatch())
            highest = qMax(highest, match.captured(1).toInt());
    }
    return highest + 1;
}

SessionPage *SimulatorWindow::createSession(bool demo)
{
    const int number = m_nextNumber;
    const QString name = QStringLiteral("user_%1").arg(number);
    const QString dataDir = QDir(m_root->text()).filePath(name);
    auto context = std::make_unique<SessionContext>(dataDir, serverUrl(), name);
    QString error;
    if (!context->open(&error)) {
        QMessageBox::warning(this, QStringLiteral("无法新建用户标签"), error);
        return nullptr;
    }
    ++m_nextNumber; // Never reuse a number during this simulator's lifetime.
    auto *page = new SessionPage(std::move(context));
    const int index = m_tabs->addTab(page, name.toUpper());
    m_tabs->setTabToolTip(index, QDir::toNativeSeparators(dataDir));
    connect(page, &SessionPage::titleChanged, this, [this, page] { updateTab(page); });
    connect(page, &SessionPage::connectionStateChanged, this, [this, page] { updateTab(page); });
    connect(page, &SessionPage::exitRequested, this, [this, page] {
        if (closePage(page))
            m_tabs->setCurrentIndex(1); // Return to the pinned welcome page.
    });
    connect(page, &SessionPage::unreadCountChanged, this, [this, page](int count) {
        page->setProperty("unreadCount", count);
        updateTab(page);
    });
    if (demo) {
        AuthSession session;
        session.accessToken = QStringLiteral("demo-token-%1").arg(number);
        session.refreshToken = QStringLiteral("demo-refresh-%1").arg(number);
        session.user.id = QStringLiteral("demo-id-%1").arg(number);
        session.user.username = number == 1 ? QStringLiteral("alice") : QStringLiteral("user_%1").arg(number);
        session.user.displayName = number == 1 ? QStringLiteral("Alice Zhang") : QStringLiteral("演示用户 %1").arg(number);
        page->startDemo(session);
    }
    m_tabs->setCurrentIndex(index);
    updateTab(page);
    QTimer::singleShot(0, this, [this, guarded = QPointer<SessionPage>(page)] {
        if (guarded)
            updateTab(guarded);
    });
    return page;
}

SessionPage *SimulatorWindow::addSession(bool demo)
{
    return createSession(demo);
}

QList<SessionPage *> SimulatorWindow::addSessions(int count, bool demo)
{
    QList<SessionPage *> result;
    for (int index = 0; index < count; ++index) {
        if (SessionPage *page = createSession(demo))
            result.append(page);
    }
    return result;
}

int SimulatorWindow::sessionCount() const
{
    int count = 0;
    for (int index = 0; index < m_tabs->count(); ++index)
        count += qobject_cast<SessionPage *>(m_tabs->widget(index)) ? 1 : 0;
    return count;
}

SessionPage *SimulatorWindow::sessionAt(int index) const
{
    for (int tab = 0; tab < m_tabs->count(); ++tab) {
        if (auto *page = qobject_cast<SessionPage *>(m_tabs->widget(tab))) {
            if (index-- == 0)
                return page;
        }
    }
    return nullptr;
}

bool SimulatorWindow::closeSession(int index)
{
    return closePage(sessionAt(index));
}

bool SimulatorWindow::closePage(SessionPage *page)
{
    if (!page)
        return false;
    const int tabIndex = m_tabs->indexOf(page);
    if (tabIndex < 0)
        return false;
    page->setActive(false);
    page->shutdown();
    m_tabs->removeTab(tabIndex);
    delete page; // Release the directory lock before another tab can reuse it.
    activateCurrentTab();
    return true;
}

void SimulatorWindow::activateCurrentTab()
{
    for (int index = 0; index < sessionCount(); ++index) {
        if (SessionPage *page = sessionAt(index))
            page->setActive(m_tabs->currentWidget() == page);
    }
}

void SimulatorWindow::updateTab(SessionPage *page)
{
    const int index = m_tabs->indexOf(page);
    if (index < 0)
        return;
    QString title;
    QString state = QStringLiteral("未登录");
    int unread = 0;
    if (page->isAuthenticated()) {
        title = page->context()->session().user.label();
        state = page->chatPage()->connectionState();
        unread = page->property("unreadCount").toInt();
    } else {
        title = page->context()->preferredUsername().toUpper();
    }
    unread = page->property("unreadCount").toInt();
    QString shortState;
    if (page->isAuthenticated()) {
        if (state.contains(QStringLiteral("已连接")) || state.contains(QStringLiteral("演示")))
            shortState = QStringLiteral(" · 在线");
        else if (state.contains(QStringLiteral("…")))
            shortState = QStringLiteral(" · 连接中");
        else
            shortState = QStringLiteral(" · 离线");
    }
    QString text = QStringLiteral("%1 %2%3")
        .arg(page->isAuthenticated() ? QStringLiteral("●") : QStringLiteral("○"), title, shortState);
    if (unread > 0)
        text += QStringLiteral("  · %1").arg(unread);
    m_tabs->setTabText(index, text);
    m_tabs->setTabToolTip(index, QStringLiteral("%1\n%2\n%3")
        .arg(title, state, QDir::toNativeSeparators(page->context()->dataDir())));
}

bool SimulatorWindow::savePreview(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QPixmap preview = grab();
    return !preview.isNull() && preview.save(path, "PNG");
}

void SimulatorWindow::closeEvent(QCloseEvent *event)
{
    if (m_systemTest)
        m_systemTest->shutdown();
    for (int index = 0; index < sessionCount(); ++index) {
        if (SessionPage *page = sessionAt(index))
            page->shutdown();
    }
    QMainWindow::closeEvent(event);
}
