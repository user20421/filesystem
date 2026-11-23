#include "ApiClient.h"
#include "MessagePayloadCodec.h"
#include "LocalStore.h"
#include "ProtobufWireCodec.h"
#include "WebSocketClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>

namespace {
AuthSession registerAccount(ApiClient &api, const QString &username, const QString &password,
                            bool *ok, QString *error)
{
    AuthSession session;
    QEventLoop loop;
    bool done = false;
    *ok = false;
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    api.registerUser(username, password, [&](bool success, const AuthSession &result,
                                             const QString &apiError) {
        *ok = success;
        session = result;
        *error = apiError;
        done = true;
        loop.quit();
    });
    loop.exec();
    if (!done)
        *error = QStringLiteral("registration timeout");
    return session;
}
bool openRealtime(ApiClient &api, WebSocketClient &socket, const QString &ticket,
                  QString *error)
{
    QEventLoop loop;
    bool connected = false;
    bool done = false;
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &WebSocketClient::connected, &loop, [&] {
        connected = true;
        done = true;
        loop.quit();
    });
    QObject::connect(&socket, &WebSocketClient::errorOccurred, &loop, [&](const QString &message) {
        *error = message;
        done = true;
        loop.quit();
    });
    if (!ticket.isEmpty()) {
        socket.open(api.webSocketUrl(ticket));
    } else {
        api.wsTicket([&](bool ok, const QString &result, const QString &apiError) {
            if (!ok) {
                *error = apiError;
                done = true;
                loop.quit();
                return;
            }
            socket.open(api.webSocketUrl(result));
        });
    }
    loop.exec();
    if (!done)
        *error = QStringLiteral("WebSocket connection timeout");
    return connected;
}
} // namespace

class BackendIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void realBackendFlow();
};

// 依赖 CLIENT_BACKEND_URL 的真实后端，随机创建两个账号并覆盖认证、实时消息、重复帧幂等、已读、文件和注销清理。
void BackendIntegrationTest::realBackendFlow()
{
    const QByteArray server = qgetenv("CLIENT_BACKEND_URL");
    if (server.isEmpty())
        QSKIP("Set CLIENT_BACKEND_URL to run the real backend integration test");

    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(16);
    const QString username = (QStringLiteral("qtit_") + suffix).toUpper();
    const QString recipientName = (QStringLiteral("qtit_peer_") + suffix).toUpper();
    const QString password = QStringLiteral("123456");
    ApiClient api;
    api.setServerUrl(QUrl(QString::fromUtf8(server)));

    bool registrationOk = false;
    QString registrationError;
    const AuthSession registered = registerAccount(api, username, password,
                                                   &registrationOk, &registrationError);
    QVERIFY2(registrationOk, qPrintable(registrationError));
    const AuthSession recipient = registerAccount(api, recipientName, password,
                                                  &registrationOk, &registrationError);
    QVERIFY2(registrationOk, qPrintable(registrationError));
    QVERIFY(!registered.user.id.isEmpty());
    QVERIFY(!recipient.user.id.isEmpty());

    AuthSession loggedIn;
    QString loginError;
    bool loginOk = false;
    bool loginDone = false;
    api.login(username, password, [&](bool ok, const AuthSession &session, const QString &error) {
        loginOk = ok;
        loggedIn = session;
        loginError = error;
        loginDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(loginDone, 10000);
    QVERIFY2(loginOk, qPrintable(loginError));
    QCOMPARE(loggedIn.user.id, registered.user.id);
    api.setAccessToken(loggedIn.accessToken);

    QList<User> users;
    QString usersError;
    bool usersOk = false;
    bool usersDone = false;
    api.users([&](bool ok, const QList<User> &result, const QString &error) {
        usersOk = ok;
        users = result;
        usersError = error;
        usersDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(usersDone, 10000);
    QVERIFY2(usersOk, qPrintable(usersError));
    bool foundSelf = false;
    bool foundRecipient = false;
    for (const User &user : users) {
        foundSelf = foundSelf || user.id == loggedIn.user.id;
        foundRecipient = foundRecipient || user.id == recipient.user.id;
    }
    QVERIFY(foundSelf);
    QVERIFY(foundRecipient);

    QString ticket;
    QString ticketError;
    bool ticketOk = false;
    bool ticketDone = false;
    api.wsTicket([&](bool ok, const QString &result, const QString &error) {
        ticketOk = ok;
        ticket = result;
        ticketError = error;
        ticketDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(ticketDone, 10000);
    QVERIFY2(ticketOk, qPrintable(ticketError));
    QVERIFY(!ticket.isEmpty());
    QCOMPARE(api.webSocketUrl(ticket).scheme(), QStringLiteral("ws"));

    ProtobufWireCodec senderCodec;
    ProtobufWireCodec recipientCodec;
    WebSocketClient senderSocket(&senderCodec);
    WebSocketClient recipientSocket(&recipientCodec);
    QString socketError;
    QVERIFY2(openRealtime(api, senderSocket, ticket, &socketError), qPrintable(socketError));
    ApiClient recipientApi;
    recipientApi.setServerUrl(QUrl(QString::fromUtf8(server)));
    recipientApi.setAccessToken(recipient.accessToken);
    QVERIFY2(openRealtime(recipientApi, recipientSocket, {}, &socketError), qPrintable(socketError));

    bool presenceDone = false;
    bool presenceOk = false;
    QSet<QString> onlineUserIds;
    QString presenceError;
    api.presence([&](bool ok, const QSet<QString> &ids, const QString &error) {
        presenceOk = ok;
        onlineUserIds = ids;
        presenceError = error;
        presenceDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(presenceDone, 10000);
    QVERIFY2(presenceOk, qPrintable(presenceError));
    QVERIFY(onlineUserIds.contains(loggedIn.user.id));
    QVERIFY(onlineUserIds.contains(recipient.user.id));

    const QString realtimeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString realtimeText = QStringLiteral("realtime-") + suffix;
    bool ackReceived = false;
    QStringList ackCursors;
    bool messageReceived = false;
    bool readReceived = false;
    bool recipientReadReceived = false;
    WireMessage receivedMessage;
    WireMessage readReceipt;
    WireMessage recipientReadReceipt;
    QObject::connect(&senderSocket, &WebSocketClient::messageReceived, &senderSocket,
                     [&](const WireMessage &message) {
        if (message.type == WireMessage::Type::Ack && message.messageId == realtimeId) {
            ackReceived = true;
            ackCursors.append(message.cursor);
        }
        if (message.type == WireMessage::Type::Read) {
            readReceipt = message;
            readReceived = true;
        }
    });
    QObject::connect(&recipientSocket, &WebSocketClient::messageReceived, &recipientSocket,
                     [&](const WireMessage &message) {
        if (message.type == WireMessage::Type::Message && message.messageId == realtimeId) {
            receivedMessage = message;
            messageReceived = true;
        }
        if (message.type == WireMessage::Type::Read) {
            recipientReadReceipt = message;
            recipientReadReceived = true;
        }
    });
    QStringList participants{loggedIn.user.id, recipient.user.id};
    participants.sort();
    WireMessage outgoing;
    outgoing.messageId = realtimeId;
    outgoing.conversationId = participants.join(QLatin1Char(':'));
    outgoing.senderId = loggedIn.user.id;
    outgoing.recipientId = recipient.user.id;
    outgoing.deviceId = QStringLiteral("qt-integration");
    outgoing.timestamp = QDateTime::currentMSecsSinceEpoch();
    ChatMessage text;
    text.content = realtimeText;
    QString sendError;
    QVERIFY2(MessagePayloadCodec::encode(text, &outgoing.payload, &sendError), qPrintable(sendError));
    QVERIFY2(senderSocket.send(outgoing, &sendError), qPrintable(sendError));
    QTRY_VERIFY_WITH_TIMEOUT(ackReceived && messageReceived, 15000);
    QVERIFY(ackReceived);
    QVERIFY(messageReceived);
    QCOMPARE(receivedMessage.senderId, loggedIn.user.id);
    QCOMPARE(receivedMessage.recipientId, recipient.user.id);
    ChatMessage receivedPayload;
    QVERIFY2(MessagePayloadCodec::decode(receivedMessage.payload, &receivedPayload, &sendError),
             qPrintable(sendError));
    QCOMPARE(receivedPayload.content, realtimeText);

    // Repeating the exact Protobuf frame exercises idempotent server storage:
    // the second ACK must identify the original row, not create a new cursor.
    QVERIFY2(senderSocket.send(outgoing, &sendError), qPrintable(sendError));
    QTRY_COMPARE_WITH_TIMEOUT(ackCursors.size(), 2, 15000);
    QCOMPARE(ackCursors.at(1), ackCursors.at(0));
    QVERIFY(!ackCursors.at(0).isEmpty());

    QList<ChatMessage> realtimeHistory;
    QString historyError;
    bool historyOk = false;
    bool historyDone = false;
    recipientApi.messages(loggedIn.user.id, {},
                          [&](bool ok, const QList<ChatMessage> &messages, const QString &error) {
        historyOk = ok;
        realtimeHistory = messages;
        historyError = error;
        historyDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(historyDone, 10000);
    QVERIFY2(historyOk, qPrintable(historyError));
    ChatMessage storedRealtime;
    for (const ChatMessage &message : realtimeHistory) {
        if (message.id == realtimeId)
            storedRealtime = message;
    }
    QCOMPARE(storedRealtime.id, realtimeId);
    QCOMPARE(storedRealtime.content, realtimeText);
    QVERIFY(!storedRealtime.cursor.isEmpty());

    bool emptyDeltaDone = false;
    recipientApi.messages(loggedIn.user.id, storedRealtime.cursor,
                          [&](bool ok, const QList<ChatMessage> &messages, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        for (const ChatMessage &message : messages)
            QVERIFY(message.id != realtimeId);
        emptyDeltaDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(emptyDeltaDone, 10000);

    bool markReadDone = false;
    bool markReadOk = false;
    QString markReadError;
    recipientApi.markRead(loggedIn.user.id, storedRealtime.cursor,
                          [&](bool ok, const QString &error) {
        markReadOk = ok;
        markReadError = error;
        markReadDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(markReadDone && readReceived && recipientReadReceived, 10000);
    QVERIFY2(markReadOk, qPrintable(markReadError));
    QCOMPARE(readReceipt.cursor, storedRealtime.cursor);
    QCOMPARE(recipientReadReceipt.cursor, storedRealtime.cursor);
    QVERIFY(readReceipt.readAt > 0);

    QList<ChatMessage> readHistory;
    historyDone = false;
    api.messages(recipient.user.id, {}, [&](bool ok, const QList<ChatMessage> &messages,
                                          const QString &error) {
        historyOk = ok;
        readHistory = messages;
        historyError = error;
        historyDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(historyDone, 10000);
    QVERIFY2(historyOk, qPrintable(historyError));
    ChatMessage persistedRead;
    for (const ChatMessage &message : readHistory) {
        if (message.id == realtimeId)
            persistedRead = message;
    }
    QCOMPARE(persistedRead.id, realtimeId);
    QVERIFY(persistedRead.readAt > 0);
    QTemporaryDir localDir;
    QVERIFY(localDir.isValid());
    LocalStore local(localDir.path());
    QVERIFY(local.initialize());
    local.appendMessage(recipient.user.id, persistedRead);
    local.appendMessage(recipient.user.id, storedRealtime); // stale, unread duplicate
    QCOMPARE(local.messages(recipient.user.id).first().readAt, persistedRead.readAt);

    const QByteArray contents("hello");
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    FileInitInfo file;
    QString initError;
    bool initOk = false;
    bool initDone = false;
    api.initFile(recipient.user.id, QStringLiteral("qt-integration-") + suffix + QStringLiteral(".txt"),
                 contents.size(), digest, [&](bool ok, const FileInitInfo &result, const QString &error) {
        initOk = ok;
        file = result;
        initError = error;
        initDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(initDone, 10000);
    QVERIFY2(initOk, qPrintable(initError));
    QVERIFY(!file.fileId.isEmpty());
    QCOMPARE(file.chunkSize, 5 * 1024 * 1024);

    FileStatus initialStatus;
    QString statusError;
    bool statusOk = false;
    bool statusDone = false;
    api.fileStatus(file.fileId, [&](bool ok, const FileStatus &result, const QString &error) {
        statusOk = ok;
        initialStatus = result;
        statusError = error;
        statusDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(statusDone, 10000);
    QVERIFY2(statusOk, qPrintable(statusError));
    QCOMPARE(initialStatus.fileId, file.fileId);
    QCOMPARE(initialStatus.size, qint64(contents.size()));

    bool chunkOk = false;
    bool chunkDone = false;
    QString chunkError;
    api.putChunk(file.fileId, 0, contents, digest, [&](bool ok, const QString &error) {
        chunkOk = ok;
        chunkError = error;
        chunkDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(chunkDone, 10000);
    QVERIFY2(chunkOk, qPrintable(chunkError));

    bool completeOk = false;
    bool completeDone = false;
    QString completeError;
    api.completeFile(file.fileId, [&](bool ok, const QString &error) {
        completeOk = ok;
        completeError = error;
        completeDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completeDone, 10000);
    QVERIFY2(completeOk, qPrintable(completeError));

    FileStatus completedStatus;
    statusDone = false;
    api.fileStatus(file.fileId, [&](bool ok, const FileStatus &result, const QString &error) {
        statusOk = ok;
        completedStatus = result;
        statusError = error;
        statusDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(statusDone, 10000);
    QVERIFY2(statusOk, qPrintable(statusError));
    qInfo().noquote() << "Backend completed file status:" << completedStatus.status;
    QCOMPARE(completedStatus.status, QStringLiteral("completed"));
    QVERIFY(completedStatus.complete);

    QByteArray downloaded;
    bool downloadOk = false;
    bool downloadDone = false;
    QString downloadError;
    ApiRequestHandle *downloadRequest = api.downloadFile(file.fileId, [&](bool ok, const QString &error) {
        downloadOk = ok;
        if (ok)
            downloaded.append(downloadRequest->readAll());
        downloadError = error;
        downloadDone = true;
    });
    QObject::connect(downloadRequest, &ApiRequestHandle::readyRead, &api, [&] {
        downloaded.append(downloadRequest->readAll());
    });
    QTRY_VERIFY_WITH_TIMEOUT(downloadDone, 10000);
    QVERIFY2(downloadOk, qPrintable(downloadError));
    QCOMPARE(downloaded, contents);

    // Best-effort cleanup; the assertions above intentionally do not depend on
    // whether the backend permits deletion of an already completed file.
    bool deleteDone = false;
    api.deleteFile(file.fileId, [&](bool, const QString &) { deleteDone = true; });
    QTRY_VERIFY_WITH_TIMEOUT(deleteDone, 10000);
    senderSocket.shutdown();
    recipientSocket.shutdown();

    bool deleteAccountDone = false;
    bool deleteAccountOk = false;
    QString deleteAccountError;
    api.deleteCurrentAccount([&](bool ok, const QString &error) {
        deleteAccountOk = ok;
        deleteAccountError = error;
        deleteAccountDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(deleteAccountDone, 10000);
    QVERIFY2(deleteAccountOk, qPrintable(deleteAccountError));

    deleteAccountDone = false;
    recipientApi.deleteCurrentAccount([&](bool ok, const QString &error) {
        deleteAccountOk = ok;
        deleteAccountError = error;
        deleteAccountDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(deleteAccountDone, 10000);
    QVERIFY2(deleteAccountOk, qPrintable(deleteAccountError));
}

QTEST_GUILESS_MAIN(BackendIntegrationTest)
#include "test_backend_integration.moc"
