// 聊天历史、实时帧、outbox 协作及 READ 回执；仅机械移动既有实现。
#include "MainWindow.h"

#include "MainWindowInternal.h"
#include "Format.h"
#include "LocalStore.h"
#include "MessageDelivery.h"
#include "MessagePayloadCodec.h"

#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QStackedWidget>
#include <QTextBrowser>
using namespace MainWindowInternal;

void MainWindow::loadHistory()
{
    requestHistory(m_peerId);
}

void MainWindow::requestHistory(const QString &peerId)
{
    if (m_closing || !m_online || peerId.isEmpty() || m_historyRequests.contains(peerId))
        return;
    m_historyRequests.insert(peerId);
    QPointer<MainWindow> guarded(this);
    m_api->messages(peerId, m_sync.cursor(peerId),
                    [this, peerId, guarded](bool ok, const QList<ChatMessage> &messages,
                                            const QString &error) {
        if (!guarded || m_closing)
            return;
        m_historyRequests.remove(peerId);
        if (!ok) {
            showNotice(QStringLiteral("消息同步：") + error);
            return;
        }
        m_synchronizingHistory = true;
        for (const ChatMessage &message : messages)
            appendChat(peerId, message);
        m_synchronizingHistory = false;
        m_delivery->onHistorySynced();
        markLatestIncomingRead(peerId);
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

bool MainWindow::sendPayload(const QString &peerId, const ChatMessage &message)
{
    if (m_closing || peerId.isEmpty())
        return false;
    ChatMessage pending;
    QString error;
    // outbox 先持久化发送意图再尝试写 socket；断线恢复沿用同一 messageId，只有 ACK 才结束待确认状态。
    if (!m_delivery->enqueue(peerId, message, &pending, &error)) {
        showNotice(QStringLiteral("发送失败：") + error);
        return false;
    }
    m_sync.accept(peerId, pending);
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
    ChatMessage message;
    message.content = text;
    if (sendPayload(m_peerId, message))
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
    if (!m_transfers->addUpload(m_peerId, source)) {
        QMessageBox prompt(QMessageBox::Warning, QStringLiteral("发送文件"),
                           QStringLiteral("文件不可读"), QMessageBox::Ok, this);
        execCenteredMessageBox(prompt, this);
    }
}

void MainWindow::handleWireMessage(const WireMessage &wire)
{
    if (m_closing)
        return;
    if (wire.type == WireMessage::Type::Read) {
        handleReadReceipt(wire);
        return;
    }
    if (wire.type == WireMessage::Type::Ack) {
        // ACK cursor belongs to this message only. It must not advance the
        // peer's incremental history cursor in MessageSyncState.
        m_delivery->handleAck(wire);
        return;
    }
    if (wire.type == WireMessage::Type::Error) {
        m_delivery->handleError(wire);
        return;
    }
    if (wire.type != WireMessage::Type::Message)
        return;
    ChatMessage message;
    QString error;
    if (!MessagePayloadCodec::decode(wire.payload, &message, &error)) {
        showNotice(QStringLiteral("协议错误：") + error);
        return;
    }
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

// handleReadReceipt 处理双方广播的 READ：只有原作者为当前用户时才更新自己的已读游标和时间，不能误标读者自己的消息。
void MainWindow::handleReadReceipt(const WireMessage &wire)
{
    if (wire.cursor.isEmpty())
        return;
    const QString peer = wire.senderId == m_session.user.id ? wire.recipientId : wire.senderId;
    if (peer.isEmpty())
        return;
    const qint64 readAt = wire.readAt > 0 ? wire.readAt
        : wire.timestamp > 0 ? wire.timestamp : QDateTime::currentMSecsSinceEpoch();
    // READ 广播给双方；recipientId 是原 MESSAGE 作者，读者自己的副本不能误标其消息。
    const QString authorId = wire.recipientId;
    if (authorId == m_session.user.id) {
        const QString previous = m_readCursors.value(peer);
        if (previous.isEmpty() || cursorAtMost(previous, wire.cursor)) {
            m_readCursors.insert(peer, wire.cursor);
            m_readTimes.insert(peer, qMax(readAt, m_readTimes.value(peer)));
        }
    }
    QString storeError;
    const bool changed = m_store->markMessagesRead(peer, authorId, wire.cursor, readAt,
                                                   &storeError);
    if (!storeError.isEmpty())
        showNotice(QStringLiteral("已读状态写盘失败：") + storeError);
    if (changed && peer == m_peerId)
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
    if (m_synchronizingHistory)
        stored = m_delivery->historyProjection(stored);
    if (stored.senderId == m_session.user.id && stored.readAt <= 0
        && cursorAtMost(stored.cursor, m_readCursors.value(peerId)))
        stored.readAt = m_readTimes.value(peerId);
    const bool accepted = m_sync.accept(peerId, stored);
    QString storeError;
    const bool changed = m_store->appendMessage(peerId, stored, &storeError);
    if (!storeError.isEmpty()) {
        showNotice(QStringLiteral("消息写盘失败：") + storeError);
        return;
    }
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
        QString deliveryState;
        if (message.readAt > 0)
            deliveryState = QStringLiteral("已读 %1").arg(formatTime(message.readAt));
        else if (message.failed)
            deliveryState = QStringLiteral("失败");
        else if (message.pending && message.attemptCount == 0)
            deliveryState = QStringLiteral("等待发送");
        else if (message.pending)
            deliveryState = QStringLiteral("等待ACK");
        else if (message.acked)
            deliveryState = QStringLiteral("已确认未读");
        else
            deliveryState = QStringLiteral("未读"); // 旧本地记录的展示投影
        time = time.isEmpty() ? deliveryState : QStringLiteral("%1 · %2").arg(time, deliveryState);
    }
    const QString body = message.isFile()
        ? QStringLiteral("📎 %1 <span style='color:#64748b'>%2</span>")
            .arg(message.fileName.toHtmlEscaped(), Format::bytes(message.fileSize))
        : message.content.toHtmlEscaped();
    const QString color = own ? QStringLiteral("#e8efff") : QStringLiteral("#f1f5f9");
    m_chat->append(QStringLiteral(
        "<table width='100%' cellspacing='0' cellpadding='7'><tr><td style='background:%1;border-radius:10px'>"
        "<b style='color:#25324b'>%2</b> <span style='color:#94a3b8;font-size:11px'>%3</span><br>"
        "<span style='color:#334155'>%4</span></td></tr></table>")
        .arg(color, sender.toHtmlEscaped(), time, body));
}
