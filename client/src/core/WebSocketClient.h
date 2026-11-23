#pragma once

#include "WireCodec.h"

#include <QWebSocket>

// WebSocketClient 负责异步连接、二进制帧编解码和错误分发。
// send 返回完整帧长度仅表示 Qt 接受写入，不代表服务端已确认或对方已读。
class WebSocketClient final : public QObject {
    Q_OBJECT
public:
    explicit WebSocketClient(WireCodec *codec, QObject *parent = nullptr);

    void open(const QUrl &url);
    void shutdown();
    bool send(const WireMessage &message, QString *error = nullptr);
    bool isConnected() const { return m_socket.state() == QAbstractSocket::ConnectedState; }

signals:
    void connected();
    void disconnected();
    void messageReceived(const WireMessage &message);
    void errorOccurred(const QString &message);
    void protocolError(const QString &message);

private:
    WireCodec *m_codec;
    QWebSocket m_socket;
};
