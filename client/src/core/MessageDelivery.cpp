#include "MessageDelivery.h"

#include "MessagePayloadCodec.h"

#include <QDateTime>
#include <QSet>
#include <QUuid>
#include <limits>

namespace {
QString conversationId(const QString &first, const QString &second)
{
    QStringList ids{first, second};
    ids.sort();
    return ids.join(QLatin1Char(':'));
}

QString wireError(const WireMessage &wire)
{
    if (!wire.errorCode.isEmpty() && !wire.errorMessage.isEmpty())
        return wire.errorCode + QStringLiteral(": ") + wire.errorMessage;
    return !wire.errorMessage.isEmpty() ? wire.errorMessage : wire.errorCode;
}
} // namespace

MessageDelivery::MessageDelivery(LocalStore *store, QString senderId,
                                 SendFunction send, ConnectedFunction connected,
                                 QObject *parent, int ackTimeoutMs)
    : QObject(parent), m_store(store), m_senderId(std::move(senderId)),
      m_send(std::move(send)), m_connected(std::move(connected)),
      m_ackTimeoutMs(qMax(1, ackTimeoutMs))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &MessageDelivery::processDue);
}

qint64 MessageDelivery::now() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

bool MessageDelivery::persist(const QString &peerId, const ChatMessage &message)
{
    QString error;
    if (m_store->appendMessage(peerId, message, &error))
        return true;
    if (!error.isEmpty())
        emit errorOccurred(QStringLiteral("消息写盘失败：") + error);
    return false;
}

bool MessageDelivery::enqueue(const QString &peerId, const ChatMessage &message,
                              ChatMessage *persisted, QString *error)
{
    if (error)
        error->clear();
    if (m_closing || peerId.isEmpty() || m_senderId.isEmpty()) {
        if (error)
            *error = QStringLiteral("消息缺少发送者或接收者");
        return false;
    }

    WireMessage wire;
    wire.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    wire.conversationId = conversationId(m_senderId, peerId);
    wire.senderId = m_senderId;
    wire.recipientId = peerId;
    wire.deviceId = QStringLiteral("desktop");
    wire.timestamp = now();
    if (!MessagePayloadCodec::encode(message, &wire.payload, error))
        return false;

    ChatMessage pending = message;
    pending.id = wire.messageId;
    pending.conversationId = wire.conversationId;
    pending.senderId = wire.senderId;
    pending.recipientId = wire.recipientId;
    pending.timestamp = wire.timestamp;
    pending.pending = true;
    pending.acked = false;
    pending.failed = false;
    pending.attemptCount = 0;
    pending.nextAttemptAt = wire.timestamp;
    pending.deliveryError.clear();

    // Durable intent precedes the first socket write. A write failure therefore
    // cannot produce an untracked server message.
    QString storeError;
    if (!m_store->appendMessage(peerId, pending, &storeError)) {
        if (error)
            *error = storeError.isEmpty() ? QStringLiteral("无法持久化待发送消息") : storeError;
        return false;
    }
    m_wire.insert(wire.messageId, wire);
    if (persisted)
        *persisted = pending;
    emit messageChanged(peerId, pending.id);
    processDue();
    return true;
}

// recover 从本地 outbox 恢复未确认消息，保留原始 ID、正文、时间戳和尝试次数，断线或重启不额外消耗发送次数。
void MessageDelivery::recover()
{
    if (m_closing)
        return;
    QString error;
    m_writeBlocked.clear();
    m_store->recoverOutbox(m_senderId, now(), &error);
    if (!error.isEmpty())
        emit errorOccurred(QStringLiteral("恢复待发送消息失败：") + error);
    scheduleNext();
}

void MessageDelivery::onConnected()
{
    if (m_closing)
        return;
    QSet<QString> peers;
    const QList<ChatMessage> outbox = m_store->outbox(m_senderId);
    for (const ChatMessage &message : outbox)
        peers.insert(message.recipientId);
    for (const QString &peerId : peers) {
        if (!peerId.isEmpty())
            emit historyRequested(peerId);
    }
    processDue();
}

void MessageDelivery::onDisconnected()
{
    // The last accepted write may still receive a late ACK. Do not consume a
    // retry attempt or fail the entry merely because the connection dropped.
    m_timer.stop();
}

void MessageDelivery::onHistorySynced()
{
    processDue();
}

// handleAck 仅把 ACK 对应的消息转为已确认并停止重试，不推进历史同步 cursor，也不表示对方已读。
void MessageDelivery::handleAck(const WireMessage &ack)
{
    if (m_closing || ack.messageId.isEmpty())
        return;
    ChatMessage message;
    QString peerId;
    if (!m_store->findMessage(ack.messageId, m_senderId, &message, &peerId))
        return;
    message.acked = true;
    message.pending = false;
    message.failed = false;
    message.nextAttemptAt = 0;
    message.deliveryError.clear();
    if (!ack.cursor.isEmpty())
        message.cursor = ack.cursor;
    if (!persist(peerId, message))
        return;
    m_wire.remove(ack.messageId);
    m_writeBlocked.remove(ack.messageId);
    emit messageChanged(peerId, ack.messageId);
    scheduleNext();
}

void MessageDelivery::handleError(const WireMessage &wire)
{
    if (m_closing || wire.messageId.isEmpty())
        return;
    ChatMessage message;
    QString peerId;
    if (!m_store->findMessage(wire.messageId, m_senderId, &message, &peerId))
        return;
    message.acked = false;
    message.pending = false;
    message.failed = true;
    message.nextAttemptAt = 0;
    message.deliveryError = wireError(wire);
    if (message.deliveryError.isEmpty())
        message.deliveryError = QStringLiteral("服务器拒绝消息");
    if (!persist(peerId, message))
        return;
    m_wire.remove(wire.messageId);
    m_writeBlocked.remove(wire.messageId);
    emit messageChanged(peerId, wire.messageId);
    scheduleNext();
}

// historyProjection 用服务端历史补证投递结果：迟到历史可恢复 failed，但 ACK 状态不能被旧 pending 或旧快照回退。
ChatMessage MessageDelivery::historyProjection(const ChatMessage &message)
{
    ChatMessage projected = message;
    if (projected.senderId == m_senderId) {
        // A row returned by history proves that the server accepted this ID,
        // even when its earlier ACK was lost or arrived after ERROR/timeout.
        projected.acked = true;
        projected.pending = false;
        projected.failed = false;
        projected.nextAttemptAt = 0;
        projected.deliveryError.clear();
        m_wire.remove(projected.id);
        m_writeBlocked.remove(projected.id);
    }
    return projected;
}

// sendDue 只有完整帧被接受才消耗一次尝试次数；写盘失败必须阻断发送，不能伪装为已入队。
void MessageDelivery::sendDue(const ChatMessage &message)
{
    if (m_closing || message.acked || message.failed || !message.pending
        || m_writeBlocked.contains(message.id))
        return;
    const qint64 current = now();
    const QString peerId = message.recipientId;
    if (peerId.isEmpty())
        return;

    if (message.attemptCount >= 3) {
        ChatMessage exhausted = message;
        exhausted.pending = false;
        exhausted.failed = true;
        exhausted.nextAttemptAt = 0;
        exhausted.deliveryError = QStringLiteral("3 次发送后仍未收到 ACK");
        if (persist(peerId, exhausted))
            emit messageChanged(peerId, exhausted.id);
        return;
    }
    if (message.nextAttemptAt > current)
        return;

    WireMessage wire;
    if (m_wire.contains(message.id)) {
        wire = m_wire.value(message.id);
    } else {
        wire.messageId = message.id;
        wire.conversationId = message.conversationId;
        wire.senderId = message.senderId;
        wire.recipientId = message.recipientId;
        wire.deviceId = QStringLiteral("desktop");
        wire.timestamp = message.timestamp;
        QString codecError;
        if (!MessagePayloadCodec::encode(message, &wire.payload, &codecError)) {
            ChatMessage update = message;
            update.nextAttemptAt = current + m_ackTimeoutMs;
            update.deliveryError = codecError;
            persist(peerId, update);
            emit errorOccurred(QStringLiteral("消息编码失败：") + codecError);
            emit messageChanged(peerId, update.id);
            return;
        }
        // Recovered messages are also encoded once per process and then reused
        // byte-for-byte by every retry.
        m_wire.insert(message.id, wire);
    }

    QString transportError;
    ChatMessage update = message;
    if (!m_send || !m_send(wire, &transportError)) {
        // No accepted full-frame write means no retry attempt is consumed.
        update.nextAttemptAt = current + m_ackTimeoutMs;
        update.deliveryError = transportError.isEmpty()
            ? QStringLiteral("WebSocket 未接受完整帧") : transportError;
        persist(peerId, update);
        emit messageChanged(peerId, update.id);
        return;
    }

    ++update.attemptCount;
    update.pending = true;
    update.acked = false;
    update.failed = false;
    update.deliveryError.clear();
    const qint64 backoff = qint64(m_ackTimeoutMs) << (update.attemptCount - 1);
    update.nextAttemptAt = current + backoff;
    if (persist(peerId, update)) {
        emit messageChanged(peerId, update.id);
    } else {
        // The frame may already be on the server, but its new attempt budget
        // could not be persisted. Block further writes until ACK/history or a
        // fresh process recovery instead of risking an unbounded retry loop.
        m_writeBlocked.insert(update.id);
    }
}

void MessageDelivery::processDue()
{
    if (m_closing)
        return;
    if (!m_connected || !m_connected()) {
        m_timer.stop();
        return;
    }
    const QList<ChatMessage> outbox = m_store->outbox(m_senderId);
    for (const ChatMessage &message : outbox) {
        if (message.pending && !m_writeBlocked.contains(message.id)
            && message.nextAttemptAt <= now())
            sendDue(message);
    }
    scheduleNext();
}

void MessageDelivery::scheduleNext()
{
    if (m_closing || !m_connected || !m_connected()) {
        m_timer.stop();
        return;
    }
    qint64 next = std::numeric_limits<qint64>::max();
    const QList<ChatMessage> outbox = m_store->outbox(m_senderId);
    for (const ChatMessage &message : outbox) {
        if (message.pending && !m_writeBlocked.contains(message.id))
            next = qMin(next, message.nextAttemptAt);
    }
    if (next == std::numeric_limits<qint64>::max()) {
        m_timer.stop();
        return;
    }
    const qint64 delay = qMax<qint64>(0, next - now());
    m_timer.start(static_cast<int>(qMin<qint64>(delay, std::numeric_limits<int>::max())));
}

void MessageDelivery::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    m_timer.stop();
    m_wire.clear();
    m_writeBlocked.clear();
}
