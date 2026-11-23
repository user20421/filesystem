// 请求句柄与 ApiClient 生命周期；流式句柄的稳定身份不变。
#include "ApiClient.h"

#include <QUrlQuery>

ApiRequestHandle::ApiRequestHandle(ApiClient *client)
    : QObject(client), m_client(client) {}
void ApiRequestHandle::abort()
{
    if (!m_completed && !m_cancelled && m_client)
        m_client->cancelRequest(this, true);
}

QByteArray ApiRequestHandle::readAll()
{
    if (!m_reply)
        return {};
    const QByteArray data = m_reply->readAll();
    m_client->scheduleStreamingFinish(this);
    return data;
}

QByteArray ApiRequestHandle::read(qint64 maxSize)
{
    if (!m_reply || maxSize <= 0)
        return {};
    const QByteArray data = m_reply->read(maxSize);
    m_client->scheduleStreamingFinish(this);
    return data;
}

qint64 ApiRequestHandle::bytesAvailable() const
{
    return m_reply ? m_reply->bytesAvailable() : 0;
}

void ApiRequestHandle::setReadPaused(bool paused)
{
    if (m_completed || m_cancelled)
        return;
    m_readPaused = paused;
    if (!paused)
        m_client->scheduleStreamingFinish(this);
}

ApiClient::ApiClient(QObject *parent)
    : QObject(parent), m_codec(createDefaultWireCodec()) {}
ApiClient::~ApiClient()
{
    shutdown();
}

void ApiClient::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    const auto requests = m_requests;
    m_requests.clear();
    m_waitingForRefresh.clear();
    m_refreshing = false;
    m_refreshRequest.clear();
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (ApiRequestHandle *request = guarded.data()) {
            request->m_cancelled = true;
            if (QNetworkReply *reply = request->m_reply.data()) {
                // No completion/progress callback is allowed to re-enter a closing session.
                reply->disconnect();
                reply->abort();
                reply->deleteLater();
                request->m_reply.clear();
            }
            request->disconnect();
            request->deleteLater();
        }
    }
    m_manager.clearAccessCache();
}

QUrl ApiClient::webSocketUrl(const QString &ticket) const
{
    QUrl url = m_baseUrl;
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                        : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/ws"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ticket"), ticket);
    url.setQuery(query);
    return url;
}
