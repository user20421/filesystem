#include "MessagePayloadCodec.h"
#include "WireCodec.h"

#include <QtTest>

class WireCodecTest final : public QObject {
    Q_OBJECT
private slots:
    void protobufRoundTrip();
    void messagePayloadRoundTrip();
    void ackAndErrorPayloadRoundTrip();
    void rejectsGarbage();
    void rejectsLegacyMessagePayload();
    void readReceiptRoundTrip();
    void controlPayloadsAreEmpty();
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
    ChatMessage text;
    text.content = QStringLiteral("hello");
    QString error;
    QVERIFY2(MessagePayloadCodec::encode(text, &source.payload, &error), qPrintable(error));
    QByteArray frame;
    QVERIFY2(codec->encode(source, &frame, &error), qPrintable(error));
    QVERIFY(!frame.isEmpty());
    WireMessage decoded;
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.version, quint32(2));
    QCOMPARE(decoded.type, source.type);
    QCOMPARE(decoded.messageId, source.messageId);
    QCOMPARE(decoded.conversationId, source.conversationId);
    QCOMPARE(decoded.senderId, source.senderId);
    QCOMPARE(decoded.recipientId, source.recipientId);
    QCOMPARE(decoded.timestamp, source.timestamp);
    QCOMPARE(decoded.payload, source.payload);
}

void WireCodecTest::messagePayloadRoundTrip()
{
    ChatMessage source;
    source.kind = QStringLiteral("file");
    source.fileId = QStringLiteral("file-1");
    source.fileName = QStringLiteral("report.pdf");
    source.fileSize = 12345;
    QByteArray payload;
    QString error;
    QVERIFY2(MessagePayloadCodec::encode(source, &payload, &error), qPrintable(error));
    ChatMessage decoded;
    QVERIFY2(MessagePayloadCodec::decode(payload, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.kind, QStringLiteral("file"));
    QCOMPARE(decoded.fileId, source.fileId);
    QCOMPARE(decoded.fileName, source.fileName);
    QCOMPARE(decoded.fileSize, source.fileSize);
    QVERIFY(decoded.isFile());
}

void WireCodecTest::ackAndErrorPayloadRoundTrip()
{
    auto codec = createDefaultWireCodec();
    WireMessage ack;
    ack.type = WireMessage::Type::Ack;
    ack.messageId = QStringLiteral("message-1");
    ack.cursor = QStringLiteral("42");
    QByteArray frame;
    QString error;
    QVERIFY2(codec->encode(ack, &frame, &error), qPrintable(error));
    WireMessage decoded;
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.cursor, QStringLiteral("42"));
    QVERIFY(!decoded.payload.isEmpty());

    WireMessage serverError;
    serverError.type = WireMessage::Type::Error;
    serverError.errorCode = QStringLiteral("invalid_payload");
    serverError.errorMessage = QStringLiteral("message rejected");
    QVERIFY2(codec->encode(serverError, &frame, &error), qPrintable(error));
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.errorCode, serverError.errorCode);
    QCOMPARE(decoded.errorMessage, serverError.errorMessage);
}

void WireCodecTest::rejectsGarbage()
{
    auto codec = createDefaultWireCodec();
    WireMessage decoded;
    QString error;
    QVERIFY(!codec->decode(QByteArray("not a protobuf frame"), &decoded, &error));
    QVERIFY(!error.isEmpty());
}

void WireCodecTest::rejectsLegacyMessagePayload()
{
    ChatMessage decoded;
    QString error;
    QVERIFY(!MessagePayloadCodec::decode(QByteArray("{\"type\":\"text\",\"text\":\"hello\"}"),
                                         &decoded, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!MessagePayloadCodec::decode(QByteArray{}, &decoded, &error));
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
    source.payload = QByteArray("must not be sent");
    QByteArray frame;
    QString error;
    QVERIFY2(codec->encode(source, &frame, &error), qPrintable(error));
    WireMessage decoded;
    QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.type, WireMessage::Type::Read);
    QCOMPARE(decoded.cursor, QStringLiteral("42"));
    QCOMPARE(decoded.readAt, source.readAt);
    QVERIFY(decoded.payload.isEmpty());
}

void WireCodecTest::controlPayloadsAreEmpty()
{
    auto codec = createDefaultWireCodec();
    for (const WireMessage::Type type : {WireMessage::Type::Ping, WireMessage::Type::Pong}) {
        WireMessage source;
        source.type = type;
        source.payload = QByteArray("must not be sent");
        QByteArray frame;
        QString error;
        QVERIFY2(codec->encode(source, &frame, &error), qPrintable(error));
        WireMessage decoded;
        QVERIFY2(codec->decode(frame, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.type, type);
        QVERIFY(decoded.payload.isEmpty());
    }
}

QTEST_GUILESS_MAIN(WireCodecTest)
#include "test_wire_codec.moc"
