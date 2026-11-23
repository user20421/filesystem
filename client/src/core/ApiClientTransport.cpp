// HTTP 传输实现：请求重放、取消、响应完成及流式背压协作。
#include "ApiClient.h"

#include "ApiJsonCodec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>

QString ApiClient::responseError(const QByteArray &body, const QString &fallback)
{
    return ApiJsonCodec::responseError(body, fallback);
}

ApiRequestHandle *ApiClient::request(const QString &method, const QString &path,
                                     const QByteArray &body, const QString &contentType,
                                     const QMap<QString, QString> &headers, RawCallback callback,
                                     bool streaming, int expectedStatus,
                                     bool allowRefresh, bool logoutBody)
{
    if (m_closing || (m_authFailureInProgress && allowRefresh)
        || (m_loggingOut && allowRefresh && !logoutBody))
        return nullptr;
    auto *handle = new ApiRequestHandle(this);
    handle->m_method = method;
    handle->m_path = path;
    handle->m_body = body;
    handle->m_contentType = contentType;
    handle->m_headers = headers;
    handle->m_callback = std::move(callback);
    handle->m_streaming = streaming;
    handle->m_expectedStatus = expectedStatus;
    handle->m_allowRefresh = allowRefresh;
    handle->m_logoutBody = logoutBody;
    handle->m_authEpoch = m_authEpoch;
    m_requests.append(handle);
    startRequest(handle);
    return handle;
}

void ApiClient::startRequest(ApiRequestHandle *handle)
{
    if (!handle || m_closing || handle->m_completed || handle->m_cancelled
        || handle->m_authEpoch != m_authEpoch)
        return;
    QNetworkRequest networkRequest(m_baseUrl.resolved(QUrl(handle->m_path)));
    if (handle->m_allowRefresh && !m_accessToken.isEmpty())
        networkRequest.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    if (!handle->m_contentType.isEmpty())
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, handle->m_contentType);
    for (auto it = handle->m_headers.cbegin(); it != handle->m_headers.cend(); ++it)
        networkRequest.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    // A logout replay must revoke the newly rotated token, not the consumed one.
    const QByteArray body = handle->m_logoutBody
        ? QJsonDocument(QJsonObject{{QStringLiteral("refresh_token"), m_refreshToken}})
            .toJson(QJsonDocument::Compact)
        : handle->m_body;
    QNetworkReply *reply = nullptr;
    if (handle->m_method == QLatin1String("GET"))
        reply = m_manager.get(networkRequest);
    else if (handle->m_method == QLatin1String("DELETE"))
        reply = m_manager.deleteResource(networkRequest);
    else
        reply = m_manager.sendCustomRequest(networkRequest, handle->m_method.toUtf8(), body);
    handle->m_reply = reply;
    if (handle->m_streaming)
        reply->setReadBufferSize(256 * 1024);

    connect(reply, &QNetworkReply::readyRead, handle, [handle, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (handle->m_reply == reply && handle->m_streaming && !handle->m_cancelled
            && !handle->m_readPaused && status >= 200 && status < 300)
            emit handle->readyRead();
    });
    connect(reply, &QNetworkReply::downloadProgress, handle,
            [handle, reply](qint64 received, qint64 total) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (handle->m_reply == reply && handle->m_streaming && !handle->m_cancelled
            && status >= 200 && status < 300)
            emit handle->downloadProgress(received, total);
    });
    QPointer<ApiRequestHandle> guarded(handle);
    connect(reply, &QNetworkReply::finished, this, [this, guarded, reply] {
        if (!guarded || guarded->m_reply != reply) {
            reply->deleteLater();
            return;
        }
        ApiRequestHandle *handle = guarded.data();
        if (m_closing) {
            handle->m_reply.clear();
            reply->deleteLater();
            return;
        }
        finishReply(handle, reply);
    });
}

void ApiClient::finishReply(ApiRequestHandle *handle, QNetworkReply *reply)
{
    if (!handle || !reply || handle->m_completed || handle->m_reply != reply)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool httpOk = status >= 200 && status < 300
        && (handle->m_expectedStatus == 0 || status == handle->m_expectedStatus);
    const bool ok = reply->error() == QNetworkReply::NoError && httpOk;
    if (ok && handle->m_streaming) {
        handle->m_networkFinished = true;
        if (handle->m_readPaused || reply->bytesAvailable() > 0) {
            // Keep the stable handle and final buffered bytes alive until the
            // consumer's bounded writer queue has accepted every byte.
            if (!handle->m_readPaused && reply->bytesAvailable() > 0) {
                QPointer<ApiRequestHandle> guarded(handle);
                QTimer::singleShot(0, handle, [guarded] {
                    if (guarded && !guarded->m_readPaused && !guarded->m_completed)
                        emit guarded->readyRead();
                });
            }
            return;
        }
    }

    // Streaming callers drain readyRead and final bytes themselves. Error
    // bodies are consumed here and are never emitted as downloaded data.
    const QByteArray response = reply->error() == QNetworkReply::OperationCanceledError
        || (handle->m_streaming && reply->error() == QNetworkReply::NoError)
        ? QByteArray{} : reply->readAll();
    QString error;
    if (!ok) {
        const QString fallback = reply->error() == QNetworkReply::NoError
                || reply->errorString().isEmpty()
            ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
        error = responseError(response, fallback);
    }

    if (status == 401 && handle->m_allowRefresh && !handle->m_cancelled
        && handle->m_authEpoch == m_authEpoch
        && handle->m_retryCount == 0 && !m_refreshToken.isEmpty()) {
        ++handle->m_retryCount;
        handle->m_reply.clear();
        reply->deleteLater();
        if (!m_waitingForRefresh.contains(handle))
            m_waitingForRefresh.append(handle);
        beginRefresh();
        return;
    }

    const bool rejectedAfterRetry = status == 401 && handle->m_allowRefresh
        && !handle->m_cancelled && handle->m_authEpoch == m_authEpoch && handle->m_retryCount > 0;
    const bool noRefreshAvailable = status == 401 && handle->m_allowRefresh
        && !handle->m_cancelled && handle->m_authEpoch == m_authEpoch
        && m_refreshToken.isEmpty() && !m_session.user.id.isEmpty();
    finishRequest(handle, ok && !handle->m_cancelled, response,
                  handle->m_cancelled ? QStringLiteral("请求已取消") : error);
    // A refreshed request is replayed exactly once. A second 401 invalidates
    // the session instead of starting another refresh cycle.
    if (rejectedAfterRetry || noRefreshAvailable)
        failAuthentication(error);
}

void ApiClient::scheduleStreamingFinish(ApiRequestHandle *handle)
{
    if (!handle || !handle->m_streaming || !handle->m_networkFinished
        || handle->m_readPaused || handle->m_completed || handle->m_cancelled)
        return;
    QPointer<ApiRequestHandle> guarded(handle);
    QTimer::singleShot(0, handle, [this, guarded] {
        if (!guarded || guarded->m_completed || guarded->m_cancelled
            || guarded->m_readPaused || !guarded->m_networkFinished || !guarded->m_reply)
            return;
        if (guarded->m_reply->bytesAvailable() == 0)
            finishReply(guarded.data(), guarded->m_reply.data());
    });
}

void ApiClient::finishRequest(ApiRequestHandle *handle, bool ok, const QByteArray &body,
                              const QString &error)
{
    if (!handle || handle->m_completed)
        return;
    handle->m_completed = true;
    m_requests.removeAll(handle);
    m_waitingForRefresh.removeAll(handle);
    QPointer<ApiRequestHandle> guarded(handle);
    QPointer<QNetworkReply> reply(handle->m_reply);
    handle->m_callback(ok, body, error);
    if (guarded)
        guarded->deleteLater();
    if (reply)
        reply->deleteLater();
}

void ApiClient::cancelRequest(ApiRequestHandle *handle, bool deliverCallback)
{
    if (!handle || handle->m_completed || handle->m_cancelled)
        return;
    handle->m_cancelled = true;
    m_waitingForRefresh.removeAll(handle);
    if (!handle->m_reply) {
        if (deliverCallback && !m_closing)
            finishRequest(handle, false, {}, QStringLiteral("请求已取消"));
        return;
    }
    if (handle->m_networkFinished) {
        QPointer<QNetworkReply> reply = handle->m_reply;
        handle->m_reply.clear();
        if (reply) {
            reply->disconnect();
            reply->deleteLater();
        }
        if (deliverCallback && !m_closing)
            finishRequest(handle, false, {}, QStringLiteral("请求已取消"));
        return;
    }
    handle->m_reply->abort();
    if (!deliverCallback && handle->m_reply)
        handle->m_reply->disconnect();
}

ApiRequestHandle *ApiClient::jsonRequest(const QString &method, const QString &path,
                                         const QJsonObject &json, RawCallback callback,
                                         bool allowRefresh)
{
    return request(method, path, QJsonDocument(json).toJson(QJsonDocument::Compact),
                   QStringLiteral("application/json"), {}, std::move(callback),
                   false, 0, allowRefresh);
}
