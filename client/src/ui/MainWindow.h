#pragma once

#include "MessageSyncState.h"
#include "TransferManager.h"
#include "WebSocketClient.h"

#include <QMap>
#include <QSet>
#include <QWidget>
#include <QUrl>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

// The authenticated chat surface.  Despite the historical class name this is a
// plain widget so the exact same implementation can be embedded in both apps.
class MainWindow final : public QWidget {
    Q_OBJECT
public:
    MainWindow(ApiClient *api, LocalStore *store, WireCodec *codec,
               const AuthSession &session, bool online = true, QWidget *parent = nullptr);
    ~MainWindow() override;

    void shutdown();
    bool isClosing() const { return m_closing; }
    void setActive(bool active);
    QString username() const { return m_session.user.username; }
    QString connectionState() const { return m_connectionState; }
    TransferManager *transferManager() const { return m_transfers; }
    void showTransfersView();
    void showChatView();
    void populateDemoData();

    using DirectoryOpener = std::function<bool(const QUrl &)>;
    static void setDirectoryOpenerForTests(DirectoryOpener opener);

signals:
    void connectionStateChanged(const QString &state);
    void unreadCountChanged(int count);
    void noticeChanged(const QString &notice);
    void exitRequested();

private:
    friend class SessionTabsTest;

    void buildUi();
    void loadUsers();
    void loadPresence();
    void applyUsers(const QList<User> &users);
    void setPeerUnread(const QString &peerId, int count);
    void clearPeerUnread(const QString &peerId);
    void updateConversationLabel();
    void selectPeer();
    void filterUsers(const QString &text);
    void loadHistory();
    void connectSocket();
    void scheduleReconnect();
    void sendText();
    void chooseFile();
    bool sendPayload(const QString &peerId, const QJsonObject &payload);
    void handleWireMessage(const WireMessage &wire);
    void appendChat(const QString &peerId, const ChatMessage &message);
    void renderMessage(const ChatMessage &message);
    void renderConversation();
    void handleReadReceipt(const WireMessage &wire);
    bool canMarkRead(const QString &peerId) const;
    void markLatestIncomingRead(const QString &peerId);
    void addTransferRow(TransferTask *task);
    void updateTransferRow(TransferTask *task);
    QString selectedTransferId() const;
    void updateSessionLabel();
    void setConnectionState(const QString &state);
    void showNotice(const QString &notice);
    QProgressBar *progressForTask(QTreeWidgetItem *item, TransferTask *task);
    void openTransferDirectory(const QString &localPath);
    void requestExit();
    void requestAccountDeletion();

    ApiClient *m_api;
    LocalStore *m_store;
    WireCodec *m_codec;
    AuthSession m_session;
    WebSocketClient *m_socket;
    TransferManager *m_transfers;

    QLabel *m_sessionLabel = nullptr;
    QLabel *m_socketLabel = nullptr;
    QLabel *m_conversationLabel = nullptr;
    QLabel *m_notice = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_users = nullptr;
    QTextBrowser *m_chat = nullptr;
    QLineEdit *m_input = nullptr;
    QStackedWidget *m_views = nullptr;
    QPushButton *m_chatNav = nullptr;
    QPushButton *m_transferNav = nullptr;
    QPushButton *m_deleteAccountButton = nullptr;
    QTreeWidget *m_transferList = nullptr;
    QSpinBox *m_concurrency = nullptr;
    QTimer *m_usersTimer = nullptr;
    QTimer *m_historyTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;

    QString m_peerId;
    QString m_connectionState = QStringLiteral("离线");
    QMap<QString, QString> m_userNames;
    QMap<QString, User> m_contacts;
    QSet<QString> m_onlineUserIds;
    bool m_presenceKnown = false;
    MessageSyncState m_sync;
    QMap<QString, QString> m_readCursors;
    QMap<QString, qint64> m_readTimes;
    QMap<QString, QString> m_sentReadCursors;
    QSet<QString> m_readRequests;
    bool m_synchronizingHistory = false;
    QSet<QString> m_downloadIds;
    bool m_ticketPending = false;
    bool m_online = true;
    bool m_active = true;
    bool m_closing = false;
    bool m_exitPending = false;
    bool m_accountDeletionPending = false;
    QMap<QString, int> m_unreadByPeer;
    int m_unreadCount = 0;

    static DirectoryOpener s_directoryOpener;
};
