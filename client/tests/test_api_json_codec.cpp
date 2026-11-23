#include "ApiJsonCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class ApiJsonCodecTest final : public QObject {
    Q_OBJECT
private slots:
    void parsesNestedAuthAndLegacyAliases();
    void preservesLegacyMessagePayloadBranch();
    void adaptsFileShapesAndUploadedChunks();
    void extractsNestedErrorsAndFallbacks();
};

void ApiJsonCodecTest::parsesNestedAuthAndLegacyAliases()
{
    const QJsonObject root{{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("user_id"), 42},
            {QStringLiteral("name"), QStringLiteral("alice")},
            {QStringLiteral("nickname"), QStringLiteral("Alice")}}},
        {QStringLiteral("tokens"), QJsonObject{
            {QStringLiteral("accessToken"), QStringLiteral("access")},
            {QStringLiteral("refreshToken"), QStringLiteral("refresh")}}}}}};
    const AuthSession session = ApiJsonCodec::parseAuth(root);
    QCOMPARE(session.accessToken, QStringLiteral("access"));
    QCOMPARE(session.refreshToken, QStringLiteral("refresh"));
    QCOMPARE(session.user.id, QStringLiteral("42"));
    QCOMPARE(session.user.username, QStringLiteral("alice"));
    QCOMPARE(session.user.displayName, QStringLiteral("Alice"));
}

void ApiJsonCodecTest::preservesLegacyMessagePayloadBranch()
{
    const QJsonObject payload{{QStringLiteral("type"), QStringLiteral("file")},
        {QStringLiteral("file_id"), QStringLiteral("file-1")},
        {QStringLiteral("filename"), QStringLiteral("legacy.txt")},
        {QStringLiteral("size"), QStringLiteral("4096")}};
    const QJsonObject legacy{{QStringLiteral("message_id"), QStringLiteral("message-1")},
        {QStringLiteral("sender_id"), QStringLiteral("peer")},
        {QStringLiteral("created_at"), QStringLiteral("123456")},
        {QStringLiteral("payload"), QString::fromUtf8(
            QJsonDocument(payload).toJson(QJsonDocument::Compact))}};
    const ChatMessage message = ApiJsonCodec::parseMessage(legacy);
    QCOMPARE(message.id, QStringLiteral("message-1"));
    QCOMPARE(message.kind, QStringLiteral("file"));
    QCOMPARE(message.fileId, QStringLiteral("file-1"));
    QCOMPARE(message.fileName, QStringLiteral("legacy.txt"));
    QCOMPARE(message.fileSize, qint64(4096));
    QCOMPARE(message.timestamp, qint64(123456));
    QVERIFY(message.isFile());
}

void ApiJsonCodecTest::adaptsFileShapesAndUploadedChunks()
{
    const QJsonObject file{{QStringLiteral("id"), QStringLiteral("file-1")},
        {QStringLiteral("original_name"), QStringLiteral("hello.txt")},
        {QStringLiteral("file_size"), QStringLiteral("8192")},
        {QStringLiteral("status"), QStringLiteral("completed")}};
    const QJsonObject root{{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("file"), file},
        {QStringLiteral("uploaded_chunks"), QJsonArray{0,
            QJsonObject{{QStringLiteral("chunk_index"), QStringLiteral("2")}},
            QJsonObject{{QStringLiteral("index"), 4}, {QStringLiteral("uploaded"), false}}}}}}};
    const FileStatus status = ApiJsonCodec::parseFileStatus(root);
    QCOMPARE(status.fileId, QStringLiteral("file-1"));
    QCOMPARE(status.fileName, QStringLiteral("hello.txt"));
    QCOMPARE(status.size, qint64(8192));
    QCOMPARE(status.chunkSize, DefaultFileChunkSize);
    QCOMPARE(status.uploadedChunks, QSet<int>({0, 2}));
    QVERIFY(status.complete);

    const FileInitInfo instant = ApiJsonCodec::parseFileInit(QJsonObject{
        {QStringLiteral("file_id"), QStringLiteral("instant-1")},
        {QStringLiteral("instant"), true}});
    QCOMPARE(instant.fileId, QStringLiteral("instant-1"));
    QVERIFY(instant.complete);
}

void ApiJsonCodecTest::extractsNestedErrorsAndFallbacks()
{
    QCOMPARE(ApiJsonCodec::responseError(
        R"({"error":{"code":"invalid","message":"用户名或密码错误"}})", QStringLiteral("HTTP 401")),
        QStringLiteral("用户名或密码错误"));
    QCOMPARE(ApiJsonCodec::responseError(R"({"detail":"failed"})", QStringLiteral("HTTP 500")),
             QStringLiteral("failed"));
    QCOMPARE(ApiJsonCodec::responseError("not-json", QStringLiteral("HTTP 503")),
             QStringLiteral("HTTP 503"));
}

QTEST_GUILESS_MAIN(ApiJsonCodecTest)
#include "test_api_json_codec.moc"
