// MainWindow 协调入口、会话操作与关闭顺序。
#include "MainWindow.h"

#include "LocalStore.h"
#include "MainWindowInternal.h"
#include "MessageDelivery.h"

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>

using namespace MainWindowInternal;

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
        ChatMessage notification;
        notification.kind = QStringLiteral("file");
        notification.fileId = task->fileId();
        notification.fileName = task->fileName();
        notification.fileSize = task->size();
        if (!sendPayload(task->peerId(), notification))
            showNotice(QStringLiteral("文件已上传，但文件消息尚未发送"));
    });
    connect(m_socket, &WebSocketClient::connected, this, [this] {
        setConnectionState(QStringLiteral("已连接"));
        m_delivery->onConnected();
    });
    connect(m_socket, &WebSocketClient::disconnected, this, [this] {
        if (m_closing)
            return;
        setConnectionState(QStringLiteral("已断开"));
        m_delivery->onDisconnected();
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

    m_delivery = new MessageDelivery(m_store, m_session.user.id,
        [this](const WireMessage &wire, QString *error) { return m_socket->send(wire, error); },
        [this] { return m_socket->isConnected(); }, this);
    connect(m_delivery, &MessageDelivery::messageChanged, this,
            [this](const QString &peerId, const QString &) {
        if (!m_closing && peerId == m_peerId)
            renderConversation();
    });
    connect(m_delivery, &MessageDelivery::historyRequested,
            this, &MainWindow::requestHistory);
    connect(m_delivery, &MessageDelivery::errorOccurred,
            this, &MainWindow::showNotice);
    m_delivery->recover();

    if (online) {
        loadUnreadSummary();
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
        QStringLiteral("确定要退出 %1 吗？将清除本地登录会话并关闭窗口，不会删除服务器账号。")
            .arg(m_session.user.username.toUpper()),
        QMessageBox::Yes | QMessageBox::No, this);
    prompt.button(QMessageBox::Yes)->setText(QStringLiteral("是"));
    prompt.button(QMessageBox::No)->setText(QStringLiteral("否"));
    prompt.setDefaultButton(QMessageBox::No);
    const auto answer = static_cast<QMessageBox::StandardButton>(execCenteredMessageBox(prompt, this));
    if (!guarded)
        return;
    if (answer != QMessageBox::Yes || m_closing) {
        m_exitPending = false;
        return;
    }
    m_exitPending = true;
    m_exitButton->setEnabled(false);
    m_api->logout([this, guarded](bool ok, const QString &error) {
        if (!guarded || m_closing)
            return;
        m_exitPending = false;
        m_exitButton->setEnabled(true);
        if (!ok) {
            showNotice(QStringLiteral("退出失败：%1。服务器登录凭据可能仍有效，本地会话保留，可重试。")
                .arg(error));
            return;
        }
        m_store->clearSession();
        emit logoutCompleted();
    });
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
        QMessageBox::Yes | QMessageBox::No, this);
    prompt.button(QMessageBox::Yes)->setText(QStringLiteral("永久删除"));
    prompt.button(QMessageBox::No)->setText(QStringLiteral("否"));
    prompt.setDefaultButton(QMessageBox::No);
    const auto answer = static_cast<QMessageBox::StandardButton>(execCenteredMessageBox(prompt, this));
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
        emit exitRequested(); // Only a 204 response reaches this point.
    });
}

void MainWindow::setConnectionState(const QString &state)
{
    if (m_socketLabel)
        m_socketLabel->setText(QStringLiteral("●  ") + state);
}

void MainWindow::showNotice(const QString &notice)
{
    if (!m_notice)
        return;
    m_notice->setText(notice);
    m_notice->setVisible(!notice.isEmpty());
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

void MainWindow::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    m_ticketPending = false;
    m_readRequests.clear();
    m_historyRequests.clear();
    m_delivery->shutdown();
    m_usersTimer->stop();
    m_historyTimer->stop();
    m_reconnectTimer->stop();
    disconnect(m_socket, nullptr, this, nullptr);
    m_socket->shutdown();
    disconnect(m_transfers, nullptr, this, nullptr);
    m_transfers->shutdown();
}
