#include "WireCodec.h"

#include <QtTest>

class WireCodecTest final : public QObject {
    Q_OBJECT
private slots:
    void protobufRoundTrip();
    void rejectsGarbage();
    void readReceiptRoundTrip();
};

void WireCodecTest::protobufRoundTrip()
{
    auto codec = createDefaultWireCodec();
    QVERIFY(codec->available());
    WireMessage source;
    source.messageId = QStringLiteral("message-1");
    source.conversationId = QStringLiteral("u1:u2");
    source.senderId = QStringLiteral("u1");
    source.recipientId = QStringLiteral("u2");
    source.deviceId = QStringLiteral("desktop");
    source.timestamp = 123456789;
    source.payload = QByteArray("{\"type\":\"text\",\"text\":\"hello\"}");
    QByteArray frame;
    QString error;
    QVERIFY2(codec->encode(source, &frame, &error), qPrintable(error));
    QVERIFY(!frame.isEmpty());
    WireMessage decoded;
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.version, source.version);
    QCOMPARE(decoded.type, source.type);
    QCOMPARE(decoded.messageId, source.messageId);
    QCOMPARE(decoded.conversationId, source.conversationId);
    QCOMPARE(decoded.senderId, source.senderId);
    QCOMPARE(decoded.recipientId, source.recipientId);
    QCOMPARE(decoded.timestamp, source.timestamp);
    QCOMPARE(decoded.payload, source.payload);
}

void WireCodecTest::rejectsGarbage()
{
    auto codec = createDefaultWireCodec();
    WireMessage decoded;
    QString error;
    QVERIFY(!codec->decode(QByteArray("not a protobuf frame"), &decoded, &error));
    QVERIFY(!error.isEmpty());
}

void WireCodecTest::readReceiptRoundTrip()
{
    auto codec = createDefaultWireCodec();
    WireMessage source;
    source.type = WireMessage::Type::Read;
    source.senderId = QStringLiteral("reader");
    source.recipientId = QStringLiteral("author");
    source.cursor = QStringLiteral("42");
    source.readAt = 1700000000123LL;
    QByteArray frame;
    QString error;
    QVERIFY2(codec->encode(source, &frame, &error), qPrintable(error));
    WireMessage decoded;
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.type, WireMessage::Type::Read);
    QCOMPARE(decoded.cursor, QStringLiteral("42"));
    QCOMPARE(decoded.readAt, source.readAt);
}

QTEST_GUILESS_MAIN(WireCodecTest)
#include "test_wire_codec.moc"
