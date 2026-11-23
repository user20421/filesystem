#include "SingleUserWindow.h"

#include "SessionPage.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QScreen>

SingleUserWindow::SingleUserWindow(std::unique_ptr<SessionContext> context, QWidget *parent)
    : QMainWindow(parent), m_page(new SessionPage(std::move(context), this))
{
    setWindowTitle(QStringLiteral("即时通讯与文件传输系统"));
    setCentralWidget(m_page);
    setFixedSize(560, 390);
    connect(m_page, &SessionPage::titleChanged, this, [this] {
        if (m_page->isAuthenticated())
            setWindowTitle(QStringLiteral("即时通讯与文件传输系统 · %1")
                .arg(m_page->context()->session().user.label()));
    });
    connect(m_page, &SessionPage::stressModeChanged, this, [this](bool active) {
        if (active) {
            setMinimumSize(960, 640);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            resize(1280, 820);
            setWindowTitle(QStringLiteral("即时通讯与文件传输系统 · ROOT · 系统压力测试"));
            QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                const QRect available = screen->availableGeometry();
                move(available.center() - rect().center());
            }
        } else {
            setFixedSize(560, 390);
            setWindowTitle(QStringLiteral("即时通讯与文件传输系统"));
        }
    });
    connect(m_page, &SessionPage::exitRequested, this, &QMainWindow::close);
}

void SingleUserWindow::closeEvent(QCloseEvent *event)
{
    m_page->shutdown();
    QMainWindow::closeEvent(event);
}
