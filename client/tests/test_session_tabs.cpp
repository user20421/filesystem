#include "ApiClient.h"
#include "MainWindow.h"
#include "SessionContext.h"
#include "SessionPage.h"
#include "SimulatorWindow.h"
#include "SystemTestPage.h"
#include "Theme.h"
#include "TransferTaskCard.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

namespace {
class ScopedCurrentDirectory final {
public:
    explicit ScopedCurrentDirectory(const QString &path)
        : m_previous(QDir::currentPath()), m_valid(QDir::setCurrent(path)) {}
    ~ScopedCurrentDirectory() { QDir::setCurrent(m_previous); }
    bool isValid() const { return m_valid; }
private:
    QString m_previous;
    bool m_valid;
};

AuthSession session(const QString &name, const QString &token)
{
    AuthSession result;
    result.accessToken = token;
    result.refreshToken = token + QStringLiteral("-refresh");
    result.user.id = name + QStringLiteral("-id");
    result.user.username = name;
    result.user.displayName = name;
    return result;
}

class HoldingServer final : public QObject {
public:
    bool start()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                m_sockets.append(socket);
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("captured").toBool())
                        return;
                    qsizetype contentLength = 0;
                    const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
                    for (const QByteArray &line : lines) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    }
                    if (buffer->size() - headerEnd - 4 < contentLength)
                        return;
                    socket->setProperty("captured", true);
                    requests.append(*buffer);
                    if (!m_responses.isEmpty()) {
                        const auto response = m_responses.takeFirst();
                        const QByteArray http = "HTTP/1.1 " + QByteArray::number(response.first)
                            + " Test\r\nContent-Type: application/json\r\nContent-Length: "
                            + QByteArray::number(response.second.size())
                            + "\r\nConnection: close\r\n\r\n" + response.second;
                        socket->write(http);
                        socket->flush();
                        socket->disconnectFromHost();
                    }
                });
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }
    void enqueue(int status, const QByteArray &body = {}) { m_responses.append({status, body}); }
    QList<QByteArray> requests;
private:
    QTcpServer m_server;
    QList<QTcpSocket *> m_sockets;
    QList<QPair<int, QByteArray>> m_responses;
};

void answerMessageBox(QMessageBox::StandardButton choice)
{
    QTimer::singleShot(0, [choice] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (auto *button = box->button(choice))
            button->click();
        else
            box->reject();
    });
}
} // namespace

class SessionTabsTest final : public QObject {
    Q_OBJECT
private slots:
    void isolatesDirectoriesTokensAndObjects();
    void rejectsAndReleasesDirectoryLock();
    void closingOneTabDoesNotAffectAnother();
    void exitRequiresConfirmationAndClearsOnlySession();
    void accountDeletionHandlesCancelFailureAndSuccess();
    void accountDeletionPreventsReentryAndClosesSafely();
    void pendingRequestsAndTransfersCloseSafely();
    void generatesOffscreenPreview();
    void fixedDataRootAndUppercaseLogin();
    void transferDirectoryUsesFakeOpenerOnce();
    void transferCardsPresentReadableDetails();
    void transferCardActionsRemainAvailable();
    void contactUnreadDotsAreIndependentAndClearOnView();
    void contactFilterAllowsRegisteredCustomUsers();
    void readReceiptsRespectVisibilityAndPersist();
};

void SessionTabsTest::isolatesDirectoriesTokensAndObjects()
{
    QTemporaryDir root;
    SessionContext first(root.filePath(QStringLiteral("user_1")), QUrl(QStringLiteral("http://localhost")));
    SessionContext second(root.filePath(QStringLiteral("user_2")), QUrl(QStringLiteral("http://localhost")));
    QVERIFY(first.open());
    QVERIFY(second.open());
    QVERIFY(first.dataDir() != second.dataDir());
    QVERIFY(first.store() != second.store());
    QVERIFY(first.api() != second.api());
    QVERIFY(first.codec() != second.codec());
    first.setSession(session(QStringLiteral("alice"), QStringLiteral("token-alice")));
    second.setSession(session(QStringLiteral("bob"), QStringLiteral("token-bob")));
    QCOMPARE(first.api()->accessToken(), QStringLiteral("token-alice"));
    QCOMPARE(second.api()->accessToken(), QStringLiteral("token-bob"));
    QVERIFY(first.store()->saveSession(first.session()));
    QVERIFY(second.store()->saveSession(second.session()));
    QCOMPARE(first.store()->loadSession().accessToken, QStringLiteral("token-alice"));
    QCOMPARE(second.store()->loadSession().accessToken, QStringLiteral("token-bob"));
    ChatMessage message;
    message.id = QStringLiteral("only-alice");
    message.content = QStringLiteral("private history");
    first.store()->appendMessage(QStringLiteral("peer"), message);
    QCOMPARE(first.store()->messages(QStringLiteral("peer")).size(), 1);
    QVERIFY(second.store()->messages(QStringLiteral("peer")).isEmpty());
    first.store()->setConcurrency(5);
    second.store()->setConcurrency(3);
    QCOMPARE(first.store()->concurrency(), 5);
    QCOMPARE(second.store()->concurrency(), 3);
    first.shutdown();
    second.shutdown();
    QVERIFY(first.api()->isClosing());
    QVERIFY(second.api()->isClosing());
}

void SessionTabsTest::rejectsAndReleasesDirectoryLock()
{
    QTemporaryDir root;
    const QString path = root.filePath(QStringLiteral("user_1"));
    auto first = std::make_unique<SessionContext>(path, QUrl(QStringLiteral("http://localhost")));
    QVERIFY(first->open());
    SessionContext contender(path, QUrl(QStringLiteral("http://localhost")));
    QString error;
    QVERIFY(!contender.open(&error));
    QVERIFY(error.contains(QStringLiteral("已被使用")));
    first->shutdown();
    first.reset();
    SessionContext reopened(path, QUrl(QStringLiteral("http://localhost")));
    QVERIFY2(reopened.open(&error), qPrintable(error));
    reopened.shutdown();
}

void SessionTabsTest::closingOneTabDoesNotAffectAnother()
{
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window(root.filePath(QStringLiteral("ignored")), QUrl(QStringLiteral("http://127.0.0.1:1")));
    QCOMPARE(window.windowTitle(), QStringLiteral("即时通讯和文件传输工具"));
    auto *rootEdit = window.findChild<QLineEdit *>(QStringLiteral("dataRootEdit"));
    QVERIFY(rootEdit);
    QVERIFY(rootEdit->isReadOnly());
    auto *serverEdit = window.findChild<QLineEdit *>(QStringLiteral("serverUrlEdit"));
    QVERIFY(serverEdit && serverEdit->isReadOnly());
    QCOMPARE(serverEdit->text(), QStringLiteral("http://127.0.0.1:1"));
    for (const QPushButton *button : window.findChildren<QPushButton *>())
        QVERIFY(!button->text().contains(QStringLiteral("一键创建")));
    QCOMPARE(rootEdit->text(), QDir::cleanPath(QDir(root.path()).absoluteFilePath(QStringLiteral("runtime/users"))));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("系统测试"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
    QVERIFY(qobject_cast<SystemTestPage *>(tabs->widget(0)));
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("systemTestLauncher")));
    auto *systemTest = qobject_cast<SystemTestPage *>(tabs->widget(0));
    QVERIFY(systemTest && systemTest->controller());
    QVERIFY(!systemTest->controller()->isRunning());
    for (int index = 0; index < 2; ++index) {
        QVERIFY(!tabs->tabBar()->tabButton(index, QTabBar::LeftSide));
        QVERIFY(!tabs->tabBar()->tabButton(index, QTabBar::RightSide));
    }
    auto pages = window.addSessions(2, true);
    QCOMPARE(pages.size(), 2);
    QCOMPARE(tabs->count(), 4);
    QVERIFY(tabs->tabBar()->tabButton(2, QTabBar::RightSide));
    QVERIFY(tabs->tabBar()->tabButton(3, QTabBar::RightSide));
    SessionPage *first = pages[0];
    SessionPage *second = pages[1];
    const QString firstDir = first->context()->dataDir();
    QPointer<SessionPage> removed(first);
    QPointer<SessionPage> survivor(second);
    QVERIFY(window.closeSession(0));
    QVERIFY(removed.isNull());
    QVERIFY(!survivor.isNull());
    QCOMPARE(window.sessionCount(), 1);
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->tabText(0), QStringLiteral("系统测试"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
    QCOMPARE(tabs->widget(2), second);
    tabs->setCurrentIndex(0);
    QCOMPARE(tabs->currentWidget(), tabs->widget(0));
    tabs->setCurrentIndex(1);
    QCOMPARE(tabs->currentWidget(), tabs->widget(1));
    tabs->setCurrentIndex(2);
    QCOMPARE(tabs->currentWidget(), second);
    QVERIFY(!second->context()->isClosing());
    QVERIFY(!second->context()->api()->isClosing());
    QVERIFY(!second->chatPage()->transferManager()->isClosing());
    QCOMPARE(second->context()->api()->accessToken(), second->context()->session().accessToken);

    SessionContext reopened(firstDir, QUrl(QStringLiteral("http://127.0.0.1:1")));
    QString error;
    QVERIFY2(reopened.open(&error), qPrintable(error));
    reopened.shutdown();

    QPointer<SessionPage> closedSurvivor(second);
    emit tabs->tabCloseRequested(tabs->indexOf(second));
    QVERIFY(closedSurvivor.isNull());
    QCOMPARE(window.sessionCount(), 0);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("系统测试"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
}

void SessionTabsTest::exitRequiresConfirmationAndClearsOnlySession()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window({}, server.url());
    auto pages = window.addSessions(2, true);
    QCOMPARE(pages.size(), 2);
    SessionPage *first = pages[0];
    SessionPage *second = pages[1];
    QVERIFY(first->context()->store()->saveSession(first->context()->session()));
    QVERIFY(second->context()->store()->saveSession(second->context()->session()));
    ChatMessage history;
    history.id = QStringLiteral("history-kept-after-logout");
    history.content = QStringLiteral("keep me");
    QVERIFY(first->context()->store()->appendMessage(QStringLiteral("peer"), history));
    const QString firstSession = first->context()->store()->path(QStringLiteral("config/session.json"));
    const QString secondSession = second->context()->store()->path(QStringLiteral("config/session.json"));
    const QString firstHistory = first->context()->store()->path(QStringLiteral("config/messages.json"));
    QVERIFY(QFileInfo::exists(firstSession));
    QVERIFY(QFileInfo::exists(secondSession));
    QVERIFY(QFileInfo::exists(firstHistory));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    auto *exit = first->chatPage()->findChild<QPushButton *>(QStringLiteral("exitButton"));
    auto *deleteAccount = first->chatPage()->findChild<QPushButton *>(
        QStringLiteral("deleteAccountButton"));
    QVERIFY(tabs && exit && deleteAccount);
    QCOMPARE(exit->text(), QStringLiteral("退出"));
    QCOMPARE(deleteAccount->text(), QStringLiteral("注销"));
    tabs->setCurrentWidget(first);
    QPointer<SessionPage> removed(first);
    QPointer<SessionPage> survivor(second);
    QString prompt;
    QString yesText;
    QString noText;
    QMessageBox::StandardButtons buttons;
    const auto answer = [](QMessageBox::StandardButton choice) {
        QTimer::singleShot(0, [choice] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            if (auto *button = box->button(choice))
                button->click();
            else
                box->reject();
        });
    };
    QTimer::singleShot(0, [&prompt, &yesText, &noText, &buttons] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        prompt = box->text();
        yesText = box->button(QMessageBox::Yes)->text();
        noText = box->button(QMessageBox::No)->text();
        buttons = box->standardButtons();
    });
    answer(QMessageBox::No);
    exit->click();
    QCOMPARE(prompt, QStringLiteral(
        "确定要退出 ALICE 吗？将清除本地登录会话并关闭该标签页，不会删除服务器账号。"));
    QVERIFY(buttons.testFlag(QMessageBox::Yes));
    QVERIFY(buttons.testFlag(QMessageBox::No));
    QCOMPARE(yesText, QStringLiteral("是"));
    QCOMPARE(noText, QStringLiteral("否"));
    QVERIFY(!removed.isNull());
    QCOMPARE(window.sessionCount(), 2);
    QCOMPARE(tabs->currentWidget(), first);
    QVERIFY(QFileInfo::exists(firstSession));
    QVERIFY(!first->isClosing());
    QVERIFY(!first->context()->api()->isClosing());

    QTimer::singleShot(0, [exit] { exit->click(); });
    answer(QMessageBox::Yes);
    exit->click();
    QVERIFY(removed.isNull());
    QVERIFY(!survivor.isNull());
    QCOMPARE(window.sessionCount(), 1);
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->currentWidget(), tabs->widget(1));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
    QVERIFY(!QFileInfo::exists(firstSession));
    QVERIFY(QFileInfo::exists(firstHistory));
    QVERIFY(QFileInfo::exists(secondSession));
    QVERIFY(!second->isClosing());
    QVERIFY(!second->context()->api()->isClosing());
    QVERIFY(!second->chatPage()->transferManager()->isClosing());
    QCOMPARE(second->context()->api()->accessToken(), second->context()->session().accessToken);
    QCoreApplication::processEvents();
    QCOMPARE(server.requests.size(), 0); // Exiting never calls the account deletion endpoint.
}

void SessionTabsTest::accountDeletionHandlesCancelFailureAndSuccess()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window({}, server.url());
    auto pages = window.addSessions(2, true);
    QCOMPARE(pages.size(), 2);
    SessionPage *first = pages[0];
    SessionPage *second = pages[1];
    QVERIFY(first->context()->store()->saveSession(first->context()->session()));
    QVERIFY(second->context()->store()->saveSession(second->context()->session()));
    ChatMessage history;
    history.id = QStringLiteral("history-kept-after-account-deletion");
    history.content = QStringLiteral("keep local history");
    QVERIFY(first->context()->store()->appendMessage(QStringLiteral("peer"), history));
    const QString firstSession = first->context()->store()->path(QStringLiteral("config/session.json"));
    const QString secondSession = second->context()->store()->path(QStringLiteral("config/session.json"));
    const QString firstHistory = first->context()->store()->path(QStringLiteral("config/messages.json"));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    auto *button = first->chatPage()->findChild<QPushButton *>(QStringLiteral("deleteAccountButton"));
    QVERIFY(tabs && button);
    tabs->setCurrentWidget(first);
    QPointer<SessionPage> removed(first);
    QPointer<SessionPage> survivor(second);

    QString prompt;
    bool defaultWasNo = false;
    QTimer::singleShot(0, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        prompt = box->text();
        defaultWasNo = box->defaultButton() == box->button(QMessageBox::No);
    });
    answerMessageBox(QMessageBox::No);
    button->click();
    QVERIFY(prompt.contains(QStringLiteral("永久删除服务器账号 ALICE")));
    QVERIFY(prompt.contains(QStringLiteral("关联消息和文件")));
    QVERIFY(prompt.contains(QStringLiteral("不可恢复")));
    QVERIFY(defaultWasNo);
    QVERIFY(removed);
    QCOMPARE(window.sessionCount(), 2);
    QVERIFY(QFileInfo::exists(firstSession));
    QCOMPARE(server.requests.size(), 0);

    server.enqueue(500, QByteArray("{\"error\":{\"message\":\"delete unavailable\"}}"));
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 3000);
    QVERIFY(removed);
    QVERIFY(!first->isClosing());
    QVERIFY(QFileInfo::exists(firstSession));
    QVERIFY(first->chatPage()->m_notice->text().contains(QStringLiteral("delete unavailable")));
    QVERIFY(first->chatPage()->m_notice->text().contains(QStringLiteral("可重试")));
    QVERIFY(!second->isClosing());
    QVERIFY(QFileInfo::exists(secondSession));

    server.enqueue(204);
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(removed.isNull(), 3000);
    QVERIFY(survivor);
    QCOMPARE(window.sessionCount(), 1);
    QCOMPARE(tabs->currentWidget(), tabs->widget(1));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
    QVERIFY(!QFileInfo::exists(firstSession));
    QVERIFY(QFileInfo::exists(firstHistory));
    QVERIFY(QFileInfo::exists(secondSession));
    QVERIFY(!second->isClosing());
    QVERIFY(!second->context()->api()->isClosing());
    QCOMPARE(server.requests.size(), 2);
    for (const QByteArray &request : server.requests) {
        QVERIFY(request.startsWith("DELETE /api/v1/users/me "));
        QVERIFY(request.contains("Authorization: Bearer demo-token-1"));
        QVERIFY(!request.contains("demo-token-2"));
    }
}

void SessionTabsTest::accountDeletionPreventsReentryAndClosesSafely()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window({}, server.url());
    auto pages = window.addSessions(2, true);
    QCOMPARE(pages.size(), 2);
    SessionPage *first = pages[0];
    SessionPage *second = pages[1];
    QVERIFY(first->context()->store()->saveSession(first->context()->session()));
    QVERIFY(second->context()->store()->saveSession(second->context()->session()));
    const QString firstSession = first->context()->store()->path(QStringLiteral("config/session.json"));
    auto *button = first->chatPage()->findChild<QPushButton *>(QStringLiteral("deleteAccountButton"));
    QVERIFY(button);
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QVERIFY(!button->isEnabled());
    first->chatPage()->requestAccountDeletion();
    button->click();
    QCoreApplication::processEvents();
    QCOMPARE(server.requests.size(), 1);

    QPointer<SessionPage> removed(first);
    QPointer<SessionPage> survivor(second);
    QVERIFY(window.closeSession(0));
    QVERIFY(removed.isNull());
    QVERIFY(survivor);
    QVERIFY(QFileInfo::exists(firstSession)); // A cancelled pending DELETE never clears a session.
    QVERIFY(!second->isClosing());
    QVERIFY(!second->context()->api()->isClosing());
    QCoreApplication::processEvents();
    QTest::qWait(80);
    QCOMPARE(window.sessionCount(), 1);
    QCOMPARE(server.requests.size(), 1);
}

void SessionTabsTest::pendingRequestsAndTransfersCloseSafely()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window(root.filePath(QStringLiteral("ignored")), server.url());
    SessionPage *first = window.addSession();
    SessionPage *second = window.addSession();
    QVERIFY(first && second);
    first->startDemo(session(QStringLiteral("alice"), QStringLiteral("first-token")));
    second->startDemo(session(QStringLiteral("bob"), QStringLiteral("second-token")));
    int callbacks = 0;
    first->context()->api()->users([&callbacks](bool, const QList<User> &, const QString &) { ++callbacks; });
    QVERIFY(first->context()->api()->pendingRequestCount() > 0);
    TransferTask *download = first->chatPage()->transferManager()->addDownload(
        QStringLiteral("peer"), QStringLiteral("active-file"), QStringLiteral("active.bin"), 1024 * 1024);
    QVERIFY(download);
    QFile source(root.filePath(QStringLiteral("upload.bin")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QVERIFY(source.write(QByteArray(128 * 1024, 'u')) == 128 * 1024);
    source.close();
    QVERIFY(first->chatPage()->transferManager()->addUpload(QStringLiteral("peer"), source.fileName()));
    QPointer<SessionPage> survivor(second);
    window.closeSession(0);
    QVERIFY(survivor);
    QVERIFY(!second->context()->isClosing());
    QCoreApplication::processEvents();
    QTest::qWait(80);
    QCOMPARE(callbacks, 0);
    QCOMPARE(window.sessionCount(), 1);
}

void SessionTabsTest::generatesOffscreenPreview()
{
    Theme::apply(*qobject_cast<QApplication *>(QCoreApplication::instance()));
    const QString path = qEnvironmentVariable("SECURE_IM_PREVIEW_PATH");
    QVERIFY(!path.isEmpty());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window(root.filePath(QStringLiteral("ignored")), QUrl(QStringLiteral("http://127.0.0.1:8080")));
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("系统测试"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("欢迎"));
    auto *testPage = qobject_cast<SystemTestPage *>(tabs->widget(0));
    QVERIFY(testPage);
    auto *testButton = testPage->findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    QVERIFY(testButton);
    QCOMPARE(testButton->text(), QStringLiteral("开始测试"));
    QVERIFY(testButton->isEnabled());
    auto *stopButton = testPage->findChild<QPushButton *>(QStringLiteral("stressStopButton"));
    auto *target = testPage->findChild<QSpinBox *>(QStringLiteral("stressTargetSpinBox"));
    auto *history = testPage->findChild<QTreeWidget *>(QStringLiteral("stressHistoryList"));
    QVERIFY(stopButton && target && history);
    QVERIFY(!stopButton->isEnabled());
    QCOMPARE(target->value(), 1000);
    QVERIFY(testPage->findChild<QLabel *>(QStringLiteral("stressResourceLabel"))->text()
        .contains(QStringLiteral("500 MiB")));
    window.show();
    QTest::qWait(80);
    const QString testPath = QFileInfo(path).absoluteDir().filePath(
        QStringLiteral("system-test-preview.png"));
    QVERIFY(window.savePreview(testPath));
    QVERIFY(QFileInfo(testPath).size() > 1000);
    tabs->setCurrentIndex(1);
    QTest::qWait(80);
    const QString welcomePath = QFileInfo(path).absoluteDir().filePath(
        QStringLiteral("session-tabs-welcome-preview.png"));
    QVERIFY(window.savePreview(welcomePath));
    QVERIFY(QFileInfo(welcomePath).size() > 1000);
    auto pages = window.addSessions(2, true);
    QCOMPARE(pages.size(), 2);
    pages[0]->chatPage()->showTransfersView();
    pages[1]->chatPage()->showChatView();
    window.show();
    QCOMPARE(pages[0]->chatPage()->connectionState(), QStringLiteral("演示连接"));
    tabs->setCurrentIndex(2);
    QTest::qWait(120);
    QVERIFY(tabs->tabText(2).contains(QStringLiteral("在线")));
    QVERIFY(window.savePreview(path));
    QFile output(path);
    QVERIFY(output.exists());
    QVERIFY(output.size() > 1000);
    QImage image(path);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(1280, 820));

    tabs->setCurrentIndex(3);
    QTest::qWait(60);
    QVERIFY(pages[1]->chatPage()->findChild<QTextBrowser *>(QStringLiteral("chatTranscript"))
        ->toPlainText().contains(QStringLiteral("已读")));
    QVERIFY(tabs->tabText(3).contains(QStringLiteral("演示用户 2").toUpper()));
    const QString chatPath = QFileInfo(path).absoluteDir().filePath(QStringLiteral("session-tabs-chat-preview.png"));
    QVERIFY(window.savePreview(chatPath));
    QVERIFY(QFileInfo(chatPath).size() > 1000);
    QVERIFY(!QImage(chatPath).isNull());
}

void SessionTabsTest::fixedDataRootAndUppercaseLogin()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window(root.filePath(QStringLiteral("must-not-be-used")), server.url());
    SessionPage *page = window.addSession();
    QVERIFY(page);
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("sessionTabs"));
    QVERIFY(tabs);
    QVERIFY(tabs->tabText(2).contains(QStringLiteral("USER_1")));
    auto *rootEdit = window.findChild<QLineEdit *>(QStringLiteral("dataRootEdit"));
    QVERIFY(rootEdit && rootEdit->isReadOnly());
    QVERIFY(rootEdit->text().endsWith(QDir::cleanPath(QStringLiteral("runtime/users"))));
    auto *username = page->findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = page->findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = page->findChild<QPushButton *>(QStringLiteral("loginButton"));
    QVERIFY(username && password && login);
    QCOMPARE(password->text(), QStringLiteral("123456"));
    QVERIFY(password->placeholderText().contains(QStringLiteral("6~128")));
    username->setText(QStringLiteral("alice"));
    password->setText(QStringLiteral("123456"));
    login->click();
    QCOMPARE(username->text(), QStringLiteral("ALICE"));
    QVERIFY(!login->isEnabled());
    QVERIFY(page->context()->api()->pendingRequestCount() > 0);
}

void SessionTabsTest::transferDirectoryUsesFakeOpenerOnce()
{
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window({}, QUrl(QStringLiteral("http://127.0.0.1:1")));
    SessionPage *page = window.addSession(true);
    QVERIFY(page);
    window.show();
    MainWindow *chat = page->chatPage();
    chat->showTransfersView();
    QList<QUrl> opened;
    MainWindow::setDirectoryOpenerForTests([&](const QUrl &url) {
        opened.append(url);
        return true;
    });
    QTreeWidgetItem *row = chat->m_transferList->topLevelItem(0);
    QVERIFY(row);
    auto *card = qobject_cast<TransferTaskCard *>(chat->m_transferList->itemWidget(row, 0));
    QVERIFY(card);
    chat->m_transferList->setCurrentItem(nullptr);
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
    QCOMPARE(chat->m_transferList->currentItem(), row);
    QCOMPARE(opened.size(), 0); // Clicking the card itself only selects it.

    auto *fileName = card->findChild<QLabel *>(QStringLiteral("transferFileName"));
    QVERIFY(fileName);
    QTest::mouseClick(fileName, Qt::LeftButton);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().toLocalFile(), QDir::cleanPath(QStringLiteral("C:/Users/demo/outbox")));
    auto *button = chat->findChild<QPushButton *>(QStringLiteral("openTransferDirectoryButton"));
    QVERIFY(button);
    button->click();
    QCOMPARE(opened.size(), 2);
    auto *path = card->findChild<QLabel *>(QStringLiteral("transferPath"));
    QVERIFY(path);
    QTest::mouseClick(path, Qt::LeftButton);
    QCOMPARE(opened.size(), 3);
    auto *cardButton = card->findChild<QPushButton *>(QStringLiteral("openTransferCardDirectoryButton"));
    QVERIFY(cardButton);
    cardButton->click();
    QCOMPARE(opened.size(), 4);
    MainWindow::setDirectoryOpenerForTests({});
}

void SessionTabsTest::transferCardsPresentReadableDetails()
{
    QTemporaryDir root;
    ScopedCurrentDirectory cwd(root.path());
    QVERIFY(cwd.isValid());
    SimulatorWindow window({}, QUrl(QStringLiteral("http://127.0.0.1:1")));
    SessionPage *page = window.addSession(true);
    QVERIFY(page);
    window.show();
    MainWindow *chat = page->chatPage();
    chat->showTransfersView();
    QCOMPARE(chat->m_transferList->columnCount(), 1);
    QVERIFY(chat->m_transferList->header()->isHidden());
    QCOMPARE(chat->m_transferList->topLevelItemCount(), 3);
    QVERIFY(!chat->m_transferEmpty->isVisible());
    QVERIFY(chat->m_transferStats->text().contains(QStringLiteral("共 3 项")));
    QVERIFY(chat->m_transferStats->text().contains(QStringLiteral("1 进行中")));
    QVERIFY(chat->m_transferStats->text().contains(QStringLiteral("1 已完成")));
    QVERIFY(chat->m_transferStats->text().contains(QStringLiteral("1 失败")));

    const QStringList states{QStringLiteral("active"), QStringLiteral("completed"),
                             QStringLiteral("failed")};
    for (int index = 0; index < 3; ++index) {
        QTreeWidgetItem *item = chat->m_transferList->topLevelItem(index);
        auto *card = qobject_cast<TransferTaskCard *>(chat->m_transferList->itemWidget(item, 0));
        QVERIFY(card);
        auto *direction = card->findChild<QLabel *>(QStringLiteral("transferDirection"));
        auto *name = card->findChild<QLabel *>(QStringLiteral("transferFileName"));
        auto *peer = card->findChild<QLabel *>(QStringLiteral("transferPeer"));
        auto *percent = card->findChild<QLabel *>(QStringLiteral("transferPercent"));
        auto *bytes = card->findChild<QLabel *>(QStringLiteral("transferBytes"));
        auto *state = card->findChild<QLabel *>(QStringLiteral("transferState"));
        auto *path = card->findChild<QLabel *>(QStringLiteral("transferPath"));
        auto *progress = card->findChild<QProgressBar *>(QStringLiteral("transferProgress"));
        QVERIFY(direction && name && peer && percent && bytes && state && path && progress);
        QVERIFY(direction->text().contains(index == 1
            ? QStringLiteral("下载") : QStringLiteral("上传")));
        QVERIFY(!name->text().isEmpty());
        QVERIFY(peer->text().contains(QStringLiteral("Bob Chen"), Qt::CaseInsensitive));
        QVERIFY(percent->text().endsWith(QLatin1Char('%')));
        QVERIFY(bytes->text().contains(QLatin1Char('/')));
        QCOMPARE(state->property("state").toString(), states[index]);
        QVERIFY(!progress->isTextVisible());
        QVERIFY(progress->height() <= 8);
        QVERIFY(path->toolTip().contains(QDir::toNativeSeparators(
            item->data(0, Qt::UserRole + 1).toString())));
        QVERIFY(item->sizeHint(0).height() >= 110);
    }
    auto *firstCard = qobject_cast<TransferTaskCard *>(
        chat->m_transferList->itemWidget(chat->m_transferList->topLevelItem(0), 0));
    QCOMPARE(firstCard->findChild<QLabel *>(QStringLiteral("transferPercent"))->text(),
             QStringLiteral("72%"));
    auto *lastCard = qobject_cast<TransferTaskCard *>(
        chat->m_transferList->itemWidget(chat->m_transferList->topLevelItem(2), 0));
    auto *error = lastCard->findChild<QLabel *>(QStringLiteral("transferError"));
    QVERIFY(error && error->isVisible());
    QVERIFY(error->text().contains(QStringLiteral("网络连接中断")));
    auto *longPath = lastCard->findChild<QLabel *>(QStringLiteral("transferPath"));
    QVERIFY(longPath->text().contains(QStringLiteral("…")));
    QVERIFY(longPath->toolTip().contains(QStringLiteral("第四季度")));

    QTest::mouseClick(lastCard, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
    QCOMPARE(chat->m_transferList->currentItem(), chat->m_transferList->topLevelItem(2));
    QVERIFY(lastCard->property("selected").toBool());
    QVERIFY(!firstCard->property("selected").toBool());
}

void SessionTabsTest::transferCardActionsRemainAvailable()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")), server.url());
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    context.setSession(auth);
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    window.showTransfersView();
    window.show();
    QVERIFY(window.m_transferEmpty->isVisible());
    QVERIFY(!window.m_transferList->isVisible());
    QCOMPARE(window.m_transferStats->text(), QStringLiteral("共 0 项"));

    TransferTask *task = window.transferManager()->addDownload(
        QStringLiteral("peer"), QStringLiteral("held-file"), QStringLiteral("report.pdf"),
        4 * 1024 * 1024);
    QVERIFY(task);
    QCOMPARE(task->state(), TransferTask::State::Downloading);
    QCOMPARE(window.m_transferList->topLevelItemCount(), 1);
    QVERIFY(!window.m_transferEmpty->isVisible());
    QVERIFY(window.m_transferList->isVisible());
    auto *card = qobject_cast<TransferTaskCard *>(
        window.m_transferList->itemWidget(window.m_transferList->topLevelItem(0), 0));
    QVERIFY(card);
    QCOMPARE(card->findChild<QLabel *>(QStringLiteral("transferFileName"))->text(),
             QStringLiteral("report.pdf"));
    QVERIFY(card->findChild<QLabel *>(QStringLiteral("transferPeer"))->text()
        .contains(QStringLiteral("PEER")));
    QCOMPARE(card->findChild<QLabel *>(QStringLiteral("transferState"))->text(),
             QStringLiteral("下载"));

    auto *pause = window.findChild<QPushButton *>(QStringLiteral("transferPauseButton"));
    auto *resume = window.findChild<QPushButton *>(QStringLiteral("transferResumeButton"));
    auto *retry = window.findChild<QPushButton *>(QStringLiteral("transferRetryButton"));
    auto *cancel = window.findChild<QPushButton *>(QStringLiteral("transferCancelButton"));
    QVERIFY(pause && resume && retry && cancel);
    pause->click();
    QCOMPARE(task->state(), TransferTask::State::Paused);
    QCOMPARE(card->findChild<QLabel *>(QStringLiteral("transferState"))->text(),
             QStringLiteral("已暂停"));
    QCoreApplication::processEvents();
    resume->click();
    QCOMPARE(task->state(), TransferTask::State::Downloading);
    cancel->click();
    QCOMPARE(task->state(), TransferTask::State::Cancelled);
    QCoreApplication::processEvents();
    retry->click();
    QCOMPARE(task->state(), TransferTask::State::Downloading);
    QVERIFY(window.m_transferStats->text().contains(QStringLiteral("共 1 项")));
    window.shutdown();
    context.shutdown();
}

void SessionTabsTest::contactUnreadDotsAreIndependentAndClearOnView()
{
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")), QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    context.setSession(auth);
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    User first;
    first.id = QStringLiteral("first-peer-uuid");
    first.username = QStringLiteral("USER_2");
    first.displayName = QStringLiteral("Bob Chen");
    User second;
    second.id = QStringLiteral("second-peer-uuid");
    second.username = QStringLiteral("USER_3");
    second.displayName = QStringLiteral("Carol Li");
    window.m_presenceKnown = true;
    window.m_onlineUserIds = {first.id};
    window.applyUsers({first, second});
    window.show();
    window.m_users->setCurrentItem(window.m_users->item(0));
    QCoreApplication::processEvents();

    QVERIFY(!window.m_sessionLabel->text().contains(auth.user.id));
    QVERIFY(!window.m_sessionLabel->toolTip().contains(auth.user.id));
    for (int index = 0; index < window.m_users->count(); ++index) {
        QListWidgetItem *item = window.m_users->item(index);
        const QString id = item->data(Qt::UserRole).toString();
        QVERIFY(!item->toolTip().contains(id));
        auto *name = window.m_users->itemWidget(item)->findChild<QLabel *>(
            QStringLiteral("contactNameLabel"));
        auto *presence = window.m_users->itemWidget(item)->findChild<QLabel *>(
            QStringLiteral("contactPresenceLabel"));
        QVERIFY(name && !name->text().isEmpty() && !name->text().contains(id));
        QVERIFY(presence && !presence->text().contains(id));
    }
    QVERIFY(!window.m_conversationLabel->text().contains(first.id));

    const auto dotFor = [&window](const QString &peerId) -> QLabel * {
        for (int index = 0; index < window.m_users->count(); ++index) {
            QListWidgetItem *item = window.m_users->item(index);
            if (item->data(Qt::UserRole).toString() == peerId)
                return window.m_users->itemWidget(item)->findChild<QLabel *>(
                    QStringLiteral("contactUnreadDot"));
        }
        return nullptr;
    };
    int serial = 0;
    const auto receive = [&window, &auth, &serial](const User &peer) {
        ++serial;
        WireMessage incoming;
        incoming.type = WireMessage::Type::Message;
        incoming.messageId = QStringLiteral("incoming-%1").arg(serial);
        incoming.senderId = peer.id;
        incoming.recipientId = auth.user.id;
        incoming.cursor = QString::number(serial);
        incoming.timestamp = serial;
        incoming.payload = QJsonDocument(textPayload(QStringLiteral("new message")))
            .toJson(QJsonDocument::Compact);
        window.handleWireMessage(incoming);
    };

    receive(second);
    receive(second);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 0);
    QCOMPARE(window.m_unreadByPeer.value(second.id), 2);
    window.setActive(false);
    receive(first);
    receive(second);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 1);
    QCOMPARE(window.m_unreadByPeer.value(second.id), 3);
    QCOMPARE(window.m_unreadCount, 4);
    QVERIFY(dotFor(first.id) && !dotFor(first.id)->isHidden());
    QVERIFY(dotFor(second.id) && !dotFor(second.id)->isHidden());

    window.applyUsers({second, first});
    QCOMPARE(window.m_peerId, first.id);
    QCOMPARE(window.m_unreadCount, 4);
    QVERIFY(!dotFor(first.id)->isHidden());
    QVERIFY(!dotFor(second.id)->isHidden());
    window.m_search->setText(second.id);
    QVERIFY(window.m_users->item(0)->isHidden() || window.m_users->item(1)->isHidden());
    QCOMPARE(window.m_unreadCount, 4);
    window.m_search->clear();
    QCOMPARE(window.m_unreadCount, 4);

    window.setActive(true);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 0);
    QCOMPARE(window.m_unreadByPeer.value(second.id), 3);
    QCOMPARE(window.m_unreadCount, 3);
    QVERIFY(dotFor(first.id)->isHidden());
    QVERIFY(!dotFor(second.id)->isHidden());

    window.showTransfersView();
    receive(first);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 1);
    QCOMPARE(window.m_unreadCount, 4);
    window.showChatView();
    QCOMPARE(window.m_unreadByPeer.value(first.id), 0);
    QCOMPARE(window.m_unreadCount, 3);
    window.m_users->setCurrentItem(window.m_users->item(0));
    QCOMPARE(window.m_peerId, second.id);
    QCOMPARE(window.m_unreadCount, 0);
    QVERIFY(dotFor(first.id)->isHidden());
    QVERIFY(dotFor(second.id)->isHidden());

    window.shutdown();
    context.shutdown();
}

void SessionTabsTest::contactFilterAllowsRegisteredCustomUsers()
{
    QTemporaryDir root;
    const QUrl serverUrl(QStringLiteral("http://127.0.0.1:1"));
    const AuthSession good = session(QStringLiteral("GOOD"), QStringLiteral("good-secret"));
    const QString registryPath = root.filePath(QStringLiteral("known_accounts.json"));

    {
        auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("user_1")), serverUrl);
        QVERIFY(context->open());
        SessionPage page(std::move(context));
        page.startSession(good);
        QVERIFY(page.context()->store()->knownAccountIds().contains(good.user.id));
        QVERIFY(page.context()->store()->saveSession(good));
    }

    // A saved custom account is registered again when its session is restored.
    QVERIFY(QFile::remove(registryPath));
    {
        auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("user_1")), serverUrl);
        QVERIFY(context->open());
        SessionPage restored(std::move(context));
        QVERIFY(restored.isAuthenticated());
        QVERIFY(restored.context()->store()->knownAccountIds().contains(good.user.id));
    }

    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("user_2")), serverUrl);
    QVERIFY(context->open());
    SessionPage observer(std::move(context));
    observer.startSession(session(QStringLiteral("CURRENT"), QStringLiteral("current-secret")));
    MainWindow *window = observer.chatPage();
    QVERIFY(window);

    User simulator;
    simulator.id = QStringLiteral("simulator-id");
    simulator.username = QStringLiteral("USER_2");
    User unknownAlice;
    unknownAlice.id = QStringLiteral("unknown-alice-id");
    unknownAlice.username = QStringLiteral("ALICE");
    User unknownIntegration;
    unknownIntegration.id = QStringLiteral("unknown-qtit-id");
    unknownIntegration.username = QStringLiteral("QTIT_RANDOM_123");
    User unknownOther;
    unknownOther.id = QStringLiteral("unknown-other-id");
    unknownOther.username = QStringLiteral("OTHER");
    window->applyUsers({good.user, unknownAlice, unknownIntegration, unknownOther, simulator});

    QCOMPARE(window->m_users->count(), 2);
    QSet<QString> visibleIds;
    for (int index = 0; index < window->m_users->count(); ++index)
        visibleIds.insert(window->m_users->item(index)->data(Qt::UserRole).toString());
    QVERIFY(visibleIds.contains(good.user.id));
    QVERIFY(visibleIds.contains(simulator.id));
    QVERIFY(!visibleIds.contains(unknownAlice.id));
    QVERIFY(!visibleIds.contains(unknownIntegration.id));
    QVERIFY(!visibleIds.contains(unknownOther.id));

    QFile registry(registryPath);
    QVERIFY(registry.open(QIODevice::ReadOnly));
    const QByteArray json = registry.readAll();
    QVERIFY(!json.contains("good-secret"));
    QVERIFY(!json.contains("current-secret"));
    QVERIFY(!json.contains("token"));
    observer.shutdown();
}

void SessionTabsTest::readReceiptsRespectVisibilityAndPersist()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")), server.url());
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    context.setSession(auth);
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    window.show();
    QVERIFY(!window.m_composer->isVisible());
    User peer;
    peer.id = QStringLiteral("peer-id");
    peer.username = QStringLiteral("USER_2");
    window.m_presenceKnown = true;
    window.m_onlineUserIds.insert(peer.id);
    User integrationUser;
    integrationUser.id = QStringLiteral("qt-integration-id");
    integrationUser.username = QStringLiteral("QTIT_PEER_ABC123");
    User legacyUser;
    legacyUser.id = QStringLiteral("legacy-test-id");
    legacyUser.username = QStringLiteral("ALICE");
    window.applyUsers({peer, integrationUser, legacyUser});
    QCOMPARE(window.m_users->count(), 1); // Only USER_<number> demo accounts appear.
    window.m_users->setCurrentItem(window.m_users->item(0));
    window.m_peerId = peer.id;
    window.updateConversationLabel();
    window.m_online = true;
    window.show();
    QVERIFY(window.m_composer->isVisible());
    auto *contactName = window.m_users->itemWidget(window.m_users->item(0))
        ->findChild<QLabel *>(QStringLiteral("contactNameLabel"));
    auto *contactPresence = window.m_users->itemWidget(window.m_users->item(0))
        ->findChild<QLabel *>(QStringLiteral("contactPresenceLabel"));
    QVERIFY(contactName && contactName->text().contains(QStringLiteral("USER_2")));
    QVERIFY(contactPresence && contactPresence->text().contains(QStringLiteral("在线")));
    QVERIFY(!contactPresence->text().contains(peer.id));
    QVERIFY(window.m_conversationLabel->text().contains(QStringLiteral("在线")));

    ChatMessage own;
    own.id = QStringLiteral("own-1");
    own.cursor = QStringLiteral("1");
    own.senderId = auth.user.id;
    own.recipientId = peer.id;
    own.content = QStringLiteral("own message");
    own.timestamp = 100;
    window.appendChat(peer.id, own);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("未读")));
    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = own.id;
    window.handleWireMessage(ack);
    QCOMPARE(context.store()->messages(peer.id).first().readAt, qint64(0));

    WireMessage read;
    read.type = WireMessage::Type::Read;
    read.senderId = peer.id;
    read.recipientId = auth.user.id;
    read.cursor = QStringLiteral("1");
    read.readAt = 1700000000123LL;
    window.handleWireMessage(read);
    QCOMPARE(context.store()->messages(peer.id).first().readAt, read.readAt);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("已读")));
    QCOMPARE(window.m_unreadCount, 0);
    QVERIFY(window.m_downloadIds.isEmpty());
    ChatMessage duplicate = own;
    duplicate.readAt = 0;
    window.appendChat(peer.id, duplicate);
    QCOMPARE(context.store()->messages(peer.id).first().readAt, read.readAt);

    window.setActive(false);
    WireMessage incoming;
    incoming.type = WireMessage::Type::Message;
    incoming.messageId = QStringLiteral("incoming-2");
    incoming.senderId = peer.id;
    incoming.recipientId = auth.user.id;
    incoming.cursor = QStringLiteral("2");
    incoming.timestamp = 200;
    incoming.payload = QJsonDocument(textPayload(QStringLiteral("background"))).toJson(QJsonDocument::Compact);
    window.handleWireMessage(incoming);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_unreadCount, 1);
    QCOMPARE(server.requests.size(), 0);
    window.handleWireMessage(read);
    QCOMPARE(window.m_sync.cursor(peer.id), QStringLiteral("2"));
    QCOMPARE(server.requests.size(), 0);

    ChatMessage ownLater = own;
    ownLater.id = QStringLiteral("own-3");
    ownLater.cursor = QStringLiteral("3");
    ownLater.readAt = 0;
    window.appendChat(peer.id, ownLater);
    WireMessage readerCopy = read;
    readerCopy.senderId = auth.user.id;
    readerCopy.recipientId = peer.id;
    readerCopy.cursor = QStringLiteral("2");
    window.handleWireMessage(readerCopy);
    const QList<ChatMessage> localMessages = context.store()->messages(peer.id);
    QCOMPARE(localMessages.last().id, ownLater.id);
    QCOMPARE(localMessages.last().readAt, qint64(0));
    QCOMPARE(localMessages.at(1).id, incoming.messageId);
    QCOMPARE(localMessages.at(1).readAt, readerCopy.readAt);
    QCOMPARE(window.m_readCursors.value(peer.id), QStringLiteral("1"));

    window.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QVERIFY(server.requests.first().startsWith("POST /api/v1/messages/read "));
    QVERIFY(server.requests.first().contains("\"peer_id\":\"peer-id\""));
    QVERIFY(server.requests.first().contains("\"cursor\":2"));
    window.shutdown();
    context.shutdown();
}

QTEST_MAIN(SessionTabsTest)
#include "test_session_tabs.moc"
