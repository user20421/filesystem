#include "WebSocketClient.h"

#include <QAbstractSocket>

WebSocketClient::WebSocketClient(WireCodec *codec, QObject *parent)
    : QObject(parent), m_codec(codec)
{
    connect(&m_socket, &QWebSocket::connected, this, &WebSocketClient::connected);
    connect(&m_socket, &QWebSocket::disconnected, this, &WebSocketClient::disconnected);
    connect(&m_socket, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray &frame) {
        WireMessage message;
        QString error;
        if (!m_codec->decode(frame, &message, &error))
            emit protocolError(error);
        else
            emit messageReceived(message);
    });
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(m_socket.errorString());
    });
}

void WebSocketClient::open(const QUrl &url)
{
    if (!m_codec->available()) {
        emit errorOccurred(m_codec->unavailableReason());
        return;
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
        m_socket.abort();
    m_socket.open(url);
}

void WebSocketClient::shutdown()
{
    disconnect(&m_socket, nullptr, this, nullptr);
    m_socket.abort();
}

bool WebSocketClient::send(const WireMessage &message, QString *error)
{
    if (!isConnected()) {
        if (error)
            *error = QStringLiteral("WebSocket 尚未连接");
        return false;
    }
    QByteArray frame;
    if (!m_codec->encode(message, &frame, error))
        return false;
    return m_socket.sendBinaryMessage(frame) == frame.size();
}
