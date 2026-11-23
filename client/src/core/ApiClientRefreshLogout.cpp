// 认证 epoch、single-flight 刷新和 logout 边界；不得改变回调接受顺序。
#include "ApiClient.h"

#include "ApiJsonCodec.h"
#include "LocalStore.h"

#include <QJsonDocument>
#include <QJsonObject>

void ApiClient::setAuthSession(const AuthSession &session)
{
    const bool unchanged = m_accessToken == session.accessToken
        && m_refreshToken == session.refreshToken && m_session.user.id == session.user.id;
    const bool clearing = session.accessToken.isEmpty() && session.refreshToken.isEmpty()
        && session.user.id.isEmpty();
    if (!unchanged || clearing)
        invalidateAuthentication();
    m_session = session;
    m_accessToken = session.accessToken;
    m_refreshToken = session.refreshToken;
}

void ApiClient::invalidateAuthentication()
{
    ++m_authEpoch;
    m_loggingOut = false;
    m_refreshing = false;
    m_refreshRequest.clear();
    const auto requests = m_requests;
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (guarded && !guarded->m_completed && !guarded->m_cancelled)
            cancelRequest(guarded.data(), true);
    }
    m_waitingForRefresh.clear();
}

void ApiClient::setAccessToken(const QString &token)
{
    m_accessToken = token;
    m_session.accessToken = token;
}

// beginRefresh 让同一认证 epoch 的并发 401 共享一次刷新，避免多个请求同时轮换 Refresh Token。
void ApiClient::beginRefresh()
{
    if (m_refreshing || m_closing)
        return;
    if (m_refreshToken.isEmpty()) {
        failAuthentication(QStringLiteral("登录会话已过期，请重新登录"));
        return;
    }
    m_refreshing = true;
    // refresh 捕获当前 epoch；显式登录或退出推进 epoch 后，迟到结果不能再覆盖新会话。
    const quint64 epoch = m_authEpoch;
    m_refreshRequest = startRefresh(m_refreshToken, [this, epoch](bool ok, const AuthSession &session,
                                                                 const QString &error) {
        completeRefresh(ok, session, error, epoch);
    }, true);
    if (!m_refreshRequest) {
        m_refreshing = false;
        failAuthentication(QStringLiteral("无法创建登录刷新请求"));
    }
}

// completeRefresh 只接受当前 epoch 的结果，随后原子保存双令牌并允许原请求重放一次；退出或重新登录后的迟到结果必须丢弃。
void ApiClient::completeRefresh(bool ok, const AuthSession &session, const QString &error,
                                quint64 epoch)
{
    if (m_closing || epoch != m_authEpoch)
        return;
    m_refreshRequest.clear();
    m_refreshing = false;
    QString failure = error;
    if (ok && m_sessionStore) {
        QString saveError;
        if (!m_sessionStore->saveSession(session, &saveError))
            failure = QStringLiteral("无法保存刷新后的会话：") + saveError;
    }
    if (!ok || !failure.isEmpty()) {
        failAuthentication(failure.isEmpty() ? QStringLiteral("登录会话刷新失败") : failure);
        return;
    }

    // Do not invalidate requests waiting on this same-epoch refresh. A later
    // explicit login/logout changes the epoch and makes late callbacks inert.
    m_session = session;
    m_accessToken = session.accessToken;
    m_refreshToken = session.refreshToken;
    emit sessionRefreshed(session);
    const auto waiting = m_waitingForRefresh;
    m_waitingForRefresh.clear();
    for (const QPointer<ApiRequestHandle> &guarded : waiting) {
        if (guarded && !guarded->m_cancelled && !guarded->m_completed)
            startRequest(guarded.data());
    }
}

// failAuthentication 使当前 epoch 的请求失效并返回登录页，防止失败回调递归或旧请求重新写回会话。
void ApiClient::failAuthentication(const QString &error)
{
    if (m_authFailureInProgress || m_closing)
        return;
    m_authFailureInProgress = true;
    m_refreshing = false;
    m_refreshRequest.clear();
    m_session = {};
    m_accessToken.clear();
    m_refreshToken.clear();
    if (m_sessionStore)
        m_sessionStore->clearSession();

    const auto waiting = m_waitingForRefresh;
    m_waitingForRefresh.clear();
    const QString message = error.isEmpty() ? QStringLiteral("登录会话已失效，请重新登录") : error;
    for (const QPointer<ApiRequestHandle> &guarded : waiting) {
        if (guarded && !guarded->m_completed)
            finishRequest(guarded.data(), false, {}, message);
    }
    m_authFailureInProgress = false;
    emit authenticationFailed(message);
}

ApiRequestHandle *ApiClient::refresh(const QString &refreshToken, AuthCallback callback)
{
    return startRefresh(refreshToken, std::move(callback), false);
}

ApiRequestHandle *ApiClient::startRefresh(const QString &refreshToken, AuthCallback callback,
                                          bool internal)
{
    if (m_loggingOut && !internal)
        return nullptr;
    return jsonRequest(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/refresh"),
                       {{QStringLiteral("refresh_token"), refreshToken}},
                       [callback](bool ok, const QByteArray &body, const QString &error) {
        AuthSession session;
        if (ok) {
            session = ApiJsonCodec::parseAuth(QJsonDocument::fromJson(body).object());
            if (session.accessToken.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少 access_token"));
            else if (session.refreshToken.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少 refresh_token"));
            else if (session.user.id.isEmpty())
                callback(false, {}, QStringLiteral("刷新响应缺少用户 id"));
            else
                callback(true, session, {});
        } else {
            callback(false, {}, error);
        }
    }, false);
}

// logout 先使旧刷新和等待请求失效，再撤销当前 Refresh Token；若自身遇 401，重放时必须使用轮换后的新令牌。
ApiRequestHandle *ApiClient::logout(OperationCallback callback)
{
    if (m_closing || m_loggingOut)
        return nullptr;

    // Logout is an authentication boundary, not an ordinary queued request.
    // Invalidate every older request (especially independent refreshes and
    // requests waiting for one) before creating the revocation request.
    m_loggingOut = true;
    ++m_authEpoch;
    m_refreshing = false;
    m_refreshRequest.clear();
    const auto requests = m_requests;
    for (const QPointer<ApiRequestHandle> &guarded : requests) {
        if (guarded && !guarded->m_completed && !guarded->m_cancelled)
            cancelRequest(guarded.data(), true);
    }
    m_waitingForRefresh.clear();

    const quint64 epoch = m_authEpoch;
    ApiRequestHandle *handle = request(QStringLiteral("POST"), QStringLiteral("/api/v1/auth/logout"), {},
                   QStringLiteral("application/json"), {},
                   [this, epoch, callback](bool ok, const QByteArray &, const QString &error) {
        if (epoch == m_authEpoch && !ok)
            m_loggingOut = false;
        callback(ok, error);
    }, false, 204, true, true);
    if (!handle)
        m_loggingOut = false;
    return handle;
}
