#include "MainWindow.h"

#include "LocalStore.h"
#include "TransferTaskCard.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QUuid>

namespace {
QString conversationId(const QString &first, const QString &second)
{
    QStringList ids{first, second};
    ids.sort();
    return ids.join(QLatin1Char(':'));
}

QString formatTime(qint64 timestamp)
{
    if (timestamp > 0 && timestamp < 1000000000000LL)
        timestamp *= 1000;
    return timestamp > 0
        ? QDateTime::fromMSecsSinceEpoch(timestamp).toString(QStringLiteral("MM-dd HH:mm"))
        : QString{};
}

bool cursorAtMost(const QString &value, const QString &limit)
{
    bool valueOk = false;
    bool limitOk = false;
    const qulonglong left = value.toULongLong(&valueOk);
    const qulonglong right = limit.toULongLong(&limitOk);
    return valueOk && limitOk && left <= right;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

ChatMessage payloadMessage(const QJsonObject &payload)
{
    ChatMessage message;
    message.kind = payload.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    message.content = payload.value(QStringLiteral("text")).toString();
    message.fileId = payload.value(QStringLiteral("file_id")).toString();
    message.fileName = payload.value(QStringLiteral("file_name")).toString();
    message.fileSize = payload.value(QStringLiteral("size")).toVariant().toLongLong();
    return message;
}

QFrame *card(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setProperty("card", true);
    return frame;
}

class ContactItemWidget final : public QWidget {
public:
    ContactItemWidget(const QString &name, const QString &presence, int unread, QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 7, 10, 7);
        layout->setSpacing(3);
        auto *nameRow = new QHBoxLayout;
        nameRow->setSpacing(7);
        auto *nameLabel = new QLabel(name, this);
        nameLabel->setObjectName(QStringLiteral("contactNameLabel"));
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameRow->addWidget(nameLabel);
        m_unreadDot = new QLabel(this);
        m_unreadDot->setObjectName(QStringLiteral("contactUnreadDot"));
        m_unreadDot->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_unreadDot->setFixedSize(10, 10);
        m_unreadDot->setStyleSheet(QStringLiteral(
            "background: #ef4444; border: none; border-radius: 5px;"));
        nameRow->addWidget(m_unreadDot, 0, Qt::AlignVCenter);
        nameRow->addStretch(1);
        layout->addLayout(nameRow);
        auto *presenceLabel = new QLabel(presence, this);
        presenceLabel->setObjectName(QStringLiteral("contactPresenceLabel"));
        presenceLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(presenceLabel);
        setUnread(unread);
    }

    void setUnread(int count)
    {
        setProperty("unreadCount", count);
        m_unreadDot->setVisible(count > 0);
        m_unreadDot->setToolTip(QStringLiteral("%1 条未读消息").arg(count));
    }

private:
    QLabel *m_unreadDot = nullptr;
};

QListWidgetItem *createContactItem(QListWidget *list, const User &user,
                                   const QString &presence, int unread)
{
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::AccessibleTextRole, QStringLiteral("%1, %2").arg(user.label(), presence));
    item->setData(Qt::UserRole, user.id);
    item->setData(Qt::UserRole + 1, QStringList{user.label(), user.username,
                                               user.displayName, user.id}.join(QLatin1Char(' ')));
    item->setToolTip(QStringLiteral("与 %1 发起安全会话 · %2")
        .arg(user.label(), presence));
    auto *widget = new ContactItemWidget(user.label(), presence, unread, list);
    list->setItemWidget(item, widget);
    QSize sizeHint = widget->sizeHint();
    sizeHint.setHeight(58);
    item->setSizeHint(sizeHint);
    return item;
}
} // namespace

MainWindow::DirectoryOpener MainWindow::s_directoryOpener;

void MainWindow::setDirectoryOpenerForTests(DirectoryOpener opener)
{
    s_directoryOpener = std::move(opener);
}

MainWindow::MainWindow(ApiClient *api, LocalStore *store, WireCodec *codec,
                       const AuthSession &session, bool online, QWidget *parent)
    : QWidget(parent), m_api(api), m_store(store), m_codec(codec), m_session(session),
      m_socket(new WebSocketClient(codec, this)),
      m_transfers(new TransferManager(api, store, this)), m_online(online)
{
    setObjectName(QStringLiteral("sessionChatPage"));
    m_api->setAccessToken(m_session.accessToken);
    buildUi();
    updateSessionLabel();

    connect(m_usersTimer, &QTimer::timeout, this, &MainWindow::loadUsers);
    connect(m_historyTimer, &QTimer::timeout, this, &MainWindow::loadHistory);
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::connectSocket);
    connect(m_users, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { selectPeer(); });
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::filterUsers);
    connect(m_transfers, &TransferManager::taskAdded, this, &MainWindow::addTransferRow);
    connect(m_transfers, &TransferManager::taskChanged, this, &MainWindow::updateTransferRow);
    connect(m_transfers, &TransferManager::uploadCompleted, this, [this](TransferTask *task) {
        if (m_closing)
            return;
        if (!sendPayload(task->peerId(), filePayload(task->fileId(), task->fileName(), task->size())))
            showNotice(QStringLiteral("文件已上传，但文件消息尚未发送"));
    });
    connect(m_socket, &WebSocketClient::connected, this, [this] {
        setConnectionState(QStringLiteral("已连接"));
    });
    connect(m_socket, &WebSocketClient::disconnected, this, [this] {
        if (m_closing)
            return;
        setConnectionState(QStringLiteral("已断开"));
        scheduleReconnect();
    });
    connect(m_socket, &WebSocketClient::errorOccurred, this, [this](const QString &error) {
        if (!m_closing)
            setConnectionState(error);
    });
    connect(m_socket, &WebSocketClient::protocolError, this, [this](const QString &error) {
        if (!m_closing)
            showNotice(QStringLiteral("协议错误：") + error);
    });
    connect(m_socket, &WebSocketClient::messageReceived, this, &MainWindow::handleWireMessage);

    if (online) {
        loadUsers();
        m_usersTimer->start();
        m_historyTimer->start();
        QTimer::singleShot(0, this, &MainWindow::connectSocket);
    } else {
        setConnectionState(QStringLiteral("演示连接"));
    }
}

MainWindow::~MainWindow()
{
    shutdown();
}

void MainWindow::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    auto *splitter = new QSplitter(this);
    splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter, 1);

    auto *left = card(splitter);
    left->setMinimumWidth(230);
    left->setMaximumWidth(330);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(16, 16, 16, 16);
    leftLayout->setSpacing(12);
    auto *contactsTitle = new QLabel(QStringLiteral("联系人"), left);
    contactsTitle->setObjectName(QStringLiteral("sectionTitle"));
    leftLayout->addWidget(contactsTitle);
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("搜索姓名或用户 ID"));
    m_search->setClearButtonEnabled(true);
    leftLayout->addWidget(m_search);
    m_users = new QListWidget(left);
    m_users->setObjectName(QStringLiteral("contactList"));
    m_users->setSpacing(4);
    leftLayout->addWidget(m_users, 1);
    auto *summary = new QLabel(QStringLiteral("每个用户拥有独立会话与本地历史"), left);
    summary->setObjectName(QStringLiteral("mutedLabel"));
    summary->setWordWrap(true);
    leftLayout->addWidget(summary);

    auto *right = card(splitter);
    auto *layout = new QVBoxLayout(right);
    layout->setContentsMargins(20, 16, 20, 18);
    layout->setSpacing(12);
    auto *top = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    m_sessionLabel = new QLabel(right);
    m_sessionLabel->setObjectName(QStringLiteral("sessionTitle"));
    m_conversationLabel = new QLabel(QStringLiteral("选择联系人开始会话"), right);
    m_conversationLabel->setObjectName(QStringLiteral("mutedLabel"));
    titles->addWidget(m_sessionLabel);
    titles->addWidget(m_conversationLabel);
    top->addLayout(titles, 1);
    m_socketLabel = new QLabel(right);
    m_socketLabel->setObjectName(QStringLiteral("connectionBadge"));
    top->addWidget(m_socketLabel);
    auto *exit = new QPushButton(QStringLiteral("退出"), right);
    exit->setObjectName(QStringLiteral("exitButton"));
    exit->setToolTip(QStringLiteral("退出当前账号并关闭标签，不删除服务器账号或历史文件"));
    top->addWidget(exit);
    connect(exit, &QPushButton::clicked, this, &MainWindow::requestExit);
    m_deleteAccountButton = new QPushButton(QStringLiteral("注销"), right);
    m_deleteAccountButton->setObjectName(QStringLiteral("deleteAccountButton"));
    m_deleteAccountButton->setToolTip(QStringLiteral("永久删除服务器账号及关联消息和文件，不可恢复"));
    top->addWidget(m_deleteAccountButton);
    connect(m_deleteAccountButton, &QPushButton::clicked,
            this, &MainWindow::requestAccountDeletion);
    layout->addLayout(top);

    auto *navigation = new QHBoxLayout;
    m_chatNav = new QPushButton(QStringLiteral("消息"), right);
    m_transferNav = new QPushButton(QStringLiteral("文件传输"), right);
    for (QPushButton *button : {m_chatNav, m_transferNav}) {
        button->setCheckable(true);
        button->setProperty("navigation", true);
        navigation->addWidget(button);
    }
    m_chatNav->setChecked(true);
    navigation->addStretch();
    layout->addLayout(navigation);

    m_views = new QStackedWidget(right);
    layout->addWidget(m_views, 1);
    auto *chatView = new QWidget(m_views);
    auto *chatLayout = new QVBoxLayout(chatView);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(10);
    m_chat = new QTextBrowser(chatView);
    m_chat->setObjectName(QStringLiteral("chatTranscript"));
    m_chat->setOpenExternalLinks(false);
    chatLayout->addWidget(m_chat, 1);
    m_composer = new QFrame(chatView);
    m_composer->setObjectName(QStringLiteral("composer"));
    m_composer->setVisible(false); // No message can be sent until a peer is selected.
    auto *composerLayout = new QHBoxLayout(m_composer);
    composerLayout->setContentsMargins(8, 6, 8, 6);
    m_input = new QLineEdit(m_composer);
    m_input->setPlaceholderText(QStringLiteral("选择用户后输入消息，按 Enter 发送"));
    auto *send = new QPushButton(QStringLiteral("发送"), m_composer);
    send->setProperty("primary", true);
    auto *attach = new QPushButton(QStringLiteral("添加文件"), m_composer);
    composerLayout->addWidget(m_input, 1);
    composerLayout->addWidget(attach);
    composerLayout->addWidget(send);
    chatLayout->addWidget(m_composer);
    m_views->addWidget(chatView);

    auto *transferView = new QWidget(m_views);
    auto *transferLayout = new QVBoxLayout(transferView);
    transferLayout->setContentsMargins(0, 0, 0, 0);
    transferLayout->setSpacing(10);
    auto *transferHeadingRow = new QHBoxLayout;
    auto *transferHeading = new QLabel(QStringLiteral("传输任务"), transferView);
    transferHeading->setObjectName(QStringLiteral("sectionTitle"));
    transferHeadingRow->addWidget(transferHeading);
    transferHeadingRow->addStretch(1);
    m_transferStats = new QLabel(transferView);
    m_transferStats->setObjectName(QStringLiteral("transferStats"));
    transferHeadingRow->addWidget(m_transferStats);
    transferLayout->addLayout(transferHeadingRow);

    m_transferEmpty = new QLabel(QStringLiteral(
        "还没有传输任务\n在消息页选择联系人并点击“添加文件”，或等待接收文件"), transferView);
    m_transferEmpty->setObjectName(QStringLiteral("transferEmptyState"));
    m_transferEmpty->setAlignment(Qt::AlignCenter);
    transferLayout->addWidget(m_transferEmpty, 1);

    m_transferList = new QTreeWidget(transferView);
    m_transferList->setObjectName(QStringLiteral("transferTable"));
    m_transferList->setColumnCount(1);
    m_transferList->setHeaderHidden(true);
    m_transferList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_transferList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_transferList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transferList->setRootIsDecorated(false);
    m_transferList->setIndentation(0);
    m_transferList->setUniformRowHeights(false);
    m_transferList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_transferList->setFocusPolicy(Qt::StrongFocus);
    connect(m_transferList, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        for (int index = 0; index < m_transferList->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = m_transferList->topLevelItem(index);
            if (auto *widget = qobject_cast<TransferTaskCard *>(m_transferList->itemWidget(item, 0)))
                widget->setSelected(item == current);
        }
    });
    transferLayout->addWidget(m_transferList, 1);
    updateTransferSummary();

    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("并发"), transferView));
    m_concurrency = new QSpinBox(transferView);
    m_concurrency->setRange(3, 5);
    m_concurrency->setValue(m_transfers->concurrency());
    controls->addWidget(m_concurrency);
    auto *openDirectory = new QPushButton(QStringLiteral("打开目录"), transferView);
    openDirectory->setObjectName(QStringLiteral("openTransferDirectoryButton"));
    openDirectory->setToolTip(QStringLiteral("打开选中文件所在的本地目录"));
    controls->addWidget(openDirectory);
    connect(openDirectory, &QPushButton::clicked, this, [this] {
        if (m_closing)
            return;
        const QTreeWidgetItem *item = m_transferList->currentItem();
        if (item)
            openTransferDirectory(item->data(0, Qt::UserRole + 1).toString());
    });
    const QList<QPair<QString, QString>> actions{
        {QStringLiteral("暂停"), QStringLiteral("pause")},
        {QStringLiteral("继续"), QStringLiteral("resume")},
        {QStringLiteral("重试"), QStringLiteral("retry")},
        {QStringLiteral("取消"), QStringLiteral("cancel")}};
    for (const auto &action : actions) {
        auto *button = new QPushButton(action.first, transferView);
        button->setObjectName(QStringLiteral("transfer%1Button").arg(
            action.second.at(0).toUpper() + action.second.mid(1)));
        controls->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action]() {
            if (m_closing)
                return;
            const QString id = selectedTransferId();
            if (id.isEmpty())
                return;
            if (action.second == QLatin1String("pause"))
                m_transfers->pause(id);
            else if (action.second == QLatin1String("resume"))
                m_transfers->resume(id);
            else if (action.second == QLatin1String("retry"))
                m_transfers->retry(id);
            else
                m_transfers->cancel(id);
        });
    }
    controls->addStretch();
    transferLayout->addLayout(controls);
    m_views->addWidget(transferView);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setSizes({260, 820});

    m_notice = new QLabel(right);
    m_notice->setObjectName(QStringLiteral("noticeLabel"));
    m_notice->setWordWrap(true);
    m_notice->hide();
    layout->addWidget(m_notice);

    m_usersTimer = new QTimer(this);
    m_usersTimer->setInterval(10000);
    m_historyTimer = new QTimer(this);
    m_historyTimer->setInterval(3000);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    m_reconnectTimer->setSingleShot(true);

    connect(send, &QPushButton::clicked, this, &MainWindow::sendText);
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendText);
    connect(attach, &QPushButton::clicked, this, &MainWindow::chooseFile);
    connect(m_chatNav, &QPushButton::clicked, this, &MainWindow::showChatView);
    connect(m_transferNav, &QPushButton::clicked, this, &MainWindow::showTransfersView);
    connect(m_concurrency, qOverload<int>(&QSpinBox::valueChanged),
            m_transfers, &TransferManager::setConcurrency);
}

void MainWindow::updateSessionLabel()
{
    m_sessionLabel->setText(m_session.user.label());
}

void MainWindow::requestExit()
{
    if (m_closing || m_exitPending || m_accountDeletionPending)
        return;
    m_exitPending = true;
    QPointer<MainWindow> guarded(this);
    QMessageBox prompt(QMessageBox::Question, QStringLiteral("退出"),
        QStringLiteral("确定要退出 %1 吗？将清除本地登录会话并关闭该标签页，不会删除服务器账号。")
            .arg(m_session.user.username.toUpper()),
        QMessageBox::Yes | QMessageBox::No, nullptr);
    prompt.button(QMessageBox::Yes)->setText(QStringLiteral("是"));
    prompt.button(QMessageBox::No)->setText(QStringLiteral("否"));
    prompt.setDefaultButton(QMessageBox::No);
    const auto answer = static_cast<QMessageBox::StandardButton>(prompt.exec());
    if (!guarded)
        return;
    m_exitPending = false;
    if (answer != QMessageBox::Yes || m_closing)
        return;
    m_store->clearSession();
    emit exitRequested(); // The receiver closes this page; do not touch members afterwards.
}

void MainWindow::requestAccountDeletion()
{
    if (m_closing || m_accountDeletionPending || m_exitPending)
        return;
    m_accountDeletionPending = true;
    QPointer<MainWindow> guarded(this);
    QMessageBox prompt(QMessageBox::Warning, QStringLiteral("永久注销账号"),
        QStringLiteral("确定要永久删除服务器账号 %1 及其关联消息和文件吗？此操作不可恢复。")
            .arg(m_session.user.username.toUpper()),
        QMessageBox::Yes | QMessageBox::No, nullptr);
    prompt.button(QMessageBox::Yes)->setText(QStringLiteral("永久删除"));
    prompt.button(QMessageBox::No)->setText(QStringLiteral("否"));
    prompt.setDefaultButton(QMessageBox::No);
    const auto answer = static_cast<QMessageBox::StandardButton>(prompt.exec());
    if (!guarded)
        return;
    if (answer != QMessageBox::Yes || m_closing) {
        m_accountDeletionPending = false;
        return;
    }

    m_deleteAccountButton->setEnabled(false);
    m_api->deleteCurrentAccount([this, guarded](bool ok, const QString &error) {
        if (!guarded || m_closing)
            return;
        m_accountDeletionPending = false;
        m_deleteAccountButton->setEnabled(true);
        if (!ok) {
            showNotice(QStringLiteral("注销失败：%1。账号和本地会话仍保留，可重试。").arg(error));
            return;
        }
        m_store->clearSession();
        m_store->forgetAccount(m_session.user.id);
        emit exitRequested(); // Only a 204 response reaches this point.
    });
}

void MainWindow::setConnectionState(const QString &state)
{
    m_connectionState = state;
    if (m_socketLabel)
        m_socketLabel->setText(QStringLiteral("●  ") + state);
    emit connectionStateChanged(state);
}

void MainWindow::showNotice(const QString &notice)
{
    if (!m_notice)
        return;
    m_notice->setText(notice);
    m_notice->setVisible(!notice.isEmpty());
    emit noticeChanged(notice);
}

void MainWindow::setActive(bool active)
{
    m_active = active;
    if (active) {
        clearPeerUnread(m_peerId);
        markLatestIncomingRead(m_peerId);
    }
}

void MainWindow::showChatView()
{
    m_views->setCurrentIndex(0);
    m_chatNav->setChecked(true);
    m_transferNav->setChecked(false);
    clearPeerUnread(m_peerId);
    markLatestIncomingRead(m_peerId);
}

void MainWindow::showTransfersView()
{
    m_views->setCurrentIndex(1);
    m_chatNav->setChecked(false);
    m_transferNav->setChecked(true);
}

void MainWindow::loadUsers()
{
    if (m_closing)
        return;
    loadPresence();
    QPointer<MainWindow> guarded(this);
    m_api->users([this, guarded](bool ok, const QList<User> &users, const QString &error) {
        if (!guarded || m_closing)
            return;
        if (!ok) {
            showNotice(QStringLiteral("用户列表：") + error);
            return;
        }
        applyUsers(users);
    });
}

void MainWindow::loadPresence()
{
    if (m_closing || !m_online)
        return;
    QPointer<MainWindow> guarded(this);
    m_api->presence([this, guarded](bool ok, const QSet<QString> &online, const QString &) {
        if (!guarded || m_closing || !ok)
            return; // A transient failure must not erase the last known state.
        m_onlineUserIds = online;
        m_presenceKnown = true;
        applyUsers(m_contacts.values());
    });
}

void MainWindow::applyUsers(const QList<User> &users)
{
    const QString selected = m_peerId;
    m_users->blockSignals(true);
    m_users->clear();
    m_userNames.clear();
    m_contacts.clear();
    QListWidgetItem *select = nullptr;
    const QSet<QString> knownAccountIds = m_store->knownAccountIds();
    for (User user : users) {
        // Keep simulator accounts and accounts authenticated on this workstation.
        // Unknown integration fixtures and arbitrary historical accounts stay hidden.
        static const QRegularExpression visibleUserPattern(QStringLiteral("^USER_[0-9]+$"));
        const bool currentAccount = user.id == m_session.user.id;
        const bool eligible = currentAccount || knownAccountIds.contains(user.id)
            || visibleUserPattern.match(user.username.toUpper()).hasMatch();
        if (!eligible || currentAccount)
            continue;
        user.presenceKnown = m_presenceKnown;
        user.online = m_presenceKnown && m_onlineUserIds.contains(user.id);
        m_contacts.insert(user.id, user);
        m_userNames.insert(user.id, user.label());
        const QString presence = !user.presenceKnown ? QStringLiteral("状态未知")
            : user.online ? QStringLiteral("在线") : QStringLiteral("离线");
        auto *item = createContactItem(m_users, user, presence, m_unreadByPeer.value(user.id));
        if (user.id == selected)
            select = item;
    }
    filterUsers(m_search->text());
    if (select)
        m_users->setCurrentItem(select);
    m_users->blockSignals(false);
    if (select)
        selectPeer();
    else if (!m_peerId.isEmpty()) {
        m_peerId.clear();
        m_composer->setVisible(false);
        m_chat->clear();
    }
    updateConversationLabel();
}

void MainWindow::setPeerUnread(const QString &peerId, int count)
{
    if (peerId.isEmpty() || m_closing)
        return;
    count = qMax(0, count);
    if (count == 0)
        m_unreadByPeer.remove(peerId);
    else
        m_unreadByPeer.insert(peerId, count);
    for (int index = 0; index < m_users->count(); ++index) {
        QListWidgetItem *item = m_users->item(index);
        if (item->data(Qt::UserRole).toString() != peerId)
            continue;
        if (auto *widget = static_cast<ContactItemWidget *>(m_users->itemWidget(item)))
            widget->setUnread(count);
        break;
    }
    int total = 0;
    for (auto it = m_unreadByPeer.cbegin(); it != m_unreadByPeer.cend(); ++it)
        total += it.value();
    if (total != m_unreadCount) {
        m_unreadCount = total;
        emit unreadCountChanged(total);
    }
}

void MainWindow::clearPeerUnread(const QString &peerId)
{
    if (canMarkRead(peerId))
        setPeerUnread(peerId, 0);
}

void MainWindow::updateConversationLabel()
{
    if (m_peerId.isEmpty()) {
        m_conversationLabel->setText(QStringLiteral("选择联系人开始会话"));
        return;
    }
    const User user = m_contacts.value(m_peerId);
    const QString presence = !user.presenceKnown ? QStringLiteral("状态未知")
        : user.online ? QStringLiteral("在线") : QStringLiteral("离线");
    m_conversationLabel->setText(QStringLiteral("与 %1 的会话 · %2")
        .arg(m_userNames.value(m_peerId, m_peerId.toUpper()), presence));
}

void MainWindow::filterUsers(const QString &text)
{
    const QString needle = text.trimmed();
    const bool blocked = m_users->blockSignals(true);
    QListWidgetItem *selected = nullptr;
    for (int index = 0; index < m_users->count(); ++index) {
        QListWidgetItem *item = m_users->item(index);
        const QString searchable = item->data(Qt::UserRole + 1).toString();
        item->setHidden(!needle.isEmpty() && !searchable.contains(needle, Qt::CaseInsensitive));
        if (item->data(Qt::UserRole).toString() == m_peerId)
            selected = item;
    }
    if (selected)
        m_users->setCurrentItem(selected);
    m_users->blockSignals(blocked);
    clearPeerUnread(m_peerId);
}

void MainWindow::selectPeer()
{
    if (m_closing)
        return;
    auto *item = m_users->currentItem();
    m_peerId = item ? item->data(Qt::UserRole).toString() : QString{};
    m_composer->setVisible(!m_peerId.isEmpty());
    m_chat->clear();
    if (m_peerId.isEmpty())
        return;
    updateConversationLabel();
    clearPeerUnread(m_peerId);
    const QList<ChatMessage> history = m_store->messages(m_peerId);
    for (const ChatMessage &message : history) {
        m_sync.accept(m_peerId, message);
        renderMessage(message);
    }
    loadHistory();
    markLatestIncomingRead(m_peerId);
}

void MainWindow::loadHistory()
{
    if (m_closing || !m_online || m_peerId.isEmpty())
        return;
    const QString peer = m_peerId;
    QPointer<MainWindow> guarded(this);
    m_api->messages(peer, m_sync.cursor(peer),
                    [this, peer, guarded](bool ok, const QList<ChatMessage> &messages, const QString &error) {
        if (!guarded || m_closing)
            return;
        if (!ok) {
            showNotice(QStringLiteral("消息同步：") + error);
            return;
        }
        m_synchronizingHistory = true;
        for (const ChatMessage &message : messages)
            appendChat(peer, message);
        m_synchronizingHistory = false;
        markLatestIncomingRead(peer);
    });
}

void MainWindow::connectSocket()
{
    if (m_closing)
        return;
    if (!m_codec->available()) {
        setConnectionState(m_codec->unavailableReason());
        return;
    }
    if (m_socket->isConnected() || m_ticketPending)
        return;
    m_ticketPending = true;
    setConnectionState(QStringLiteral("获取票据…"));
    QPointer<MainWindow> guarded(this);
    m_api->wsTicket([this, guarded](bool ok, const QString &ticket, const QString &error) {
        if (!guarded || m_closing)
            return;
        m_ticketPending = false;
        if (!ok) {
            setConnectionState(error);
            scheduleReconnect();
            return;
        }
        m_socket->open(m_api->webSocketUrl(ticket));
    });
}

void MainWindow::scheduleReconnect()
{
    if (!m_closing && !m_reconnectTimer->isActive())
        m_reconnectTimer->start();
}

bool MainWindow::sendPayload(const QString &peerId, const QJsonObject &payload)
{
    if (m_closing || peerId.isEmpty())
        return false;
    WireMessage wire;
    wire.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    wire.conversationId = conversationId(m_session.user.id, peerId);
    wire.senderId = m_session.user.id;
    wire.recipientId = peerId;
    wire.deviceId = QStringLiteral("desktop");
    wire.timestamp = QDateTime::currentMSecsSinceEpoch();
    wire.payload = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QString error;
    if (!m_socket->send(wire, &error)) {
        if (error.isEmpty())
            error = QStringLiteral("WebSocket 未接受完整帧");
        showNotice(QStringLiteral("发送失败：") + error);
        return false;
    }
    ChatMessage message = payloadMessage(payload);
    message.id = wire.messageId;
    message.conversationId = wire.conversationId;
    message.senderId = wire.senderId;
    message.recipientId = wire.recipientId;
    message.timestamp = wire.timestamp;
    appendChat(peerId, message);
    return true;
}

void MainWindow::sendText()
{
    if (m_peerId.isEmpty()) {
        showNotice(QStringLiteral("请先选择用户"));
        return;
    }
    const QString text = m_input->text();
    if (text.isEmpty())
        return;
    if (sendPayload(m_peerId, textPayload(text)))
        m_input->clear();
}

void MainWindow::chooseFile()
{
    if (m_peerId.isEmpty()) {
        showNotice(QStringLiteral("请先选择用户"));
        return;
    }
    const QString source = QFileDialog::getOpenFileName(this, QStringLiteral("发送文件"));
    if (source.isEmpty())
        return;
    if (!m_transfers->addUpload(m_peerId, source))
        QMessageBox::warning(this, QStringLiteral("发送文件"), QStringLiteral("文件不可读"));
}

void MainWindow::handleWireMessage(const WireMessage &wire)
{
    if (m_closing)
        return;
    if (wire.type == WireMessage::Type::Read) {
        handleReadReceipt(wire);
        return;
    }
    // ACK confirms transport only; it never means that the peer has read it.
    if (wire.type != WireMessage::Type::Message)
        return;
    QJsonObject payload;
    const QJsonDocument document = QJsonDocument::fromJson(wire.payload);
    if (document.isObject())
        payload = document.object();
    else
        payload = textPayload(QString::fromUtf8(wire.payload));
    ChatMessage message = payloadMessage(payload);
    message.id = wire.messageId;
    message.conversationId = wire.conversationId;
    message.senderId = wire.senderId;
    message.recipientId = wire.recipientId;
    message.timestamp = wire.timestamp;
    message.cursor = wire.cursor;
    message.readAt = wire.readAt;
    const QString peer = wire.senderId == m_session.user.id ? wire.recipientId : wire.senderId;
    appendChat(peer, message);
}

void MainWindow::handleReadReceipt(const WireMessage &wire)
{
    if (wire.cursor.isEmpty())
        return;
    const QString peer = wire.senderId == m_session.user.id ? wire.recipientId : wire.senderId;
    if (peer.isEmpty())
        return;
    const qint64 readAt = wire.readAt > 0 ? wire.readAt
        : wire.timestamp > 0 ? wire.timestamp : QDateTime::currentMSecsSinceEpoch();
    // READ is broadcast to both participants. recipientId is the author of the
    // original MESSAGE; on the reader's own copy we must not mark our messages.
    const QString authorId = wire.recipientId;
    if (authorId == m_session.user.id) {
        const QString previous = m_readCursors.value(peer);
        if (previous.isEmpty() || cursorAtMost(previous, wire.cursor)) {
            m_readCursors.insert(peer, wire.cursor);
            m_readTimes.insert(peer, qMax(readAt, m_readTimes.value(peer)));
        }
    }
    if (m_store->markMessagesRead(peer, authorId, wire.cursor, readAt)
        && peer == m_peerId)
        renderConversation();
    // Older servers may omit read_at on READ; history is authoritative then.
    if (wire.readAt <= 0)
        loadHistory();
}

void MainWindow::appendChat(const QString &peerId, const ChatMessage &message)
{
    if (m_closing)
        return;
    ChatMessage stored = message;
    if (stored.senderId == m_session.user.id && stored.readAt <= 0
        && cursorAtMost(stored.cursor, m_readCursors.value(peerId)))
        stored.readAt = m_readTimes.value(peerId);
    const bool accepted = m_sync.accept(peerId, stored);
    const bool changed = m_store->appendMessage(peerId, stored);
    if (!accepted) {
        if (changed && peerId == m_peerId)
            renderConversation();
        if (!m_synchronizingHistory && stored.senderId != m_session.user.id)
            markLatestIncomingRead(peerId);
        return;
    }
    if (peerId == m_peerId)
        renderMessage(stored);
    if (stored.senderId != m_session.user.id) {
        if (canMarkRead(peerId))
            setPeerUnread(peerId, 0);
        else
            setPeerUnread(peerId, m_unreadByPeer.value(peerId) + 1);
    }
    if (stored.isFile() && stored.senderId != m_session.user.id
        && !m_downloadIds.contains(stored.fileId)) {
        m_downloadIds.insert(stored.fileId);
        m_transfers->addDownload(peerId, stored.fileId, stored.fileName, stored.fileSize);
    }
    if (!m_synchronizingHistory && stored.senderId != m_session.user.id)
        markLatestIncomingRead(peerId);
}

bool MainWindow::canMarkRead(const QString &peerId) const
{
    if (m_closing || !m_active || peerId.isEmpty() || peerId != m_peerId
        || !m_views || m_views->currentIndex() != 0 || !m_chat->isVisible())
        return false;
    const QListWidgetItem *selected = m_users->currentItem();
    return selected && !selected->isHidden()
        && selected->data(Qt::UserRole).toString() == peerId;
}

void MainWindow::markLatestIncomingRead(const QString &peerId)
{
    if (!m_online || !canMarkRead(peerId) || m_readRequests.contains(peerId))
        return;
    const QList<ChatMessage> history = m_store->messages(peerId);
    for (auto it = history.crbegin(); it != history.crend(); ++it) {
        if (it->senderId == m_session.user.id)
            continue;
        if (it->cursor.isEmpty()) {
            loadHistory(); // A realtime frame may not carry its database cursor yet.
            return;
        }
        if (m_sentReadCursors.value(peerId) == it->cursor)
            return;
        const QString cursor = it->cursor;
        m_readRequests.insert(peerId);
        QPointer<MainWindow> guarded(this);
        m_api->markRead(peerId, cursor,
                        [this, guarded, peerId, cursor](bool ok, const QString &) {
            if (!guarded || m_closing)
                return;
            m_readRequests.remove(peerId);
            if (ok) {
                m_sentReadCursors.insert(peerId, cursor);
                markLatestIncomingRead(peerId); // A newer frame may have arrived in flight.
            }
        });
        return;
    }
}

void MainWindow::renderConversation()
{
    m_chat->clear();
    const QList<ChatMessage> history = m_store->messages(m_peerId);
    for (const ChatMessage &message : history)
        renderMessage(message);
}

void MainWindow::renderMessage(const ChatMessage &message)
{
    const bool own = message.senderId == m_session.user.id;
    const QString sender = own ? QStringLiteral("我")
        : m_userNames.value(message.senderId, message.senderId.toUpper());
    QString time = formatTime(message.timestamp);
    if (own) {
        const QString readState = message.readAt > 0
            ? QStringLiteral("已读 %1").arg(formatTime(message.readAt)) : QStringLiteral("未读");
        time = time.isEmpty() ? readState : QStringLiteral("%1 · %2").arg(time, readState);
    }
    const QString body = message.isFile()
        ? QStringLiteral("📎 %1 <span style='color:#64748b'>%2</span>")
            .arg(message.fileName.toHtmlEscaped(), formatBytes(message.fileSize))
        : message.content.toHtmlEscaped();
    const QString color = own ? QStringLiteral("#e8efff") : QStringLiteral("#f1f5f9");
    m_chat->append(QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='7'><tr><td style='background:%1;border-radius:10px'>"
        "<b style='color:#25324b'>%2</b> <span style='color:#94a3b8;font-size:11px'>%3</span><br>"
        "<span style='color:#334155'>%4</span></td></tr></table>")
        .arg(color, sender.toHtmlEscaped(), time, body));
}

void MainWindow::addTransferRow(TransferTask *task)
{
    if (m_closing)
        return;
    auto *item = new QTreeWidgetItem(m_transferList);
    item->setData(0, Qt::UserRole, task->id());
    item->setData(0, Qt::UserRole + 1, QDir::cleanPath(task->localPath()));
    auto *widget = new TransferTaskCard(m_transferList);
    m_transferList->setItemWidget(item, 0, widget);
    connect(widget, &TransferTaskCard::clicked, this, [this, item] {
        if (!m_closing)
            m_transferList->setCurrentItem(item);
    });
    connect(widget, &TransferTaskCard::openDirectoryRequested,
            this, &MainWindow::openTransferDirectory);
    updateTransferRow(task);
    if (!m_transferList->currentItem())
        m_transferList->setCurrentItem(item);
}

void MainWindow::updateTransferRow(TransferTask *task)
{
    if (m_closing)
        return;
    QTreeWidgetItem *found = nullptr;
    for (int index = 0; index < m_transferList->topLevelItemCount(); ++index) {
        auto *item = m_transferList->topLevelItem(index);
        if (item->data(0, Qt::UserRole).toString() == task->id()) {
            found = item;
            break;
        }
    }
    if (!found)
        return;
    auto *widget = qobject_cast<TransferTaskCard *>(m_transferList->itemWidget(found, 0));
    if (!widget)
        return;
    const QString peerName = m_userNames.value(task->peerId(), task->peerId().toUpper());
    widget->setTask(*task, peerName);
    found->setData(0, Qt::UserRole + 1, QDir::cleanPath(task->localPath()));
    found->setData(0, Qt::UserRole + 2, widget->property("stateKey"));
    found->setText(0, QStringLiteral("%1 · %2 · %3").arg(task->fileName(), peerName, task->stateText()));
    found->setToolTip(0, QDir::toNativeSeparators(task->localPath()));
    found->setSizeHint(0, widget->sizeHint());
    updateTransferSummary();
}

void MainWindow::updateTransferSummary()
{
    if (!m_transferStats || !m_transferList || !m_transferEmpty)
        return;
    int active = 0;
    int completed = 0;
    int failed = 0;
    int paused = 0;
    const int count = m_transferList->topLevelItemCount();
    for (int index = 0; index < count; ++index) {
        const QString state = m_transferList->topLevelItem(index)
            ->data(0, Qt::UserRole + 2).toString();
        if (state == QLatin1String("completed"))
            ++completed;
        else if (state == QLatin1String("failed"))
            ++failed;
        else if (state == QLatin1String("paused"))
            ++paused;
        else if (state != QLatin1String("cancelled"))
            ++active;
    }
    QStringList parts{QStringLiteral("共 %1 项").arg(count)};
    if (active > 0)
        parts.append(QStringLiteral("%1 进行中").arg(active));
    if (paused > 0)
        parts.append(QStringLiteral("%1 已暂停").arg(paused));
    if (completed > 0)
        parts.append(QStringLiteral("%1 已完成").arg(completed));
    if (failed > 0)
        parts.append(QStringLiteral("%1 失败").arg(failed));
    m_transferStats->setText(parts.join(QStringLiteral("  ·  ")));
    m_transferList->setVisible(count > 0);
    m_transferEmpty->setVisible(count == 0);
}

void MainWindow::openTransferDirectory(const QString &localPath)
{
    if (m_closing || localPath.trimmed().isEmpty())
        return;
    const QString normalized = QDir::cleanPath(QFileInfo(localPath).absoluteFilePath());
    const QString directory = QDir::cleanPath(QFileInfo(normalized).absolutePath());
    if (directory.isEmpty())
        return;
    const QUrl url = QUrl::fromLocalFile(directory);
    if (s_directoryOpener)
        s_directoryOpener(url);
    else
        QDesktopServices::openUrl(url);
}

QString MainWindow::selectedTransferId() const
{
    auto *item = m_transferList->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString{};
}

void MainWindow::populateDemoData()
{
    const User peer{QStringLiteral("user-demo-bob"), QStringLiteral("bob"),
                    QStringLiteral("Bob Chen"), true, true};
    m_contacts.insert(peer.id, peer);
    m_presenceKnown = true;
    m_onlineUserIds.insert(peer.id);
    m_userNames.insert(peer.id, peer.label());
    auto *item = createContactItem(m_users, peer, QStringLiteral("在线"), 0);
    m_users->setCurrentItem(item);
    m_chat->clear();
    ChatMessage incoming;
    incoming.senderId = peer.id;
    incoming.content = QStringLiteral("新的多用户工作台已经准备好了。");
    incoming.timestamp = QDateTime::currentMSecsSinceEpoch() - 65000;
    renderMessage(incoming);
    ChatMessage own;
    own.senderId = m_session.user.id;
    own.content = QStringLiteral("很好，聊天和文件传输现在都能独立切换。");
    own.timestamp = QDateTime::currentMSecsSinceEpoch() - 35000;
    own.readAt = QDateTime::currentMSecsSinceEpoch() - 30000;
    renderMessage(own);
    incoming.content = QStringLiteral("设计评审.pdf 已上传，请查看传输进度。");
    incoming.timestamp = QDateTime::currentMSecsSinceEpoch() - 8000;
    renderMessage(incoming);

    QList<TransferCardData> rows;
    TransferCardData uploading;
    uploading.upload = true;
    uploading.fileName = QStringLiteral("设计评审.pdf");
    uploading.peerName = peer.label();
    uploading.localPath = QStringLiteral("C:/Users/demo/outbox/设计评审.pdf");
    uploading.stateText = QStringLiteral("上传中");
    uploading.stateKey = QStringLiteral("active");
    uploading.size = 5 * 1024 * 1024;
    uploading.transferred = 3774874; // 72%, displayed independently from the bar.
    rows.append(uploading);

    TransferCardData completed;
    completed.upload = false;
    completed.fileName = QStringLiteral("产品使用手册.pdf");
    completed.peerName = peer.label();
    completed.localPath = QStringLiteral("C:/Users/demo/downloads/产品使用手册.pdf");
    completed.stateText = QStringLiteral("已完成");
    completed.stateKey = QStringLiteral("completed");
    completed.size = 2465792;
    completed.transferred = completed.size;
    rows.append(completed);

    TransferCardData failed;
    failed.upload = true;
    failed.fileName = QStringLiteral("季度财务汇总-final-v12.xlsx");
    failed.peerName = peer.label();
    failed.localPath = QStringLiteral(
        "C:/Users/demo/OneDrive/Documents/公司资料/跨区域联合项目/2026/第四季度/区域销售与财务对账/最终确认版本/管理层审阅/已审批归档文件/季度财务汇总-final-v12.xlsx");
    failed.stateText = QStringLiteral("失败");
    failed.stateKey = QStringLiteral("failed");
    failed.size = 19293798;
    failed.transferred = 4823450; // 25%.
    failed.error = QStringLiteral("网络连接中断，请检查网络后重试");
    rows.append(failed);

    for (const TransferCardData &data : rows) {
        auto *row = new QTreeWidgetItem(m_transferList);
        row->setData(0, Qt::UserRole + 1, QDir::cleanPath(data.localPath));
        row->setData(0, Qt::UserRole + 2, data.stateKey);
        row->setText(0, QStringLiteral("%1 · %2 · %3").arg(data.fileName, data.peerName, data.stateText));
        auto *widget = new TransferTaskCard(m_transferList);
        m_transferList->setItemWidget(row, 0, widget);
        widget->setData(data);
        row->setSizeHint(0, widget->sizeHint());
        row->setToolTip(0, QDir::toNativeSeparators(data.localPath));
        connect(widget, &TransferTaskCard::clicked, this, [this, row] {
            if (!m_closing)
                m_transferList->setCurrentItem(row);
        });
        connect(widget, &TransferTaskCard::openDirectoryRequested,
                this, &MainWindow::openTransferDirectory);
    }
    m_transferList->setCurrentItem(m_transferList->topLevelItem(0));
    updateTransferSummary();
    showTransfersView();
    setPeerUnread(peer.id, 1);
}

void MainWindow::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    m_ticketPending = false;
    m_readRequests.clear();
    m_usersTimer->stop();
    m_historyTimer->stop();
    m_reconnectTimer->stop();
    disconnect(m_socket, nullptr, this, nullptr);
    m_socket->shutdown();
    disconnect(m_transfers, nullptr, this, nullptr);
    m_transfers->shutdown();
}
