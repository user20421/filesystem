#pragma once

#include <QList>
#include <QMainWindow>
#include <QUrl>

class QLabel;
class QLineEdit;
class SessionPage;
class QTabWidget;
class QToolButton;

class SimulatorWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SimulatorWindow(const QString &dataRoot = {}, const QUrl &serverUrl = {},
                             QWidget *parent = nullptr);

    int sessionCount() const;
    SessionPage *sessionAt(int index) const; // Zero-based among user tabs, not the welcome tab.
    SessionPage *addSession(bool demo = false);
    QList<SessionPage *> addSessions(int count, bool demo = false);
    bool closeSession(int index);
    bool savePreview(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QWidget *createWelcomePage();
    SessionPage *createSession(bool demo);
    bool closePage(SessionPage *page);
    void updateTab(SessionPage *page);
    void activateCurrentTab();
    int nextUserNumber(const QString &root) const;
    QUrl serverUrl() const;

    QTabWidget *m_tabs;
    QLineEdit *m_root;
    QLineEdit *m_server;
    QToolButton *m_add;
    int m_nextNumber = 1;
    bool m_restoringTabMove = false;
};
