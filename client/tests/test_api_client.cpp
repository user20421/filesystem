#include "ApiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
struct CapturedRequest {
    QByteArray method;
    QByteArray path;
    QByteArray body;
    QByteArray authorization;
};

class FakeServer final : public QObject {
public:
    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections())
                acceptSocket(m_server.nextPendingConnection());
        });
        return true;
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }

    void enqueue(int status, const QJsonObject &body)
    {
        m_responses.append({status, QJsonDocument(body).toJson(QJsonDocument::Compact)});
    }

    QList<CapturedRequest> requests;

private:
    void acceptSocket(QTcpSocket *socket)
    {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            qsizetype contentLength = 0;
            QByteArray authorization;
            for (const QByteArray &line : lines) {
                if (line.toLower().startsWith("content-length:"))
                    contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                if (line.toLower().startsWith("authorization:"))
                    authorization = line.mid(line.indexOf(':') + 1).trimmed();
            }
            const int bodyStart = headerEnd + 4;
            if (buffer->size() - bodyStart < contentLength)
                return;
            requests.append({requestLine.value(0), requestLine.value(1), buffer->mid(bodyStart),
                             authorization});
            const auto response = m_responses.isEmpty()
                ? QPair<int, QByteArray>{500, QByteArray("{}")} : m_responses.takeFirst();
            const QByteArray http = "HTTP/1.1 " + QByteArray::number(response.first)
                + " Test\r\nContent-Type: application/json\r\nContent-Length: "
                + QByteArray::number(response.second.size())
                + "\r\nConnection: close\r\n\r\n" + response.second;
            socket->write(http);
            socket->flush();
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    QTcpServer m_server;
    QList<QPair<int, QByteArray>> m_responses;
};

QJsonObject userJson(const QString &id, const QString &username)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("username"), username}};
}
} // namespace

class ApiClientTest final : public QObject {
    Q_OBJECT
private slots:
    void parsesNestedAuthTokens();
    void parsesNestedError();
    void adaptsRealFileShapes();
    void decodesHistoryEnvelopeAndCursor();
    void presenceAndReadReceiptRequests();
    void deletesCurrentAccountOnlyOnNoContent();
};

void ApiClientTest::parsesNestedAuthTokens()
{
    FakeServer server;
    QVERIFY(server.start());
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("user"), userJson(QStringLiteral("u1"), QStringLiteral("alice"))},
        {QStringLiteral("tokens"), QJsonObject{{QStringLiteral("access_token"), QStringLiteral("a")},
                                               {QStringLiteral("refresh_token"), QStringLiteral("r")}}}}}});
    ApiClient api;
    api.setServerUrl(server.url());
    bool done = false;
    AuthSession session;
    api.registerUser(QStringLiteral("ALICE"), QStringLiteral("123456"),
                     [&](bool ok, const AuthSession &result, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        session = result;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QCOMPARE(session.accessToken, QStringLiteral("a"));
    QCOMPARE(session.refreshToken, QStringLiteral("r"));
    QCOMPARE(session.user.id, QStringLiteral("u1"));
    QCOMPARE(session.user.displayName, QStringLiteral("alice"));
    QCOMPARE(server.requests.size(), 1);
    const QJsonObject request = QJsonDocument::fromJson(server.requests.first().body).object();
    QCOMPARE(request.value(QStringLiteral("display_name")).toString(), QStringLiteral("ALICE"));
    QCOMPARE(request.value(QStringLiteral("password")).toString(), QStringLiteral("123456"));
}

void ApiClientTest::parsesNestedError()
{
    FakeServer server;
    QVERIFY(server.start());
    server.enqueue(401, {{QStringLiteral("error"), QJsonObject{
        {QStringLiteral("code"), QStringLiteral("invalid_credentials")},
        {QStringLiteral("message"), QStringLiteral("用户名或密码错误")},
        {QStringLiteral("request_id"), QStringLiteral("req-1")}}}});
    ApiClient api;
    api.setServerUrl(server.url());
    bool done = false;
    QString message;
    api.login(QStringLiteral("alice"), QStringLiteral("123456789012"),
              [&](bool ok, const AuthSession &, const QString &error) {
        QVERIFY(!ok);
        message = error;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QCOMPARE(message, QStringLiteral("用户名或密码错误"));
}

void ApiClientTest::adaptsRealFileShapes()
{
    FakeServer server;
    QVERIFY(server.start());
    const QJsonObject file{{QStringLiteral("id"), QStringLiteral("f1")},
        {QStringLiteral("original_name"), QStringLiteral("hello.txt")},
        {QStringLiteral("size"), 5}, {QStringLiteral("chunk_size"), 4194304},
        {QStringLiteral("status"), QStringLiteral("completed")}};
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{{QStringLiteral("file"), file}}}});
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("file"), file},
        {QStringLiteral("uploaded_chunks"), QJsonArray{0, 2, 4}}}}});
    ApiClient api;
    api.setServerUrl(server.url());
    bool initDone = false;
    FileInitInfo info;
    api.initFile(QStringLiteral("u2"), QStringLiteral("hello.txt"), 5, QStringLiteral("abc"),
                 [&](bool ok, const FileInitInfo &result, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        info = result;
        initDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(initDone, 3000);
    QCOMPARE(info.fileId, QStringLiteral("f1"));
    QCOMPARE(info.chunkSize, 4194304);
    const QJsonObject initRequest = QJsonDocument::fromJson(server.requests.first().body).object();
    QCOMPARE(initRequest.value(QStringLiteral("recipient_id")).toString(), QStringLiteral("u2"));
    QCOMPARE(initRequest.value(QStringLiteral("filename")).toString(), QStringLiteral("hello.txt"));
    QVERIFY(!initRequest.contains(QStringLiteral("peer_id")));
    QVERIFY(!initRequest.contains(QStringLiteral("file_name")));

    bool statusDone = false;
    FileStatus status;
    api.fileStatus(QStringLiteral("f1"),
                   [&](bool ok, const FileStatus &result, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        status = result;
        statusDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(statusDone, 3000);
    QCOMPARE(status.fileName, QStringLiteral("hello.txt"));
    QCOMPARE(status.uploadedChunks, QSet<int>({0, 2, 4}));
    QCOMPARE(status.status, QStringLiteral("completed"));
    QVERIFY(status.complete);
}

void ApiClientTest::decodesHistoryEnvelopeAndCursor()
{
    auto codec = createDefaultWireCodec();
    WireMessage wire;
    wire.messageId = QStringLiteral("m1");
    wire.conversationId = QStringLiteral("u1:u2");
    wire.senderId = QStringLiteral("u2");
    wire.recipientId = QStringLiteral("u1");
    wire.timestamp = 123456;
    wire.readAt = 123999;
    wire.payload = QJsonDocument(textPayload(QStringLiteral("hello"))).toJson(QJsonDocument::Compact);
    QByteArray frame;
    QString codecError;
    QVERIFY2(codec->encode(wire, &frame, &codecError), qPrintable(codecError));

    FakeServer server;
    QVERIFY(server.start());
    const QJsonObject item{{QStringLiteral("cursor"), 42},
                           {QStringLiteral("envelope"), QString::fromLatin1(frame.toBase64())}};
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("messages"), QJsonArray{item}}}}});
    ApiClient api;
    api.setServerUrl(server.url());
    bool done = false;
    QList<ChatMessage> messages;
    api.messages(QStringLiteral("u2"), QStringLiteral("41"),
                 [&](bool ok, const QList<ChatMessage> &result, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        messages = result;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.first().cursor, QStringLiteral("42"));
    QCOMPARE(messages.first().id, QStringLiteral("m1"));
    QCOMPARE(messages.first().senderId, QStringLiteral("u2"));
    QCOMPARE(messages.first().content, QStringLiteral("hello"));
    QCOMPARE(messages.first().readAt, qint64(123999));
    QVERIFY(server.requests.first().path.contains("after_id=41"));
}

void ApiClientTest::presenceAndReadReceiptRequests()
{
    FakeServer server;
    QVERIFY(server.start());
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{
        {QStringLiteral("online_user_ids"), QJsonArray{QStringLiteral("u2"), QStringLiteral("u3")}}}}});
    server.enqueue(200, {{QStringLiteral("data"), QJsonObject{}}});
    ApiClient api;
    api.setServerUrl(server.url());
    bool presenceDone = false;
    QSet<QString> online;
    api.presence([&](bool ok, const QSet<QString> &result, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        online = result;
        presenceDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(presenceDone, 3000);
    QCOMPARE(online, QSet<QString>({QStringLiteral("u2"), QStringLiteral("u3")}));
    QCOMPARE(server.requests.first().path, QByteArray("/api/v1/presence"));

    bool readDone = false;
    api.markRead(QStringLiteral("u2"), QStringLiteral("42"),
                 [&](bool ok, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        readDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(readDone, 3000);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(server.requests[1].path, QByteArray("/api/v1/messages/read"));
    const QJsonObject request = QJsonDocument::fromJson(server.requests[1].body).object();
    QCOMPARE(request.value(QStringLiteral("peer_id")).toString(), QStringLiteral("u2"));
    QCOMPARE(request.value(QStringLiteral("cursor")).toVariant().toLongLong(), 42);
}

void ApiClientTest::deletesCurrentAccountOnlyOnNoContent()
{
    FakeServer server;
    QVERIFY(server.start());
    server.enqueue(200, {});
    server.enqueue(204, {});
    ApiClient api;
    api.setServerUrl(server.url());
    api.setAccessToken(QStringLiteral("account-token"));

    bool firstDone = false;
    api.deleteCurrentAccount([&](bool ok, const QString &error) {
        QVERIFY(!ok);
        QCOMPARE(error, QStringLiteral("HTTP 200"));
        firstDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(firstDone, 3000);

    bool secondDone = false;
    api.deleteCurrentAccount([&](bool ok, const QString &error) {
        QVERIFY2(ok, qPrintable(error));
        secondDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(secondDone, 3000);
    QCOMPARE(server.requests.size(), 2);
    for (const CapturedRequest &request : server.requests) {
        QCOMPARE(request.method, QByteArray("DELETE"));
        QCOMPARE(request.path, QByteArray("/api/v1/users/me"));
        QCOMPARE(request.authorization, QByteArray("Bearer account-token"));
    }
}

QTEST_GUILESS_MAIN(ApiClientTest)
#include "test_api_client.moc"
