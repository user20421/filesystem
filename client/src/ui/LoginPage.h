#pragma once

#include "ApiClient.h"
#include "LocalStore.h"

#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;

class LoginPage final : public QWidget {
    Q_OBJECT
public:
    explicit LoginPage(ApiClient *api, QWidget *parent = nullptr);
    void setUsername(const QString &username);
    void showError(const QString &error);
    void shutdown();

    // Return true when the interceptor owns this request. SessionPage activates
    // the backend-identified user and persists ordinary sessions centrally.
    using AuthenticationInterceptor = std::function<bool(
        const QString &, const QString &, bool, ApiClient::AuthCallback)>;
    void setAuthenticationInterceptor(AuthenticationInterceptor interceptor)
    {
        m_authenticationInterceptor = std::move(interceptor);
    }

signals:
    void authenticated(const AuthSession &session);

private:
    void authenticate(bool registration);
    void setBusy(bool busy);

    ApiClient *m_api;
    QLineEdit *m_username;
    QLineEdit *m_password;
    QPushButton *m_login;
    QPushButton *m_register;
    QPushButton *m_systemTest;
    QLabel *m_status;
    AuthenticationInterceptor m_authenticationInterceptor;
    bool m_closing = false;
};
