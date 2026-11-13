#pragma once

#include "WireCodec.h"

#include <QWebSocket>

class WebSocketClient final : public QObject {
    Q_OBJECT
public:
    explicit WebSocketClient(WireCodec *codec, QObject *parent = nullptr);

    void open(const QUrl &url);
    void close();
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
