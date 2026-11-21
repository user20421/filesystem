#include "ApiClient.h"
#include "MainWindow.h"
#include "MessagePayloadCodec.h"
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
    void initialSessionShowsLoginAndRestoresSavedSession();
    void isolatesDirectoriesTokensAndObjects();
    void rejectsAndReleasesDirectoryLock();
    void simulatorRootLoginShowsOnlyStressPage();
    void loginUsesNormalAuthenticationAndUppercaseUsername();
    void refreshFailureStopsSessionAndReturnsToLogin();
    void exitRequiresConfirmationAndClearsOnlySession();
    void accountDeletionHandlesCancelFailureAndSuccess();
    void accountDeletionPreventsReentryAndClosesSafely();
    void pendingRequestsAndTransfersCloseSafely();
    void transferDirectoryUsesFakeOpenerOnce();
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

void SessionTabsTest::simulatorRootLoginShowsOnlyStressPage()
{
    HoldingServer server;
    QVERIFY(server.start());
    server.enqueue(200, QByteArray(R"({"data":{"user":{"id":"root-id","username":"ROOT","display_name":"ROOT"},"tokens":{"access_token":"real-root-token","refresh_token":"real-root-refresh"}}})"));
    QTemporaryDir root;
    SimulatorWindow window(root.filePath(QStringLiteral("simulator")), server.url());
    QVERIFY2(window.isOpen(), qPrintable(window.startupError()));
    QVERIFY(!window.isAuthenticated());
    QVERIFY(!window.findChild<QTabWidget *>());
    auto *username = window.findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = window.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = window.findChild<QPushButton *>(QStringLiteral("loginButton"));
    auto *registration = window.findChild<QPushButton *>(QStringLiteral("registerButton"));
    QVERIFY(username && password && login && registration);
    QCOMPARE(username->text(), QStringLiteral("ROOT"));
    QVERIFY(username->isReadOnly());
    QCOMPARE(password->text(), QStringLiteral("123456"));
    QVERIFY(registration->isHidden());

    login->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.isAuthenticated(), 3000);
    SystemTestPage *systemTest = window.systemTestPage();
    QVERIFY(systemTest);
    QVERIFY(systemTest->isVisible() || !window.isVisible());
    QVERIFY(systemTest->findChild<QPushButton *>(QStringLiteral("stressTestButton")));
    QVERIFY(!window.findChild<MainWindow *>());
    QVERIFY(!window.findChild<QTabWidget *>());
    QCOMPARE(window.context()->session().accessToken, QStringLiteral("real-root-token"));
    QCOMPARE(window.context()->api()->accessToken(), QStringLiteral("real-root-token"));
    QCOMPARE(systemTest->controller()->config().server, server.url().toString());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 3000);
    QVERIFY(server.requests.first().startsWith("POST /api/v1/auth/login "));
    QVERIFY(server.requests.first().contains("\"username\":\"ROOT\""));
    QVERIFY(server.requests.first().contains("\"password\":\"123456\""));
    QVERIFY(!server.requests.first().contains("real-root-token"));
    window.close();
}

void SessionTabsTest::refreshFailureStopsSessionAndReturnsToLogin()
{
    HoldingServer server;
    QVERIFY(server.start());
    server.enqueue(401, QByteArray("{\"error\":{\"message\":\"access expired\"}}"));
    server.enqueue(401, QByteArray("{\"error\":{\"message\":\"refresh expired\"}}"));
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    SessionPage page(std::move(context));
    page.startSession(session(QStringLiteral("alice"), QStringLiteral("test-token")), false);
    QVERIFY(page.context()->store()->saveSession(page.context()->session()));
    MainWindow *oldChat = page.chatPage();
    TransferManager *transfers = oldChat->transferManager();
    bool callbackDone = false;
    page.context()->api()->users([&](bool ok, const QList<User> &, const QString &) {
        QVERIFY(!ok);
        callbackDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!page.isAuthenticated() && callbackDone, 5000);
    QVERIFY(transfers->isClosing());
    QVERIFY(oldChat->isClosing());
    QVERIFY(page.context()->session().accessToken.isEmpty());
    QVERIFY(page.context()->session().refreshToken.isEmpty());
    QVERIFY(!QFileInfo::exists(page.context()->store()->path(QStringLiteral("config/session.json"))));
    auto *status = page.findChild<QLabel *>(QStringLiteral("loginStatus"));
    QVERIFY(status && status->text().contains(QStringLiteral("refresh expired")));
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests.at(0).startsWith("GET /api/v1/users "));
    QVERIFY(server.requests.at(1).startsWith("POST /api/v1/auth/refresh "));
    QVERIFY(server.requests.at(1).contains("\"refresh_token\":\"test-token-refresh\""));
    page.shutdown();
}

void SessionTabsTest::exitRequiresConfirmationAndClearsOnlySession()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    SessionPage page(std::move(context));
    page.startSession(session(QStringLiteral("alice"), QStringLiteral("test-token")), false);
    QVERIFY(page.context()->store()->saveSession(page.context()->session()));
    ChatMessage history;
    history.id = QStringLiteral("history-kept-after-logout");
    history.content = QStringLiteral("keep me");
    QVERIFY(page.context()->store()->appendMessage(QStringLiteral("peer"), history));
    const QString sessionPath = page.context()->store()->path(QStringLiteral("config/session.json"));
    const QString historyPath = page.context()->store()->path(QStringLiteral("config/messages.json"));
    auto *exit = page.chatPage()->findChild<QPushButton *>(QStringLiteral("exitButton"));
    QVERIFY(exit);
    QSignalSpy exitRequested(&page, &SessionPage::exitRequested);

    answerMessageBox(QMessageBox::No);
    exit->click();
    QCOMPARE(exitRequested.size(), 0);
    QVERIFY(QFileInfo::exists(sessionPath));
    QVERIFY(!page.isClosing());

    server.enqueue(500, QByteArray("{\"error\":{\"message\":\"logout unavailable\"}}"));
    answerMessageBox(QMessageBox::Yes);
    exit->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(exit->isEnabled(), 3000);
    QCOMPARE(exitRequested.size(), 0);
    QVERIFY(QFileInfo::exists(sessionPath));
    QVERIFY(page.chatPage()->m_notice->text().contains(QStringLiteral("logout unavailable")));

    server.enqueue(204);
    answerMessageBox(QMessageBox::Yes);
    exit->click();
    QTRY_VERIFY_WITH_TIMEOUT(!page.isAuthenticated(), 3000);
    QCOMPARE(exitRequested.size(), 0); // Logout returns to login; only account deletion closes the page.
    QVERIFY(!QFileInfo::exists(sessionPath));
    QVERIFY(QFileInfo::exists(historyPath));
    QVERIFY(!page.isClosing());
    QCOMPARE(server.requests.size(), 2);
    for (const QByteArray &request : server.requests) {
        QVERIFY(request.startsWith("POST /api/v1/auth/logout "));
        QVERIFY(request.contains("Authorization: Bearer test-token"));
        QVERIFY(request.contains("\"refresh_token\":\"test-token-refresh\""));
    }
    page.shutdown();
}

void SessionTabsTest::accountDeletionHandlesCancelFailureAndSuccess()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    SessionPage page(std::move(context));
    page.startSession(session(QStringLiteral("alice"), QStringLiteral("test-token")), false);
    QVERIFY(page.context()->store()->saveSession(page.context()->session()));
    const QString sessionPath = page.context()->store()->path(QStringLiteral("config/session.json"));
    auto *button = page.chatPage()->findChild<QPushButton *>(QStringLiteral("deleteAccountButton"));
    QVERIFY(button);
    QSignalSpy exitRequested(&page, &SessionPage::exitRequested);

    answerMessageBox(QMessageBox::No);
    button->click();
    QCOMPARE(server.requests.size(), 0);
    QVERIFY(QFileInfo::exists(sessionPath));

    server.enqueue(500, QByteArray("{\"error\":{\"message\":\"delete unavailable\"}}"));
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 3000);
    QCOMPARE(exitRequested.size(), 0);
    QVERIFY(QFileInfo::exists(sessionPath));
    QVERIFY(page.chatPage()->m_notice->text().contains(QStringLiteral("delete unavailable")));

    server.enqueue(204);
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(exitRequested.size() == 1, 3000);
    QVERIFY(!QFileInfo::exists(sessionPath));
    QCOMPARE(server.requests.size(), 2);
    for (const QByteArray &request : server.requests) {
        QVERIFY(request.startsWith("DELETE /api/v1/users/me "));
        QVERIFY(request.contains("Authorization: Bearer test-token"));
    }
    page.shutdown();
}

void SessionTabsTest::accountDeletionPreventsReentryAndClosesSafely()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    auto *page = new SessionPage(std::move(context));
    page->startSession(session(QStringLiteral("alice"), QStringLiteral("test-token")), false);
    QVERIFY(page->context()->store()->saveSession(page->context()->session()));
    const QString sessionPath = page->context()->store()->path(QStringLiteral("config/session.json"));
    auto *button = page->chatPage()->findChild<QPushButton *>(QStringLiteral("deleteAccountButton"));
    answerMessageBox(QMessageBox::Yes);
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 1, 3000);
    QVERIFY(!button->isEnabled());
    page->chatPage()->requestAccountDeletion();
    QCOMPARE(server.requests.size(), 1);
    QPointer<SessionPage> guarded(page);
    page->shutdown();
    delete page;
    QVERIFY(guarded.isNull());
    QVERIFY(QFileInfo::exists(sessionPath));
    QCoreApplication::processEvents();
    QCOMPARE(server.requests.size(), 1);
}

void SessionTabsTest::pendingRequestsAndTransfersCloseSafely()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    auto *page = new SessionPage(std::move(context));
    page->startSession(session(QStringLiteral("alice"), QStringLiteral("test-token")), false);
    int callbacks = 0;
    page->context()->api()->users([&callbacks](bool, const QList<User> &, const QString &) { ++callbacks; });
    QVERIFY(page->context()->api()->pendingRequestCount() > 0);
    QVERIFY(page->chatPage()->transferManager()->addDownload(
        QStringLiteral("peer"), QStringLiteral("active-file"), QStringLiteral("active.bin"), 1024 * 1024));
    QPointer<SessionPage> guarded(page);
    page->shutdown();
    delete page;
    QVERIFY(guarded.isNull());
    QCoreApplication::processEvents();
    QTest::qWait(80);
    QCOMPARE(callbacks, 0);
}

void SessionTabsTest::initialSessionShowsLoginAndRestoresSavedSession()
{
    QTemporaryDir root;
    const QUrl serverUrl(QStringLiteral("http://127.0.0.1:1"));
    const QString dataDir = root.filePath(QStringLiteral("alice"));
    {
        auto context = std::make_unique<SessionContext>(dataDir, serverUrl, QStringLiteral("alice"));
        QVERIFY(context->open());
        SessionPage page(std::move(context));
        QVERIFY(!page.isAuthenticated());
        QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("usernameEdit")));
        QVERIFY(page.findChild<QPushButton *>(QStringLiteral("loginButton")));
        QVERIFY(page.findChild<QPushButton *>(QStringLiteral("registerButton")));
        QCOMPARE(page.findChild<QLineEdit *>(QStringLiteral("usernameEdit"))->text(), QStringLiteral("ALICE"));
        QVERIFY(!page.chatPage());
        const AuthSession auth = session(QStringLiteral("ALICE"), QStringLiteral("saved-token"));
        QVERIFY(page.context()->store()->saveSession(auth));
    }
    {
        auto context = std::make_unique<SessionContext>(dataDir, serverUrl);
        QVERIFY(context->open());
        SessionPage restored(std::move(context));
        QVERIFY(restored.isAuthenticated());
        QCOMPARE(restored.context()->session().accessToken, QStringLiteral("saved-token"));
        QCOMPARE(restored.context()->api()->accessToken(), QStringLiteral("saved-token"));
        restored.shutdown();
    }
}

void SessionTabsTest::loginUsesNormalAuthenticationAndUppercaseUsername()
{
    HoldingServer server;
    QVERIFY(server.start());
    server.enqueue(200, QByteArray(R"({"data":{"user":{"id":"alice-id","username":"ALICE","display_name":"ALICE"},"tokens":{"access_token":"real-alice-token","refresh_token":"real-alice-refresh"}}})"));
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    SessionPage page(std::move(context));
    auto *username = page.findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = page.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = page.findChild<QPushButton *>(QStringLiteral("loginButton"));
    username->setText(QStringLiteral("alice"));
    password->setText(QStringLiteral("123456"));
    login->click();
    QCOMPARE(username->text(), QStringLiteral("ALICE"));
    QTRY_VERIFY_WITH_TIMEOUT(page.isAuthenticated(), 3000);
    QCOMPARE(page.context()->session().accessToken, QStringLiteral("real-alice-token"));
    QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(), 3000);
    QVERIFY(server.requests.first().startsWith("POST /api/v1/auth/login "));
    QVERIFY(server.requests.first().contains("\"username\":\"ALICE\""));
    QVERIFY(QFileInfo::exists(page.context()->store()->path(QStringLiteral("config/session.json"))));
    page.shutdown();
}

void SessionTabsTest::transferDirectoryUsesFakeOpenerOnce()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    context.setSession(auth);
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    TransferTask *task = window.transferManager()->addDownload(
        QStringLiteral("peer"), QStringLiteral("file-id"), QStringLiteral("report.pdf"), 1024 * 1024);
    QVERIFY(task);
    window.showTransfersView();
    window.show();
    QList<QUrl> opened;
    MainWindow::setDirectoryOpenerForTests([&](const QUrl &url) {
        opened.append(url);
        return true;
    });
    QTreeWidgetItem *row = window.m_transferList->topLevelItem(0);
    auto *card = qobject_cast<TransferTaskCard *>(window.m_transferList->itemWidget(row, 0));
    QVERIFY(row && card);
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
    QCOMPARE(window.m_transferList->currentItem(), row);
    QCOMPARE(opened.size(), 0);
    QTest::mouseClick(card->findChild<QLabel *>(QStringLiteral("transferFileName")), Qt::LeftButton);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().toLocalFile(), QDir::cleanPath(QFileInfo(task->localPath()).absolutePath()));
    window.findChild<QPushButton *>(QStringLiteral("openTransferDirectoryButton"))->click();
    QCOMPARE(opened.size(), 2);
    card->findChild<QPushButton *>(QStringLiteral("openTransferCardDirectoryButton"))->click();
    QCOMPARE(opened.size(), 3);
    MainWindow::setDirectoryOpenerForTests({});
    window.shutdown();
    context.shutdown();
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
        ChatMessage text;
        text.content = QStringLiteral("new message");
        QString payloadError;
        QVERIFY2(MessagePayloadCodec::encode(text, &incoming.payload, &payloadError),
                 qPrintable(payloadError));
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
    const QString firstRegistry = root.filePath(QStringLiteral("alice/config/known_accounts.json"));

    {
        auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), serverUrl);
        QVERIFY(context->open());
        SessionPage page(std::move(context));
        page.startSession(good);
        QVERIFY(page.context()->store()->knownAccountIds().contains(good.user.id));
        QVERIFY(page.context()->store()->saveSession(good));
    }

    // Restoring a session rebuilds only that user's private account registry.
    QVERIFY(QFile::remove(firstRegistry));
    {
        auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), serverUrl);
        QVERIFY(context->open());
        SessionPage restored(std::move(context));
        QVERIFY(restored.isAuthenticated());
        QVERIFY(restored.context()->store()->knownAccountIds().contains(good.user.id));
    }

    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("bob")), serverUrl);
    QVERIFY(context->open());
    SessionPage observer(std::move(context));
    observer.startSession(session(QStringLiteral("CURRENT"), QStringLiteral("current-secret")));
    MainWindow *window = observer.chatPage();
    QVERIFY(window);
    QVERIFY(!observer.context()->store()->knownAccountIds().contains(good.user.id));

    User simulator;
    simulator.id = QStringLiteral("simulator-id");
    simulator.username = QStringLiteral("USER_2");
    User unknownAlice;
    unknownAlice.id = QStringLiteral("unknown-alice-id");
    unknownAlice.username = QStringLiteral("ALICE");
    window->applyUsers({good.user, unknownAlice, simulator});
    QCOMPARE(window->m_users->count(), 1);
    QCOMPARE(window->m_users->item(0)->data(Qt::UserRole).toString(), simulator.id);

    QVERIFY(observer.context()->store()->rememberAccount(good.user));
    window->applyUsers({good.user, unknownAlice, simulator});
    QCOMPARE(window->m_users->count(), 2);
    const QString secondRegistry = observer.context()->store()->path(QStringLiteral("config/known_accounts.json"));
    QVERIFY(QFileInfo::exists(secondRegistry));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("known_accounts.json"))));
    QFile registry(secondRegistry);
    QVERIFY(registry.open(QIODevice::ReadOnly));
    const QByteArray json = registry.readAll();
    QVERIFY(json.contains("GOOD"));
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
    ChatMessage text;
    text.content = QStringLiteral("background");
    QString payloadError;
    QVERIFY2(MessagePayloadCodec::encode(text, &incoming.payload, &payloadError), qPrintable(payloadError));
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
