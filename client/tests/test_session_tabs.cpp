#include "ApiClient.h"
#include "MainWindow.h"
#include "MessageDelivery.h"
#include "MessagePayloadCodec.h"
#include "SessionContext.h"
#include "SessionPage.h"
#include "SingleUserWindow.h"
#include "SimulatorWindow.h"
#include "SystemTestPage.h"
#include "Theme.h"
#include "TransferTaskCard.h"
#include "support/TestEnvironment.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>

#include <algorithm>
#include <numeric>

namespace {
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
                    socket->setProperty("request", *buffer);
                    const int routed = responseIndexFor(*buffer);
                    if (routed >= 0) {
                        writeResponse(socket, m_responses.takeAt(routed));
                    } else if (!m_waiting.contains(socket)) {
                        m_waiting.append(socket);
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
    void enqueue(int status, const QByteArray &body = {})
    {
        enqueueFor({}, status, body);
    }
    void enqueueFor(const QByteArray &requestPrefix, int status, const QByteArray &body = {})
    {
        m_responses.append({requestPrefix, status, body});
        for (int waiting = 0; waiting < m_waiting.size() && !m_responses.isEmpty();) {
            QTcpSocket *socket = m_waiting.at(waiting);
            const int response = responseIndexFor(socket->property("request").toByteArray());
            if (response < 0) {
                ++waiting;
                continue;
            }
            m_waiting.removeAt(waiting);
            writeResponse(socket, m_responses.takeAt(response));
        }
    }
    QList<QByteArray> requests;
private:
    struct Response {
        QByteArray requestPrefix;
        int status;
        QByteArray body;
    };
    int responseIndexFor(const QByteArray &request) const
    {
        for (int index = 0; index < m_responses.size(); ++index) {
            if (m_responses.at(index).requestPrefix.isEmpty()
                || request.startsWith(m_responses.at(index).requestPrefix))
                return index;
        }
        return -1;
    }
    static void writeResponse(QTcpSocket *socket, const Response &response)
    {
        const QByteArray http = "HTTP/1.1 " + QByteArray::number(response.status)
            + " Test\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(response.body.size())
            + "\r\nConnection: close\r\n\r\n" + response.body;
        socket->write(http);
        socket->flush();
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<QTcpSocket *> m_sockets;
    QList<QTcpSocket *> m_waiting;
    QList<Response> m_responses;
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
    void sanitizesSpecialUserDirectories();
    void migratesPrefixedAndHistoricalDirectoriesByIdentity();
    void migratesValidLegacySessionAutomatically();
    void anonymousLegacyMigrationRequiresChoiceAndRejectsMerge();
    void restoresOnlyUniqueOrPreferredSession();
    void lateRefreshCannotWriteToNextUser();
    void simulatorRootLoginShowsOnlyStressPage();
    void integratedRootLoginIsolatesSessionAndReturnsToNormalLogin();
    void integratedRootLoginFailureStaysOnNormalLogin();
    void loginUsesNormalAuthenticationAndUppercaseUsername();
    void refreshFailureStopsSessionAndReturnsToLogin();
    void exitRequiresConfirmationAndClearsOnlySession();
    void accountDeletionHandlesCancelFailureAndSuccess();
    void accountDeletionPreventsReentryAndClosesSafely();
    void pendingRequestsAndTransfersCloseSafely();
    void transferDirectoryUsesFakeOpenerOnce();
    void transferCardActionsRemainAvailable();
    void fixedWindowAndDisabledSplitterSurvivePageChanges();
    void restoredUnreadDotsAppearWithoutSelectionAndClearOnRead();
    void contactUnreadDotsAreIndependentAndClearOnView();
    void contactFilterAllowsRegisteredCustomUsers();
    void readReceiptsRespectVisibilityAndPersist();
    void connectedOutboxHistoryIsSingleFlightPerPeer();
    void deliveryStatesRenderAndAckIsNotRead();

private:
    static int unreadTotal(const MainWindow &window);
};

int SessionTabsTest::unreadTotal(const MainWindow &window)
{
    return std::accumulate(window.m_unreadByPeer.cbegin(), window.m_unreadByPeer.cend(), 0,
                           [](int total, int count) { return total + count; });
}

void SessionTabsTest::isolatesDirectoriesTokensAndObjects()
{
    QTemporaryDir root;
    SessionContext first(root.filePath(QStringLiteral("base")), QUrl(QStringLiteral("http://localhost")));
    SessionContext second(root.filePath(QStringLiteral("base")), QUrl(QStringLiteral("http://localhost")));
    QVERIFY(first.open());
    QVERIFY(second.open());
    QVERIFY(first.store() == nullptr);
    QVERIFY(second.store() == nullptr);
    QVERIFY(first.activateUser(session(QStringLiteral("alice"), QStringLiteral("token-alice"))));
    QVERIFY(second.activateUser(session(QStringLiteral("bob"), QStringLiteral("token-bob"))));
    QCOMPARE(first.dataDir(), second.dataDir());
    QVERIFY(first.userDataDir() != second.userDataDir());
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
    const QString path = root.filePath(QStringLiteral("base"));
    const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
    auto first = std::make_unique<SessionContext>(path, QUrl(QStringLiteral("http://localhost")));
    QVERIFY(first->open());
    QVERIFY(first->activateUser(alice));
    SessionContext contender(path, QUrl(QStringLiteral("http://localhost")));
    QString error;
    QVERIFY(contender.open(&error));
    QVERIFY(!contender.activateUser(alice, false, &error));
    QVERIFY(error.contains(QStringLiteral("已被使用")));
    first->shutdown();
    first.reset();
    SessionContext reopened(path, QUrl(QStringLiteral("http://localhost")));
    QVERIFY2(reopened.open(&error), qPrintable(error));
    QVERIFY2(reopened.activateUser(alice, false, &error), qPrintable(error));
    reopened.shutdown();
}

void SessionTabsTest::sanitizesSpecialUserDirectories()
{
    QTemporaryDir root;
    QCOMPARE(SessionContext::userDirectoryName(QStringLiteral("alice")), QStringLiteral("ALICE"));
    const QStringList names{QStringLiteral("CON"), QStringLiteral("../CON"),
        QStringLiteral("A/B"), QStringLiteral("A\\B"), QStringLiteral("NAME."),
        QStringLiteral(".."), QStringLiteral("bad:name*?")};
    QSet<QString> paths;
    for (const QString &name : names) {
        const QString path = SessionContext::userDataPath(root.path(), name);
        const QString fileName = QFileInfo(path).fileName();
        QVERIFY(!fileName.startsWith(QStringLiteral("u-")));
        QVERIFY(!fileName.endsWith(QLatin1Char('.')));
        QVERIFY(!fileName.contains(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]"))));
        const QString relative = QDir(root.filePath(QStringLiteral("users"))).relativeFilePath(path);
        QVERIFY(!relative.startsWith(QStringLiteral("..")));
        paths.insert(QDir::cleanPath(path).toLower());
    }
    QCOMPARE(paths.size(), names.size());

    SessionContext context(root.path(), QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context.open());
    const AuthSession special = session(QStringLiteral("../con"), QStringLiteral("token"));
    QVERIFY(context.activateUser(special));
    QCOMPARE(context.userDataDir(), QDir(SessionContext::userDataPath(
        root.path(), special.user.username)).absolutePath());
    QVERIFY(context.store()->root().absolutePath().contains(QStringLiteral("/users/")));
    QVERIFY(!context.store()->root().absolutePath().contains(QStringLiteral("/users/u-")));
    context.shutdown();
}

void SessionTabsTest::migratesPrefixedAndHistoricalDirectoriesByIdentity()
{
    const auto writeMarker = [](const QString &storePath, const QString &name) {
        QFile marker(QDir(storePath).filePath(QStringLiteral("downloads/") + name));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("keep"), qint64(4));
    };
    const QUrl server(QStringLiteral("http://127.0.0.1:1"));

    {
        QTemporaryDir root;
        const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
        const QString source = root.filePath(QStringLiteral("users/u-ALICE"));
        LocalStore old(source);
        QVERIFY(old.initialize());
        QVERIFY(old.saveSession(alice));
        writeMarker(source, QStringLiteral("old.txt"));
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QVERIFY2(context.activateUser(alice), qPrintable(QStringLiteral("prefixed migration failed")));
        const QString target = SessionContext::userDataPath(root.path(), QStringLiteral("ALICE"));
        QCOMPARE(context.userDataDir(), QDir(target).absolutePath());
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(QFileInfo::exists(QDir(target).filePath(QStringLiteral("downloads/old.txt"))));
        context.shutdown();
    }
    {
        QTemporaryDir root;
        const AuthSession bob = session(QStringLiteral("BOB"), QStringLiteral("token"));
        const QString first = root.filePath(QStringLiteral("alice/users/u-BOB"));
        const QString second = root.filePath(QStringLiteral("bob/users/u-BOB"));
        for (const QString &source : {first, second}) {
            LocalStore old(source);
            QVERIFY(old.initialize());
            QVERIFY(old.saveSession(bob));
            writeMarker(source, QStringLiteral("old.txt"));
        }
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QString error;
        QVERIFY(!context.activateUser(bob, false, &error));
        QVERIFY(error.contains(QStringLiteral("多个")));
        QVERIFY(QFileInfo::exists(QDir(first).filePath(QStringLiteral("downloads/old.txt"))));
        QVERIFY(QFileInfo::exists(QDir(second).filePath(QStringLiteral("downloads/old.txt"))));
        QVERIFY(!QFileInfo::exists(SessionContext::userDataPath(root.path(), QStringLiteral("BOB"))));
        context.shutdown();
    }
    {
        QTemporaryDir root;
        const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
        const QString source = root.filePath(QStringLiteral("users/u-ALICE"));
        LocalStore old(source);
        QVERIFY(old.initialize());
        QVERIFY(old.saveSession(alice));
        writeMarker(source, QStringLiteral("old.txt"));
        const QString target = SessionContext::userDataPath(root.path(), QStringLiteral("ALICE"));
        QVERIFY(QDir().mkpath(QDir(target).filePath(QStringLiteral("downloads"))));
        writeMarker(target, QStringLiteral("existing.txt"));
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QString error;
        QVERIFY(!context.activateUser(alice, false, &error));
        QVERIFY(error.contains(QStringLiteral("禁止合并")));
        QVERIFY(QFileInfo::exists(QDir(source).filePath(QStringLiteral("downloads/old.txt"))));
        QVERIFY(QFileInfo::exists(QDir(target).filePath(QStringLiteral("downloads/existing.txt"))));
        context.shutdown();
    }
    {
        QTemporaryDir root;
        const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
        const QString source = root.filePath(QStringLiteral("alice/users/u-ALICE"));
        LocalStore old(source);
        QVERIFY(old.initialize());
        QVERIFY(old.saveSession(alice));
        writeMarker(source, QStringLiteral("old.txt"));
        QFile unrelated(root.filePath(QStringLiteral("alice/keep.txt")));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        QCOMPARE(unrelated.write("unrelated"), qint64(9));
        unrelated.close();
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QString error;
        QVERIFY2(context.activateUser(alice, false, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(context.store()->path(QStringLiteral("downloads/old.txt"))));
        QVERIFY(QFileInfo::exists(unrelated.fileName()));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("alice"))));
        context.shutdown();
    }
    {
        QTemporaryDir root;
        const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
        const QString source = root.filePath(QStringLiteral("users/u-ALICE"));
        LocalStore old(source);
        QVERIFY(old.initialize());
        QVERIFY(old.saveSession(session(QStringLiteral("BOB"), QStringLiteral("other"))));
        writeMarker(source, QStringLiteral("other.txt"));
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QVERIFY(context.activateUser(alice));
        QVERIFY(QFileInfo::exists(QDir(source).filePath(QStringLiteral("downloads/other.txt"))));
        QVERIFY(!QFileInfo::exists(context.store()->path(QStringLiteral("downloads/other.txt"))));
        context.shutdown();
    }
    {
        QTemporaryDir root;
        const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("token"));
        const QString source = root.filePath(QStringLiteral("users/u-ALICE"));
        LocalStore old(source);
        QVERIFY(old.initialize());
        QVERIFY(old.saveSession(alice));
        writeMarker(source, QStringLiteral("old.txt"));
        QLockFile oldLock(QDir(source).filePath(QStringLiteral("client.lock")));
        QVERIFY(oldLock.tryLock(0));
        SessionContext context(root.path(), server);
        QVERIFY(context.open());
        QString error;
        QVERIFY(!context.activateUser(alice, false, &error));
        QVERIFY(error.contains(QStringLiteral("锁")));
        QVERIFY(QFileInfo::exists(QDir(source).filePath(QStringLiteral("downloads/old.txt"))));
        QVERIFY(!QFileInfo::exists(SessionContext::userDataPath(root.path(), QStringLiteral("ALICE"))));
        oldLock.unlock();
        QVERIFY2(context.activateUser(alice, false, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(context.store()->path(QStringLiteral("downloads/old.txt"))));
        context.shutdown();
    }
}

void SessionTabsTest::migratesValidLegacySessionAutomatically()
{
    QTemporaryDir root;
    const AuthSession alice = session(QStringLiteral("alice"), QStringLiteral("legacy-token"));
    {
        LocalStore legacy(root.path());
        QVERIFY(legacy.initialize());
        QVERIFY(legacy.saveSession(alice));
        QFile marker(legacy.path(QStringLiteral("downloads/old.txt")));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("old-download"), qint64(12));
    }

    auto context = std::make_unique<SessionContext>(root.path(),
        QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context->open());
    QCOMPARE(context->sessionToRestore().user.username, QStringLiteral("alice"));
    SessionPage page(std::move(context));
    QVERIFY(page.isAuthenticated());
    QCOMPARE(page.context()->session().user.username, QStringLiteral("ALICE"));
    const QString expected = SessionContext::userDataPath(root.path(), QStringLiteral("ALICE"));
    QCOMPARE(page.context()->userDataDir(), QDir(expected).absolutePath());
    QVERIFY(QFileInfo::exists(QDir(expected).filePath(QStringLiteral("config/session.json"))));
    QVERIFY(QFileInfo::exists(QDir(expected).filePath(QStringLiteral("downloads/old.txt"))));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("config"))));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("downloads/old.txt"))));
    page.shutdown();
}

void SessionTabsTest::anonymousLegacyMigrationRequiresChoiceAndRejectsMerge()
{
    const auto createAnonymousLegacy = [](const QString &base) {
        LocalStore legacy(base);
        QVERIFY(legacy.initialize());
        QFile history(legacy.path(QStringLiteral("config/messages.json")));
        QVERIFY(history.open(QIODevice::WriteOnly));
        QCOMPARE(history.write("{}"), qint64(2));
        QFile download(legacy.path(QStringLiteral("downloads/old.txt")));
        QVERIFY(download.open(QIODevice::WriteOnly));
        QCOMPARE(download.write("legacy"), qint64(6));
    };
    {
        QTemporaryDir root;
        createAnonymousLegacy(root.path());
        auto context = std::make_unique<SessionContext>(root.path(),
            QUrl(QStringLiteral("http://127.0.0.1:1")));
        QVERIFY(context->open());
        SessionPage page(std::move(context));
        QVERIFY(!page.isAuthenticated());
        answerMessageBox(QMessageBox::Yes);
        page.startSession(session(QStringLiteral("alice"), QStringLiteral("token")), false);
        QVERIFY(page.isAuthenticated());
        QVERIFY(QFileInfo::exists(page.context()->store()->path(
            QStringLiteral("downloads/old.txt"))));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("config/messages.json"))));
        page.shutdown();
    }
    {
        QTemporaryDir root;
        createAnonymousLegacy(root.path());
        const QString target = SessionContext::userDataPath(root.path(), QStringLiteral("ALICE"));
        QVERIFY(QDir().mkpath(QDir(target).filePath(QStringLiteral("config"))));
        QFile existing(QDir(target).filePath(QStringLiteral("config/existing.txt")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("keep"), qint64(4));
        existing.close();

        auto context = std::make_unique<SessionContext>(root.path(),
            QUrl(QStringLiteral("http://127.0.0.1:1")));
        QVERIFY(context->open());
        SessionPage page(std::move(context));
        answerMessageBox(QMessageBox::Yes);
        page.startSession(session(QStringLiteral("alice"), QStringLiteral("token")), false);
        QVERIFY(!page.isAuthenticated());
        QVERIFY(page.findChild<QLabel *>(QStringLiteral("loginStatus"))->text()
            .contains(QStringLiteral("禁止合并")));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("config/messages.json"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("downloads/old.txt"))));
        QVERIFY(QFileInfo::exists(existing.fileName()));
        page.shutdown();
    }
}

void SessionTabsTest::restoresOnlyUniqueOrPreferredSession()
{
    const auto saveUser = [](const QString &base, const AuthSession &auth) {
        SessionContext writer(base, QUrl(QStringLiteral("http://127.0.0.1:1")));
        QVERIFY(writer.open());
        QVERIFY(writer.activateUser(auth));
        QVERIFY(writer.store()->saveSession(writer.session()));
        writer.shutdown();
    };
    const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("alice-token"));
    const AuthSession bob = session(QStringLiteral("BOB"), QStringLiteral("bob-token"));
    {
        QTemporaryDir root;
        saveUser(root.path(), alice);
        SessionContext unique(root.path(), QUrl(QStringLiteral("http://127.0.0.1:1")));
        QVERIFY(unique.open());
        QCOMPARE(unique.sessionToRestore().user.username, QStringLiteral("ALICE"));
        unique.shutdown();
        saveUser(root.path(), bob);

        SessionContext multiple(root.path(), QUrl(QStringLiteral("http://127.0.0.1:1")));
        QVERIFY(multiple.open());
        QVERIFY(multiple.sessionToRestore().accessToken.isEmpty());
        multiple.shutdown();
        SessionContext targeted(root.path(), QUrl(QStringLiteral("http://127.0.0.1:1")),
                                QStringLiteral("bob"));
        QVERIFY(targeted.open());
        QCOMPARE(targeted.sessionToRestore().accessToken, QStringLiteral("bob-token"));
        targeted.shutdown();
    }
}

void SessionTabsTest::lateRefreshCannotWriteToNextUser()
{
    HoldingServer server;
    QVERIFY(server.start());
    server.enqueue(401, QByteArray("{\"error\":{\"message\":\"expired\"}}"));
    QTemporaryDir root;
    SessionContext context(root.path(), server.url());
    QVERIFY(context.open());
    const AuthSession alice = session(QStringLiteral("ALICE"), QStringLiteral("alice-token"));
    QVERIFY(context.activateUser(alice));
    QVERIFY(context.store()->saveSession(context.session()));
    const QString aliceSession = context.store()->path(QStringLiteral("config/session.json"));
    context.api()->users([](bool, const QList<User> &, const QString &) {});
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 3000);
    QVERIFY(server.requests.at(1).startsWith("POST /api/v1/auth/refresh "));

    context.store()->clearSession();
    context.closeUser();
    QVERIFY(context.store() == nullptr);
    const AuthSession bob = session(QStringLiteral("BOB"), QStringLiteral("bob-token"));
    QVERIFY(context.activateUser(bob));
    QVERIFY(context.store()->saveSession(context.session()));
    const QString bobSession = context.store()->path(QStringLiteral("config/session.json"));
    server.enqueue(200, QByteArray(R"({"data":{"user":{"id":"ALICE-id","username":"ALICE"},"tokens":{"access_token":"late-alice","refresh_token":"late-alice-refresh"}}})"));
    QTest::qWait(100);
    QCoreApplication::processEvents();
    QCOMPARE(context.session().user.username, QStringLiteral("BOB"));
    QCOMPARE(context.api()->accessToken(), QStringLiteral("bob-token"));
    QCOMPARE(context.store()->loadSession().accessToken, QStringLiteral("bob-token"));
    QVERIFY(!QFileInfo::exists(aliceSession));
    QVERIFY(QFileInfo::exists(bobSession));
    context.shutdown();
}

void SessionTabsTest::simulatorRootLoginShowsOnlyStressPage()
{
    HoldingServer server;
    QVERIFY(server.start());
    server.enqueue(200, QByteArray(R"({"data":{"user":{"id":"root-id","username":"ROOT","display_name":"ROOT"},"tokens":{"access_token":"real-root-token","refresh_token":"real-root-refresh"}}})"));
    QTemporaryDir root;
    ScopedEnvironment projectRoot("SECURE_IM_PROJECT_ROOT");
    ScopedEnvironment skipSetup("SECURE_IM_STRESS_SKIP_SETUP");
    qputenv("SECURE_IM_PROJECT_ROOT", root.path().toUtf8());
    qputenv("SECURE_IM_STRESS_SKIP_SETUP", "1");
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

void SessionTabsTest::integratedRootLoginIsolatesSessionAndReturnsToNormalLogin()
{
    ScopedEnvironment stressUrl("SECURE_IM_STRESS_SERVER");
    ScopedEnvironment projectRoot("SECURE_IM_PROJECT_ROOT");
    ScopedEnvironment skipSetup("SECURE_IM_STRESS_SKIP_SETUP");
    QTemporaryDir root;
    qputenv("SECURE_IM_PROJECT_ROOT", root.path().toUtf8());
    qputenv("SECURE_IM_STRESS_SKIP_SETUP", "1");
    qunsetenv("SECURE_IM_STRESS_SERVER");
    QCOMPARE(StressRunConfig::forProject(root.path()).server,
             QStringLiteral("http://127.0.0.1:18080"));

    HoldingServer normalServer;
    HoldingServer stressServer;
    QVERIFY(normalServer.start());
    QVERIFY(stressServer.start());
    qputenv("SECURE_IM_STRESS_SERVER", stressServer.url().toString().toUtf8());
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")),
                                                    normalServer.url());
    QVERIFY(context->open());
    SingleUserWindow window(std::move(context));
    window.show();
    const QSize fixedSize(560, 390);
    QCOMPARE(window.size(), fixedSize);

    // ROOT authentication must leave another normal user's directory untouched.
    const AuthSession normal = session(QStringLiteral("ALICE"), QStringLiteral("normal-token"));
    const QString normalDir = SessionContext::userDataPath(root.filePath(QStringLiteral("alice")),
                                                           normal.user.username);
    LocalStore normalStore(normalDir);
    QVERIFY(normalStore.initialize());
    QVERIFY(normalStore.saveSession(normal));
    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const QString normalSessionPath = normalStore.path(QStringLiteral("config/session.json"));
    const QString normalAccountsPath = normalStore.path(QStringLiteral("config/known_accounts.json"));
    QFile legacyAccounts(normalAccountsPath);
    QVERIFY(legacyAccounts.open(QIODevice::WriteOnly));
    const QByteArray savedAccounts = R"([{"id":"legacy-id","username":"LEGACY"}])";
    QCOMPARE(legacyAccounts.write(savedAccounts), savedAccounts.size());
    legacyAccounts.close();
    const QByteArray savedSession = readFile(normalSessionPath);
    QVERIFY(!savedSession.isEmpty());
    QCOMPARE(readFile(normalAccountsPath), savedAccounts);

    auto *username = window.findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = window.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = window.findChild<QPushButton *>(QStringLiteral("loginButton"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("loginStatus"));
    QVERIFY(username && password && login && status);
    stressServer.enqueue(401, QByteArray("{\"error\":{\"message\":\"bad root password\"}}"));
    username->setText(QStringLiteral("root"));
    password->setText(QStringLiteral("bad123"));
    login->click();
    QTRY_COMPARE_WITH_TIMEOUT(stressServer.requests.size(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(login->isEnabled(), 3000);
    QVERIFY(status->text().contains(QStringLiteral("bad root password")));
    QVERIFY(!window.sessionPage()->systemTestPage());
    QVERIFY(!window.sessionPage()->isAuthenticated());
    QCOMPARE(window.size(), fixedSize);
    QCOMPARE(normalServer.requests.size(), 0);
    QCOMPARE(readFile(normalSessionPath), savedSession);
    QCOMPARE(readFile(normalAccountsPath), savedAccounts);

    stressServer.enqueue(200, QByteArray(R"({"data":{"user":{"id":"root-id","username":"ROOT","display_name":"ROOT"},"tokens":{"access_token":"integrated-root-token","refresh_token":"integrated-root-refresh"}}})"));
    auto *openSystemTest = window.findChild<QPushButton *>(QStringLiteral("openSystemTestButton"));
    QVERIFY(openSystemTest);
    openSystemTest->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.sessionPage()->systemTestPage(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(stressServer.requests.size(), 2, 3000);
    SystemTestPage *stressPage = window.sessionPage()->systemTestPage();
    QVERIFY(stressPage->findChild<QPushButton *>(QStringLiteral("stressTestButton")));
    QCOMPARE(stressPage->controller()->config().server, stressServer.url().toString());
    QCOMPARE(window.sessionPage()->context()->api()->accessToken(), QString{});
    QCOMPARE(window.sessionPage()->context()->session().accessToken, QString{});
    QCOMPARE(readFile(normalSessionPath), savedSession);
    QCOMPARE(readFile(normalAccountsPath), savedAccounts);
    QVERIFY(QFileInfo::exists(QDir(window.sessionPage()->context()->dataDir()).filePath(
        QStringLiteral("root-stress/config/session.json"))));
    QVERIFY(!QFileInfo::exists(QDir(window.sessionPage()->context()->dataDir()).filePath(
        QStringLiteral("users/ROOT"))));
    QVERIFY(!window.sessionPage()->chatPage());
    QVERIFY(window.width() >= 960 && window.height() >= 640);
    QVERIFY(window.maximumWidth() > fixedSize.width());
    QScreen *stressScreen = QGuiApplication::screenAt(window.frameGeometry().center());
    QVERIFY(stressScreen);
    const QRect stressAvailable = stressScreen->availableGeometry();
    if (stressAvailable.width() >= window.frameGeometry().width()
        && stressAvailable.height() >= window.frameGeometry().height()) {
        const QPoint centerDelta = window.frameGeometry().center() - stressAvailable.center();
        QVERIFY(centerDelta.manhattanLength() <= 2);
    }
    QCOMPARE(normalServer.requests.size(), 0);
    for (const QByteArray &request : stressServer.requests) {
        QVERIFY(request.startsWith("POST /api/v1/auth/login "));
        QVERIFY(request.contains("\"username\":\"ROOT\""));
    }
    QVERIFY(stressServer.requests.at(0).contains("\"password\":\"bad123\""));
    QVERIFY(stressServer.requests.at(1).contains("\"password\":\"123456\""));

    auto *back = window.findChild<QPushButton *>(QStringLiteral("returnToLoginButton"));
    QVERIFY(back);
    back->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.sessionPage()->systemTestPage(), 3000);
    QCOMPARE(window.size(), fixedSize);
    QCOMPARE(window.minimumSize(), fixedSize);
    QCOMPARE(window.maximumSize(), fixedSize);
    QCOMPARE(readFile(normalSessionPath), savedSession);
    QCOMPARE(readFile(normalAccountsPath), savedAccounts);

    normalServer.enqueue(200, QByteArray(R"({"data":{"user":{"id":"alice-id","username":"ALICE","display_name":"ALICE"},"tokens":{"access_token":"normal-after-root","refresh_token":"normal-after-root-refresh"}}})"));
    username->setText(QStringLiteral("alice"));
    password->setText(QStringLiteral("123456"));
    login->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.sessionPage()->isAuthenticated(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!normalServer.requests.isEmpty(), 3000);
    QVERIFY(normalServer.requests.first().startsWith("POST /api/v1/auth/login "));
    QVERIFY(normalServer.requests.first().contains("\"username\":\"ALICE\""));
    QCOMPARE(window.sessionPage()->context()->api()->accessToken(),
             QStringLiteral("normal-after-root"));
    QCOMPARE(window.size(), fixedSize);
    window.close();
}

void SessionTabsTest::integratedRootLoginFailureStaysOnNormalLogin()
{
    ScopedEnvironment stressUrl("SECURE_IM_STRESS_SERVER");
    ScopedEnvironment projectRoot("SECURE_IM_PROJECT_ROOT");
    ScopedEnvironment skipSetup("SECURE_IM_STRESS_SKIP_SETUP");
    QTemporaryDir root;
    qputenv("SECURE_IM_PROJECT_ROOT", root.path().toUtf8());
    qputenv("SECURE_IM_STRESS_SKIP_SETUP", "1");
    qputenv("SECURE_IM_STRESS_SERVER", "http://127.0.0.1:1");
    HoldingServer normalServer;
    QVERIFY(normalServer.start());
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")),
                                                    normalServer.url());
    QVERIFY(context->open());
    SingleUserWindow window(std::move(context));
    window.show();
    auto *username = window.findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = window.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = window.findChild<QPushButton *>(QStringLiteral("loginButton"));
    auto *status = window.findChild<QLabel *>(QStringLiteral("loginStatus"));
    QVERIFY(username && password && login && status);
    username->setText(QStringLiteral("ROOT"));
    password->setText(QStringLiteral("123456"));
    login->click();
    QTRY_VERIFY_WITH_TIMEOUT(login->isEnabled()
        && !status->text().isEmpty()
        && status->text() != QStringLiteral("正在登录…"), 10000);
    QVERIFY(!window.sessionPage()->systemTestPage());
    QVERIFY(!window.sessionPage()->isAuthenticated());
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("usernameEdit"))->isVisible());
    QCOMPARE(window.size(), QSize(560, 390));
    QCOMPARE(normalServer.requests.size(), 0);
    QVERIFY(!QFileInfo::exists(QDir(window.sessionPage()->context()->dataDir()).filePath(
        QStringLiteral("config/session.json"))));
    QVERIFY(!QFileInfo::exists(QDir(window.sessionPage()->context()->dataDir()).filePath(
        QStringLiteral("users/ROOT"))));
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
    const QString expiredSessionPath = page.context()->store()->path(
        QStringLiteral("config/session.json"));
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
    QVERIFY(!QFileInfo::exists(expiredSessionPath));
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
        page.startSession(auth, false);
        QVERIFY(page.isAuthenticated());
        QVERIFY(QFileInfo::exists(SessionContext::userDataPath(dataDir, auth.user.username)
            + QStringLiteral("/config/session.json")));
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
    QCOMPARE(password->text(), QStringLiteral("123456"));
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
    QVERIFY(context.activateUser(auth));
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
    QVERIFY(context.activateUser(auth));
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

void SessionTabsTest::fixedWindowAndDisabledSplitterSurvivePageChanges()
{
    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")),
        QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context->open());
    SingleUserWindow window(std::move(context));
    window.show();
    const QSize fixedSize(560, 390);
    QCOMPARE(window.size(), fixedSize);
    QCOMPARE(window.minimumSize(), fixedSize);
    QCOMPARE(window.maximumSize(), fixedSize);
    QVERIFY(!window.sessionPage()->isAuthenticated());
    auto *username = window.findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *password = window.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    QVERIFY(username && password);
    QVERIFY(username->height() >= 36 && password->height() >= 36);

    window.sessionPage()->startSession(session(QStringLiteral("alice"), QStringLiteral("token")), false);
    QCoreApplication::processEvents();
    QCOMPARE(window.size(), fixedSize);
    QCOMPARE(window.minimumSize(), fixedSize);
    QCOMPARE(window.maximumSize(), fixedSize);
    window.resize(900, 700);
    QCOMPARE(window.size(), fixedSize);

    auto *splitter = window.sessionPage()->chatPage()->findChild<QSplitter *>(
        QStringLiteral("chatSplitter"));
    QVERIFY(splitter);
    QCOMPARE(splitter->handleWidth(), 0);
    QVERIFY(splitter->handle(1));
    QVERIFY(!splitter->handle(1)->isEnabled());
    QCOMPARE(splitter->sizes().size(), 2);
    QVERIFY(splitter->sizes().at(0) > 0);
    QVERIFY(splitter->sizes().at(1) > 0);
    window.close();
}

void SessionTabsTest::restoredUnreadDotsAppearWithoutSelectionAndClearOnRead()
{
    HoldingServer server;
    QVERIFY(server.start());
    // 启动请求并行到达，按接口路由响应，避免 TCP readyRead 顺序造成偶发错配。
    server.enqueueFor("GET /api/v1/messages/unread ", 200, QByteArray(R"({"data":{"unread":[{"peer_id":"peer-id","unread_count":2,"last_message_at":1760000000123}]}})"));
    server.enqueueFor("GET /api/v1/presence ", 200, QByteArray(R"({"data":{"online_user_ids":[]}})"));
    server.enqueueFor("GET /api/v1/users ", 200, QByteArray(R"({"data":{"users":[{"id":"alice-id","username":"ALICE"},{"id":"peer-id","username":"BOB"}]}})"));

    QTemporaryDir root;
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("alice")), server.url());
    QVERIFY(context->open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    QVERIFY(context->activateUser(auth));
    QVERIFY(context->store()->saveSession(auth));
    ChatMessage local;
    local.id = QStringLiteral("offline-local");
    local.cursor = QStringLiteral("7");
    local.senderId = QStringLiteral("peer-id");
    local.recipientId = auth.user.id;
    local.content = QStringLiteral("offline");
    QVERIFY(context->store()->appendMessage(QStringLiteral("peer-id"), local));

    SessionPage page(std::move(context));
    page.show();
    QVERIFY(page.isAuthenticated());
    MainWindow *window = page.chatPage();
    QTRY_VERIFY_WITH_TIMEOUT(window->m_users->count() == 1, 3000);
    QCOMPARE(window->m_peerId, QString{});
    QTRY_COMPARE_WITH_TIMEOUT(window->m_unreadByPeer.value(QStringLiteral("peer-id")), 2, 3000);
    QCOMPARE(unreadTotal(*window), 2);
    auto *dot = window->m_users->itemWidget(window->m_users->item(0))
        ->findChild<QLabel *>(QStringLiteral("contactUnreadDot"));
    QVERIFY(dot && !dot->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() >= 3, 3000);
    QVERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
        return request.startsWith("GET /api/v1/messages/unread ");
    }));

    server.enqueueFor("GET /api/v1/messages?", 200, QByteArray(R"({"data":{"messages":[]}})"));
    server.enqueueFor("POST /api/v1/messages/read ", 200, QByteArray(R"({"data":{}})"));
    window->m_users->setCurrentItem(window->m_users->item(0));
    QTRY_VERIFY_WITH_TIMEOUT(unreadTotal(*window) == 0, 3000);
    QVERIFY(dot->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() >= 6, 3000);
    bool readRequested = false;
    for (const QByteArray &request : server.requests) {
        if (request.startsWith("POST /api/v1/messages/read ")) {
            readRequested = true;
            QVERIFY(request.contains("\"peer_id\":\"peer-id\""));
            QVERIFY(request.contains("\"cursor\":7"));
        }
    }
    QVERIFY(readRequested);
    page.shutdown();
}

void SessionTabsTest::contactUnreadDotsAreIndependentAndClearOnView()
{
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")), QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    QVERIFY(context.activateUser(auth));
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
    QCOMPARE(unreadTotal(window), 4);
    QVERIFY(dotFor(first.id) && !dotFor(first.id)->isHidden());
    QVERIFY(dotFor(second.id) && !dotFor(second.id)->isHidden());

    window.applyUsers({second, first});
    QCOMPARE(window.m_peerId, first.id);
    QCOMPARE(unreadTotal(window), 4);
    QVERIFY(!dotFor(first.id)->isHidden());
    QVERIFY(!dotFor(second.id)->isHidden());
    window.m_search->setText(second.id);
    QVERIFY(window.m_users->item(0)->isHidden() || window.m_users->item(1)->isHidden());
    QCOMPARE(unreadTotal(window), 4);
    window.m_search->clear();
    QCOMPARE(unreadTotal(window), 4);

    window.setActive(true);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 0);
    QCOMPARE(window.m_unreadByPeer.value(second.id), 3);
    QCOMPARE(unreadTotal(window), 3);
    QVERIFY(dotFor(first.id)->isHidden());
    QVERIFY(!dotFor(second.id)->isHidden());

    window.showTransfersView();
    receive(first);
    QCOMPARE(window.m_unreadByPeer.value(first.id), 1);
    QCOMPARE(unreadTotal(window), 4);
    window.showChatView();
    QCOMPARE(window.m_unreadByPeer.value(first.id), 0);
    QCOMPARE(unreadTotal(window), 3);
    window.m_users->setCurrentItem(window.m_users->item(0));
    QCOMPARE(window.m_peerId, second.id);
    QCOMPARE(unreadTotal(window), 0);
    QVERIFY(dotFor(first.id)->isHidden());
    QVERIFY(dotFor(second.id)->isHidden());

    window.shutdown();
    context.shutdown();
}

void SessionTabsTest::contactFilterAllowsRegisteredCustomUsers()
{
    QTemporaryDir root;
    const QUrl serverUrl(QStringLiteral("http://127.0.0.1:1"));
    auto context = std::make_unique<SessionContext>(root.filePath(QStringLiteral("bob")), serverUrl);
    QVERIFY(context->open());
    SessionPage observer(std::move(context));
    observer.startSession(session(QStringLiteral("CURRENT"), QStringLiteral("current-secret")), false);
    MainWindow *window = observer.chatPage();
    QVERIFY(window);

    const AuthSession good = session(QStringLiteral("GOOD"), QStringLiteral("good-secret"));
    User simulator;
    simulator.id = QStringLiteral("simulator-id");
    simulator.username = QStringLiteral("USER_2");
    User unknownAlice;
    unknownAlice.id = QStringLiteral("unknown-alice-id");
    unknownAlice.username = QStringLiteral("ALICE");
    window->applyUsers({good.user, unknownAlice, simulator});
    QCOMPARE(window->m_users->count(), 3);
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
    QVERIFY(context.activateUser(auth));
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
    QCOMPARE(window.m_users->count(), 3); // All registered users appear; only the current account is hidden.
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
    own.senderId = auth.user.id;
    own.recipientId = peer.id;
    own.content = QStringLiteral("own message");
    own.timestamp = 100;
    window.appendChat(peer.id, own);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("未读")));
    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = own.id;
    ack.cursor = QStringLiteral("1");
    window.handleWireMessage(ack);
    QCOMPARE(context.store()->messages(peer.id).first().readAt, qint64(0));
    QVERIFY(context.store()->messages(peer.id).first().acked);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("已确认未读")));
    QCOMPARE(window.m_sync.cursor(peer.id), QString{});

    WireMessage read;
    read.type = WireMessage::Type::Read;
    read.senderId = peer.id;
    read.recipientId = auth.user.id;
    read.cursor = QStringLiteral("1");
    read.readAt = 1700000000123LL;
    window.handleWireMessage(read);
    QCOMPARE(context.store()->messages(peer.id).first().readAt, read.readAt);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("已读")));
    QCOMPARE(unreadTotal(window), 0);
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
    QCOMPARE(unreadTotal(window), 1);
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

void SessionTabsTest::deliveryStatesRenderAndAckIsNotRead()
{
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")),
                           QUrl(QStringLiteral("http://127.0.0.1:1")));
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    QVERIFY(context.activateUser(auth));
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    const QString peer = QStringLiteral("peer");
    window.m_peerId = peer;
    ChatMessage message;
    message.id = QStringLiteral("states");
    message.cursor = QStringLiteral("7");
    message.senderId = auth.user.id;
    message.recipientId = peer;
    message.content = QStringLiteral("hello");
    message.pending = true;
    window.appendChat(peer, message);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("等待发送")));
    message.attemptCount = 1;
    message.nextAttemptAt = 100;
    window.appendChat(peer, message);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("等待ACK")));
    message.pending = false;
    message.failed = true;
    message.deliveryError = QStringLiteral("no ack");
    window.appendChat(peer, message);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("失败")));

    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = message.id;
    window.handleWireMessage(ack);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("已确认未读")));
    QVERIFY(!window.m_chat->toPlainText().contains(QStringLiteral("已读 ")));
    WireMessage read;
    read.type = WireMessage::Type::Read;
    read.senderId = peer;
    read.recipientId = auth.user.id;
    read.cursor = QStringLiteral("7");
    read.readAt = 1700000000123LL;
    window.handleWireMessage(read);
    QVERIFY(window.m_chat->toPlainText().contains(QStringLiteral("已读")));
    window.shutdown();
    context.shutdown();
}

void SessionTabsTest::connectedOutboxHistoryIsSingleFlightPerPeer()
{
    HoldingServer server;
    QVERIFY(server.start());
    QTemporaryDir root;
    SessionContext context(root.filePath(QStringLiteral("user")), server.url());
    QVERIFY(context.open());
    const AuthSession auth = session(QStringLiteral("alice"), QStringLiteral("token"));
    QVERIFY(context.activateUser(auth));
    for (const QString &peer : {QStringLiteral("peer-a"), QStringLiteral("peer-b")}) {
        ChatMessage pending;
        pending.id = QStringLiteral("pending-") + peer;
        pending.senderId = auth.user.id;
        pending.recipientId = peer;
        pending.content = QStringLiteral("offline");
        pending.pending = true;
        QVERIFY(context.store()->appendMessage(peer, pending));
    }
    MainWindow window(context.api(), context.store(), context.codec(), auth, false);
    window.m_online = true;
    window.m_delivery->onConnected();
    window.m_delivery->onConnected();
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 3000);
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 2);
    QStringList paths;
    for (const QByteArray &request : server.requests)
        paths.append(QString::fromLatin1(request.left(request.indexOf(" HTTP/"))));
    QVERIFY(paths.at(0).contains(QStringLiteral("GET /api/v1/messages?")));
    QVERIFY(paths.at(1).contains(QStringLiteral("GET /api/v1/messages?")));
    QVERIFY(paths.at(0).contains(QStringLiteral("peer_id=peer-a"))
            || paths.at(1).contains(QStringLiteral("peer_id=peer-a")));
    QVERIFY(paths.at(0).contains(QStringLiteral("peer_id=peer-b"))
            || paths.at(1).contains(QStringLiteral("peer_id=peer-b")));
    window.shutdown();
    context.shutdown();
}

QTEST_MAIN(SessionTabsTest)
#include "test_session_tabs.moc"
