#include "LocalStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class LocalStoreTest final : public QObject {
    Q_OBJECT
private slots:
    void createsIsolatedLayout();
    void sanitizesUntrustedNames();
    void storesAndDeduplicatesHistory();
    void mergesCursorAndReadReceiptAcrossDuplicateHistory();
    void deliveryStateNeverRegressesAndLateAckRecovers();
    void readReceiptOnlyChangesReadAt();
    void outboxUsesSenderAndRestartRecovery();
    void reportsWriteFailure();
    void trimmingRetainsPendingMessages();
};

void LocalStoreTest::createsIsolatedLayout()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    LocalStore one(first.path());
    LocalStore two(second.path());
    QVERIFY(one.initialize());
    QVERIFY(two.initialize());
    const QStringList names{QStringLiteral("inbox"), QStringLiteral("outbox"),
        QStringLiteral("downloads"), QStringLiteral("temp"), QStringLiteral("keys"),
        QStringLiteral("logs"), QStringLiteral("config")};
    for (const QString &name : names) {
        QVERIFY(QFileInfo::exists(one.path(name)));
        QVERIFY(QFileInfo::exists(two.path(name)));
    }
    QVERIFY(one.root().absolutePath() != two.root().absolutePath());
}

void LocalStoreTest::sanitizesUntrustedNames()
{
    QCOMPARE(LocalStore::sanitizeFileName(QStringLiteral("../../secret.txt")), QStringLiteral("secret.txt"));
    QCOMPARE(LocalStore::sanitizeFileName(QStringLiteral("..\\..\\evil.exe")), QStringLiteral("evil.exe"));
    QCOMPARE(LocalStore::sanitizeFileName(QStringLiteral("CON")), QStringLiteral("_CON"));
    QCOMPARE(LocalStore::sanitizeFileName(QStringLiteral("..")), QStringLiteral("file"));
    QCOMPARE(LocalStore::sanitizeFileName(QStringLiteral("a:b*c?.txt")), QStringLiteral("abc.txt"));
}

void LocalStoreTest::storesAndDeduplicatesHistory()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ChatMessage message;
    message.id = QStringLiteral("m1");
    message.cursor = QStringLiteral("41");
    message.senderId = QStringLiteral("u1");
    message.recipientId = QStringLiteral("u2");
    message.content = QStringLiteral("hello");
    message.timestamp = 123;
    store.appendMessage(QStringLiteral("u2"), message);
    store.appendMessage(QStringLiteral("u2"), message);
    const QList<ChatMessage> messages = store.messages(QStringLiteral("u2"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().content, QStringLiteral("hello"));
    QVERIFY(store.messages(QStringLiteral("u2"), QStringLiteral("m1")).isEmpty());
    QVERIFY(store.messages(QStringLiteral("u2"), QStringLiteral("41")).isEmpty());
    QVERIFY(store.messages(QStringLiteral("other")).isEmpty());
}

void LocalStoreTest::mergesCursorAndReadReceiptAcrossDuplicateHistory()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ChatMessage optimistic;
    optimistic.id = QStringLiteral("own-1");
    optimistic.senderId = QStringLiteral("me");
    optimistic.recipientId = QStringLiteral("peer");
    optimistic.content = QStringLiteral("hello");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), optimistic));

    ChatMessage history = optimistic;
    history.cursor = QStringLiteral("41");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), history));
    QVERIFY(store.markMessagesRead(QStringLiteral("peer"), QStringLiteral("me"),
                                   QStringLiteral("41"), 1700000000123LL));
    QCOMPARE(store.messages(QStringLiteral("peer")).first().readAt, 1700000000123LL);

    // Replaying an older history projection must not clear either field.
    history.readAt = 0;
    QVERIFY(!store.appendMessage(QStringLiteral("peer"), history));
    LocalStore reloaded(dir.path());
    const QList<ChatMessage> messages = reloaded.messages(QStringLiteral("peer"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().cursor, QStringLiteral("41"));
    QCOMPARE(messages.first().readAt, 1700000000123LL);

    ChatMessage newer = history;
    newer.readAt = 1700000000999LL;
    QVERIFY(reloaded.appendMessage(QStringLiteral("peer"), newer));
    QCOMPARE(reloaded.messages(QStringLiteral("peer")).first().readAt, 1700000000999LL);
}

void LocalStoreTest::deliveryStateNeverRegressesAndLateAckRecovers()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ChatMessage message;
    message.id = QStringLiteral("file-1");
    message.senderId = QStringLiteral("me");
    message.recipientId = QStringLiteral("peer");
    message.kind = QStringLiteral("file");
    message.fileId = QStringLiteral("file-id");
    message.fileName = QStringLiteral("report.pdf");
    message.fileSize = 4096;
    message.timestamp = 1234;
    message.pending = true;
    message.attemptCount = 2;
    message.nextAttemptAt = 2000;
    QVERIFY(store.appendMessage(QStringLiteral("peer"), message));

    ChatMessage error = message;
    error.pending = false;
    error.failed = true;
    error.deliveryError = QStringLiteral("rejected");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), error));
    // A stale retry projection cannot resurrect a failed entry.
    QVERIFY(!store.appendMessage(QStringLiteral("peer"), message));
    QCOMPARE(store.messages(QStringLiteral("peer")).first().attemptCount, 2);
    QVERIFY(store.messages(QStringLiteral("peer")).first().failed);

    ChatMessage history;
    history.id = message.id;
    history.cursor = QStringLiteral("99");
    history.acked = true;
    QVERIFY(store.appendMessage(QStringLiteral("peer"), history));
    const ChatMessage recovered = store.messages(QStringLiteral("peer")).first();
    QVERIFY(recovered.acked && !recovered.pending && !recovered.failed);
    QCOMPARE(recovered.cursor, QStringLiteral("99"));
    QCOMPARE(recovered.fileId, QStringLiteral("file-id"));
    QCOMPARE(recovered.fileName, QStringLiteral("report.pdf"));
    QCOMPARE(recovered.fileSize, qint64(4096));
    QCOMPARE(recovered.timestamp, qint64(1234));
    QCOMPARE(recovered.deliveryError, QString{});
}

void LocalStoreTest::readReceiptOnlyChangesReadAt()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ChatMessage message;
    message.id = QStringLiteral("m1");
    message.cursor = QStringLiteral("7");
    message.senderId = QStringLiteral("me");
    message.recipientId = QStringLiteral("peer");
    message.pending = true;
    message.attemptCount = 1;
    message.nextAttemptAt = 100;
    QVERIFY(store.appendMessage(QStringLiteral("peer"), message));
    QVERIFY(store.markMessagesRead(QStringLiteral("peer"), QStringLiteral("me"),
                                   QStringLiteral("7"), 500));
    const ChatMessage updated = store.messages(QStringLiteral("peer")).first();
    QCOMPARE(updated.readAt, qint64(500));
    QVERIFY(updated.pending && !updated.acked && !updated.failed);
    QCOMPARE(updated.attemptCount, 1);
    QCOMPARE(updated.nextAttemptAt, qint64(100));
}

void LocalStoreTest::outboxUsesSenderAndRestartRecovery()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ChatMessage legacy;
    legacy.id = QStringLiteral("legacy");
    legacy.cursor = QStringLiteral("8");
    legacy.senderId = QStringLiteral("me");
    legacy.recipientId = QStringLiteral("peer");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), legacy));
    ChatMessage failed = legacy;
    failed.id = QStringLiteral("failed");
    failed.cursor.clear();
    failed.failed = true;
    failed.attemptCount = 3;
    QVERIFY(store.appendMessage(QStringLiteral("peer"), failed));
    ChatMessage other = failed;
    other.id = QStringLiteral("other");
    other.senderId = QStringLiteral("other-user");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), other));
    QCOMPARE(store.outbox(QStringLiteral("me")).size(), 2);

    QVERIFY(store.recoverOutbox(QStringLiteral("me"), 1000));
    const QList<ChatMessage> outbox = store.outbox(QStringLiteral("me"));
    QCOMPARE(outbox.size(), 1);
    QCOMPARE(outbox.first().id, QStringLiteral("failed"));
    QVERIFY(outbox.first().pending && !outbox.first().failed);
    QCOMPARE(outbox.first().attemptCount, 0);
    QCOMPARE(outbox.first().nextAttemptAt, qint64(1000));
    QVERIFY(store.findMessage(QStringLiteral("legacy"), QStringLiteral("me"), &legacy));
    QVERIFY(legacy.acked);
    QVERIFY(store.findMessage(QStringLiteral("other"), QStringLiteral("other-user"), &other));
    QVERIFY(other.failed);
}

void LocalStoreTest::reportsWriteFailure()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    QVERIFY(QDir().mkdir(store.path(QStringLiteral("config/messages.json"))));
    ChatMessage message;
    message.id = QStringLiteral("m1");
    message.senderId = QStringLiteral("me");
    message.recipientId = QStringLiteral("peer");
    message.pending = true;
    QString error;
    QVERIFY(!store.appendMessage(QStringLiteral("peer"), message, &error));
    QVERIFY(!error.isEmpty());
}

void LocalStoreTest::trimmingRetainsPendingMessages()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    QJsonArray rows;
    for (int index = 0; index < 1999; ++index) {
        rows.append(QJsonObject{{QStringLiteral("message_id"), QString::number(index)},
                                {QStringLiteral("sender_id"), QStringLiteral("me")},
                                {QStringLiteral("acked"), true}});
    }
    rows.append(QJsonObject{{QStringLiteral("message_id"), QStringLiteral("pending")},
                            {QStringLiteral("sender_id"), QStringLiteral("me")},
                            {QStringLiteral("recipient_id"), QStringLiteral("peer")},
                            {QStringLiteral("pending"), true}});
    QFile file(store.path(QStringLiteral("config/messages.json")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(QJsonObject{{QStringLiteral("peer"), rows}})
                           .toJson(QJsonDocument::Compact)) > 0);
    file.close();
    ChatMessage next;
    next.id = QStringLiteral("next");
    next.senderId = QStringLiteral("peer");
    next.content = QStringLiteral("new");
    QVERIFY(store.appendMessage(QStringLiteral("peer"), next));
    const QList<ChatMessage> messages = store.messages(QStringLiteral("peer"));
    QCOMPARE(messages.size(), 2000);
    ChatMessage pending;
    QVERIFY(store.findMessage(QStringLiteral("pending"), QStringLiteral("me"), &pending));
    QVERIFY(pending.pending);
}

QTEST_GUILESS_MAIN(LocalStoreTest)
#include "test_local_store.moc"
