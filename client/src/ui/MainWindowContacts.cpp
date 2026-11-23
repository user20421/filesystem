// 联系人、presence、过滤及未读摘要版本控制。
#include "MainWindow.h"

#include "MainWindowInternal.h"
#include "LocalStore.h"

#include <QFrame>
#include <QLineEdit>
#include <QPointer>
#include <QTextBrowser>
using namespace MainWindowInternal;

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

// loadUnreadSummary 使用请求开始时的版本快照接受未读摘要，实时来信或本地阅读发生后，旧摘要不得覆盖新状态。
void MainWindow::loadUnreadSummary()
{
    if (m_closing)
        return;
    const QMap<QString, quint64> requestedVersions = m_unreadVersions;
    QPointer<MainWindow> guarded(this);
    m_api->unread([this, guarded, requestedVersions](
                      bool ok, const QList<UnreadSummary> &summaries, const QString &error) {
        if (!guarded || m_closing)
            return;
        if (!ok) {
            showNotice(QStringLiteral("未读摘要：") + error);
            return;
        }
        QMap<QString, int> counts;
        QSet<QString> peers;
        for (const UnreadSummary &summary : summaries) {
            counts.insert(summary.peerId, summary.unreadCount);
            peers.insert(summary.peerId);
        }
        for (auto it = requestedVersions.cbegin(); it != requestedVersions.cend(); ++it)
            peers.insert(it.key());
        for (const QString &peerId : peers) {
            // 按联系人比较版本，丢弃请求期间已被实时消息或本地已读推进的旧未读快照。
            if (m_unreadVersions.value(peerId) != requestedVersions.value(peerId))
                continue;
            setPeerUnread(peerId, counts.value(peerId));
        }
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
    for (User user : users) {
        if (user.id == m_session.user.id)
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
    m_unreadVersions.insert(peerId, ++m_unreadVersion);
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
