#include "LocalStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class LocalStoreTest final : public QObject {
    Q_OBJECT
private slots:
    void createsIsolatedLayout();
    void sanitizesUntrustedNames();
    void storesAndDeduplicatesHistory();
    void persistsKnownAccountsWithoutCredentials();
    void mergesCursorAndReadReceiptAcrossDuplicateHistory();
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

void LocalStoreTest::persistsKnownAccountsWithoutCredentials()
{
    QTemporaryDir root;
    LocalStore first(root.filePath(QStringLiteral("alice")));
    LocalStore second(root.filePath(QStringLiteral("bob")));
    QVERIFY(first.initialize());
    QVERIFY(second.initialize());

    User good;
    good.id = QStringLiteral("good-id");
    good.username = QStringLiteral("GOOD");
    good.displayName = QStringLiteral("Custom display name");
    QVERIFY(first.rememberAccount(good));
    QVERIFY(first.rememberAccount(good)); // Reauthentication is idempotent.
    QVERIFY(first.knownAccountIds().contains(good.id));
    QVERIFY(!second.knownAccountIds().contains(good.id));
    QVERIFY(second.rememberAccount(good));

    const QString firstPath = first.path(QStringLiteral("config/known_accounts.json"));
    const QString secondPath = second.path(QStringLiteral("config/known_accounts.json"));
    QVERIFY(firstPath != secondPath);
    QVERIFY(QFileInfo::exists(firstPath));
    QVERIFY(QFileInfo::exists(secondPath));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("known_accounts.json"))));
    QFile registry(firstPath);
    QVERIFY(registry.open(QIODevice::ReadOnly));
    const QByteArray json = registry.readAll();
    QVERIFY(json.contains("good-id"));
    QVERIFY(json.contains("GOOD"));
    QVERIFY(!json.contains("Custom display name"));
    QVERIFY(!json.contains("token"));
    QVERIFY(!json.contains("password"));
    registry.close();

    QVERIFY(first.forgetAccount(good.id));
    QVERIFY(first.knownAccountIds().isEmpty());
    QVERIFY(second.knownAccountIds().contains(good.id));
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

QTEST_GUILESS_MAIN(LocalStoreTest)
#include "test_local_store.moc"
