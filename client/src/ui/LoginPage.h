#pragma once

#include "ApiClient.h"
#include "LocalStore.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

class LoginPage final : public QWidget {
    Q_OBJECT
public:
    LoginPage(ApiClient *api, LocalStore *store, QWidget *parent = nullptr);
    void setUsername(const QString &username);
    void shutdown();

signals:
    void authenticated(const AuthSession &session);

private:
    void authenticate(bool registration);
    void setBusy(bool busy);

    ApiClient *m_api;
    LocalStore *m_store;
    QLineEdit *m_username;
    QLineEdit *m_password;
    QPushButton *m_login;
    QPushButton *m_register;
    QLabel *m_status;
    bool m_closing = false;
};
