#pragma once

#include "LocalStore.h"
#include "WireCodec.h"

#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

// MessageDelivery 提供持久化的 at-least-once 消息投递。
// 写入 WebSocket 只表示开始等待 ACK，不代表服务端已保存，也不代表对方已读。
class MessageDelivery final : public QObject {
    Q_OBJECT
public:
    using SendFunction = std::function<bool(const WireMessage &, QString *)>;
    using ConnectedFunction = std::function<bool()>;

    MessageDelivery(LocalStore *store, QString senderId,
                    SendFunction send, ConnectedFunction connected,
                    QObject *parent = nullptr, int ackTimeoutMs = 12000);

    bool enqueue(const QString &peerId, const ChatMessage &message,
                 ChatMessage *persisted = nullptr, QString *error = nullptr);
    void recover();
    void onConnected();
    void onDisconnected();
    void onHistorySynced();
    void handleAck(const WireMessage &ack);
    void handleError(const WireMessage &error);
    ChatMessage historyProjection(const ChatMessage &message);
    void processDue();
    void shutdown();
    bool isClosing() const { return m_closing; }

signals:
    void messageChanged(const QString &peerId, const QString &messageId);
    void historyRequested(const QString &peerId);
    void errorOccurred(const QString &error);

private:
    void sendDue(const ChatMessage &message);
    void scheduleNext();
    bool persist(const QString &peerId, const ChatMessage &message);
    qint64 now() const;

    LocalStore *m_store;
    QString m_senderId;
    SendFunction m_send;
    ConnectedFunction m_connected;
    QMap<QString, WireMessage> m_wire;
    QSet<QString> m_writeBlocked;
    QTimer m_timer;
    int m_ackTimeoutMs;
    bool m_closing = false;
};
