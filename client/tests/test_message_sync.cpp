#include "MessageSyncState.h"

#include <QtTest>

class MessageSyncTest final : public QObject {
    Q_OBJECT
private slots:
    void duplicateOptimisticMessageStillAdvancesCursor();
    void realtimeMessageDoesNotReplaceServerCursor();
    void cursorsAreIndependentPerPeer();
    void staleHistoryDoesNotRegressCursor();
};

void MessageSyncTest::duplicateOptimisticMessageStillAdvancesCursor()
{
    MessageSyncState state;
    ChatMessage optimistic;
    optimistic.id = QStringLiteral("m1");
    optimistic.content = QStringLiteral("hello");
    QVERIFY(state.accept(QStringLiteral("peer"), optimistic));
    QVERIFY(state.cursor(QStringLiteral("peer")).isEmpty());

    ChatMessage history = optimistic;
    history.cursor = QStringLiteral("41");
    QVERIFY(!state.accept(QStringLiteral("peer"), history));
    QCOMPARE(state.cursor(QStringLiteral("peer")), QStringLiteral("41"));

    ChatMessage next = optimistic;
    next.id = QStringLiteral("m2");
    next.cursor = QStringLiteral("42");
    QVERIFY(state.accept(QStringLiteral("peer"), next));
    QCOMPARE(state.cursor(QStringLiteral("peer")), QStringLiteral("42"));
}

void MessageSyncTest::realtimeMessageDoesNotReplaceServerCursor()
{
    MessageSyncState state;
    ChatMessage history;
    history.id = QStringLiteral("m1");
    history.cursor = QStringLiteral("100");
    QVERIFY(state.accept(QStringLiteral("peer"), history));

    ChatMessage realtime;
    realtime.id = QStringLiteral("m2");
    QVERIFY(state.accept(QStringLiteral("peer"), realtime));
    QCOMPARE(state.cursor(QStringLiteral("peer")), QStringLiteral("100"));
}

void MessageSyncTest::cursorsAreIndependentPerPeer()
{
    MessageSyncState state;
    ChatMessage message;
    message.id = QStringLiteral("m1");
    message.cursor = QStringLiteral("7");
    QVERIFY(state.accept(QStringLiteral("alice"), message));
    QVERIFY(state.cursor(QStringLiteral("bob")).isEmpty());
    QCOMPARE(state.cursor(QStringLiteral("alice")), QStringLiteral("7"));
}

void MessageSyncTest::staleHistoryDoesNotRegressCursor()
{
    MessageSyncState state;
    ChatMessage message;
    message.id = QStringLiteral("m10");
    message.cursor = QStringLiteral("10");
    QVERIFY(state.accept(QStringLiteral("peer"), message));
    message.id = QStringLiteral("m9");
    message.cursor = QStringLiteral("9");
    QVERIFY(state.accept(QStringLiteral("peer"), message));
    QCOMPARE(state.cursor(QStringLiteral("peer")), QStringLiteral("10"));
}

QTEST_GUILESS_MAIN(MessageSyncTest)
#include "test_message_sync.moc"
