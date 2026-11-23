#include "MessageDelivery.h"
#include "MessagePayloadCodec.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

namespace {
struct FakeTransport {
    bool connected = false;
    bool accept = true;
    QString error;
    QList<WireMessage> writes;
};

std::unique_ptr<MessageDelivery> makeDelivery(LocalStore *store, const QString &sender,
                                               FakeTransport &transport, int timeout = 25)
{
    return std::make_unique<MessageDelivery>(store, sender,
        [&transport](const WireMessage &wire, QString *error) {
            if (!transport.connected || !transport.accept) {
                if (error)
                    *error = transport.error.isEmpty() ? QStringLiteral("offline") : transport.error;
                return false;
            }
            transport.writes.append(wire);
            return true;
        },
        [&transport] { return transport.connected; }, nullptr, timeout);
}

ChatMessage textMessage(const QString &text)
{
    ChatMessage message;
    message.content = text;
    return message;
}
} // namespace

class MessageDeliveryTest final : public QObject {
    Q_OBJECT
private slots:
    void offlineIntentSurvivesRestartWithIdenticalWire();
    void retriesSameFrameAtMostThreeTimesAndAcceptsLateAck();
    void errorIsTerminalUntilLateAck();
    void historyReconcilesFailedFileNotification();
    void writeFailurePreventsSocketWrite();
    void disconnectAndShutdownDoNotConsumeAttempts();
};

void MessageDeliveryTest::offlineIntentSurvivesRestartWithIdenticalWire()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    FakeTransport firstTransport;
    auto first = makeDelivery(&store, QStringLiteral("me"), firstTransport);
    ChatMessage file;
    file.kind = QStringLiteral("file");
    file.fileId = QStringLiteral("file-id");
    file.fileName = QStringLiteral("report.pdf");
    file.fileSize = 8192;
    ChatMessage pending;
    QVERIFY(first->enqueue(QStringLiteral("peer"), file, &pending));
    QVERIFY(firstTransport.writes.isEmpty());
    ChatMessage stored;
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.pending && !stored.acked && stored.attemptCount == 0);
    QCOMPARE(stored.fileName, QStringLiteral("report.pdf"));
    QCOMPARE(stored.fileSize, qint64(8192));
    first->shutdown();
    first.reset();

    FakeTransport secondTransport;
    secondTransport.connected = true;
    auto second = makeDelivery(&store, QStringLiteral("me"), secondTransport);
    second->recover();
    second->onConnected();
    QCOMPARE(secondTransport.writes.size(), 1);
    const WireMessage wire = secondTransport.writes.first();
    QCOMPARE(wire.messageId, pending.id);
    QCOMPARE(wire.timestamp, pending.timestamp);
    ChatMessage decoded;
    QString error;
    QVERIFY2(MessagePayloadCodec::decode(wire.payload, &decoded, &error), qPrintable(error));
    QVERIFY(decoded.isFile());
    QCOMPARE(decoded.fileId, QStringLiteral("file-id"));
    QCOMPARE(decoded.fileName, QStringLiteral("report.pdf"));
    QCOMPARE(decoded.fileSize, qint64(8192));
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.pending && !stored.acked);
    QCOMPARE(stored.attemptCount, 1);
    second->shutdown();
}

void MessageDeliveryTest::retriesSameFrameAtMostThreeTimesAndAcceptsLateAck()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    FakeTransport transport;
    transport.connected = true;
    auto delivery = makeDelivery(&store, QStringLiteral("me"), transport);
    ChatMessage pending;
    QVERIFY(delivery->enqueue(QStringLiteral("peer"), textMessage(QStringLiteral("hello")), &pending));
    QTRY_COMPARE_WITH_TIMEOUT(transport.writes.size(), 3, 1000);
    ChatMessage stored;
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QTRY_VERIFY_WITH_TIMEOUT(store.findMessage(pending.id, QStringLiteral("me"), &stored)
                             && stored.failed, 1000);
    QCOMPARE(transport.writes.size(), 3);
    QCOMPARE(stored.attemptCount, 3);
    QVERIFY(!stored.pending && !stored.acked);
    for (const WireMessage &wire : transport.writes) {
        QCOMPARE(wire.messageId, transport.writes.first().messageId);
        QCOMPARE(wire.timestamp, transport.writes.first().timestamp);
        QCOMPARE(wire.payload, transport.writes.first().payload);
    }

    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = pending.id;
    ack.cursor = QStringLiteral("42");
    delivery->handleAck(ack);
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.acked && !stored.failed && !stored.pending);
    QCOMPARE(stored.cursor, QStringLiteral("42"));
    QCOMPARE(stored.readAt, qint64(0));
    QCOMPARE(stored.attemptCount, 3);
    QTest::qWait(80);
    QCOMPARE(transport.writes.size(), 3);
    delivery->shutdown();
}

void MessageDeliveryTest::errorIsTerminalUntilLateAck()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    FakeTransport transport;
    transport.connected = true;
    auto delivery = makeDelivery(&store, QStringLiteral("me"), transport);
    ChatMessage pending;
    QVERIFY(delivery->enqueue(QStringLiteral("peer"), textMessage(QStringLiteral("hello")), &pending));
    WireMessage error;
    error.type = WireMessage::Type::Error;
    error.messageId = pending.id;
    error.errorCode = QStringLiteral("message_rejected");
    error.errorMessage = QStringLiteral("invalid payload");
    delivery->handleError(error);
    ChatMessage stored;
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.failed && !stored.pending);
    QVERIFY(stored.deliveryError.contains(QStringLiteral("message_rejected")));
    QTest::qWait(80);
    QCOMPARE(transport.writes.size(), 1);

    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = pending.id;
    delivery->handleAck(ack);
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.acked && !stored.failed);
    QCOMPARE(stored.deliveryError, QString{});
    delivery->shutdown();
}

void MessageDeliveryTest::historyReconcilesFailedFileNotification()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    FakeTransport transport;
    transport.connected = true;
    auto delivery = makeDelivery(&store, QStringLiteral("me"), transport);
    ChatMessage file;
    file.kind = QStringLiteral("file");
    file.fileId = QStringLiteral("file-id");
    file.fileName = QStringLiteral("a.bin");
    file.fileSize = 10;
    ChatMessage pending;
    QVERIFY(delivery->enqueue(QStringLiteral("peer"), file, &pending));
    WireMessage error;
    error.messageId = pending.id;
    error.errorMessage = QStringLiteral("lost ack");
    delivery->handleError(error);

    ChatMessage history = pending;
    history.cursor = QStringLiteral("55");
    history = delivery->historyProjection(history);
    QVERIFY(history.acked && !history.failed && !history.pending);
    QVERIFY(store.appendMessage(QStringLiteral("peer"), history));
    QVERIFY(store.outbox(QStringLiteral("me")).isEmpty());
    const ChatMessage stored = store.messages(QStringLiteral("peer")).first();
    QCOMPARE(stored.cursor, QStringLiteral("55"));
    QCOMPARE(stored.fileId, QStringLiteral("file-id"));
    QCOMPARE(stored.fileName, QStringLiteral("a.bin"));
    QCOMPARE(stored.fileSize, qint64(10));
    delivery->shutdown();
}

void MessageDeliveryTest::writeFailurePreventsSocketWrite()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    QVERIFY(QDir().mkdir(store.path(QStringLiteral("config/messages.json"))));
    FakeTransport transport;
    transport.connected = true;
    auto delivery = makeDelivery(&store, QStringLiteral("me"), transport);
    QString error;
    QVERIFY(!delivery->enqueue(QStringLiteral("peer"), textMessage(QStringLiteral("hello")),
                               nullptr, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(transport.writes.isEmpty());
    delivery->shutdown();
}

void MessageDeliveryTest::disconnectAndShutdownDoNotConsumeAttempts()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    FakeTransport transport;
    transport.connected = true;
    auto delivery = makeDelivery(&store, QStringLiteral("me"), transport);
    ChatMessage pending;
    QVERIFY(delivery->enqueue(QStringLiteral("peer"), textMessage(QStringLiteral("hello")), &pending));
    QCOMPARE(transport.writes.size(), 1);
    transport.connected = false;
    delivery->onDisconnected();
    QTest::qWait(100);
    QCOMPARE(transport.writes.size(), 1);
    ChatMessage stored;
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QCOMPARE(stored.attemptCount, 1);
    QVERIFY(stored.pending && !stored.failed);

    delivery->shutdown();
    transport.connected = true;
    QTest::qWait(100);
    QCOMPARE(transport.writes.size(), 1);
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QVERIFY(stored.pending && !stored.failed);
    QCOMPARE(stored.attemptCount, 1);

    auto restarted = makeDelivery(&store, QStringLiteral("me"), transport);
    restarted->recover();
    restarted->onConnected();
    QCOMPARE(transport.writes.size(), 2);
    QCOMPARE(transport.writes.at(1).messageId, pending.id);
    QCOMPARE(transport.writes.at(1).timestamp, pending.timestamp);
    QCOMPARE(transport.writes.at(1).payload, transport.writes.at(0).payload);
    QVERIFY(store.findMessage(pending.id, QStringLiteral("me"), &stored));
    QCOMPARE(stored.attemptCount, 2);
    restarted->shutdown();
}

QTEST_GUILESS_MAIN(MessageDeliveryTest)
#include "test_message_delivery.moc"
