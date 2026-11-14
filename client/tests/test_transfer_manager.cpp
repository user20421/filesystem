#include "LocalStore.h"
#include "TransferManager.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
class DownloadServer final : public QObject {
public:
    explicit DownloadServer(QByteArray body, int delayMs = 0)
        : m_body(std::move(body)), m_delayMs(delayMs)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections())
                accept(m_server.nextPendingConnection());
        });
    }
    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }

private:
    void accept(QTcpSocket *socket)
    {
        auto request = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request] {
            request->append(socket->readAll());
            if (!request->contains("\r\n\r\n") || request->startsWith("HTTP/"))
                return;
            request->clear();
            request->append("HTTP/"); // Do not enter this handler a second time.
            const QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                "Content-Length: " + QByteArray::number(m_body.size())
                + "\r\nConnection: close\r\n\r\n";
            socket->write(header);
            auto offset = std::make_shared<qsizetype>(0);
            auto *timer = new QTimer(socket);
            timer->setInterval(m_delayMs);
            connect(timer, &QTimer::timeout, socket, [this, socket, offset] {
                constexpr qsizetype blockSize = 64 * 1024;
                const qsizetype count = qMin(blockSize, m_body.size() - *offset);
                socket->write(m_body.constData() + *offset, count);
                *offset += count;
                if (*offset >= m_body.size()) {
                    socket->disconnectFromHost();
                    socket->disconnect(this);
                }
            });
            timer->start();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    QTcpServer m_server;
    QByteArray m_body;
    int m_delayMs;
};

class SlowUploadServer final : public QObject {
public:
    SlowUploadServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections())
                accept(m_server.nextPendingConnection());
        });
    }
    ~SlowUploadServer() override
    {
        const auto sockets = m_sockets;
        for (const QPointer<QTcpSocket> &socket : sockets) {
            if (socket) {
                socket->disconnect();
                delete socket.data();
            }
        }
    }
    bool start() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }
    int attempts(int index) const { return m_attempts.value(index); }
    int statusRequests() const { return m_statusRequests; }
    int initRequests() const { return m_initRequests; }
    bool completed() const { return m_completed; }
    QSet<int> confirmedChunks() const { return m_confirmedChunks; }
    QSet<int> abortedChunks() const { return m_abortedChunks; }

private:
    static void respond(QTcpSocket *socket, const QJsonObject &object)
    {
        const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    static QJsonObject fileObject()
    {
        return {{QStringLiteral("id"), QStringLiteral("slow-upload")},
                {QStringLiteral("size"), 10 * 1024 * 1024 + 123},
                {QStringLiteral("chunk_size"), ChunkSize},
                {QStringLiteral("status"), QStringLiteral("uploading")}};
    }

    QJsonArray uploadedChunks() const
    {
        QList<int> chunks = m_confirmedChunks.values();
        std::sort(chunks.begin(), chunks.end());
        QJsonArray result;
        for (int index : chunks)
            result.append(index);
        return result;
    }

    void accept(QTcpSocket *socket)
    {
        m_sockets.append(socket);
        auto request = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        auto responded = std::make_shared<bool>(false);
        auto chunkIndex = std::make_shared<int>(-1);
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket, request, handled, responded, chunkIndex] {
            if (*handled)
                return;
            request->append(socket->readAll());
            const qsizetype headerEnd = request->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QList<QByteArray> lines = request->left(headerEnd).split('\n');
            if (lines.isEmpty())
                return;
            const QList<QByteArray> firstLine = lines.first().trimmed().split(' ');
            if (firstLine.size() < 2)
                return;
            qsizetype contentLength = 0;
            for (const QByteArray &line : lines) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:"))
                    contentLength = trimmed.mid(15).trimmed().toLongLong();
            }
            if (request->size() < headerEnd + 4 + contentLength)
                return;
            *handled = true;
            const QByteArray method = firstLine.at(0);
            const QByteArray path = firstLine.at(1);
            if (method == "POST" && path == "/api/v1/files/init") {
                ++m_initRequests;
                const QJsonObject data{{QStringLiteral("file"), fileObject()},
                                       {QStringLiteral("uploaded_chunks"), QJsonArray{}}};
                respond(socket, QJsonObject{{QStringLiteral("data"), data}});
                *responded = true;
                return;
            }
            if (method == "GET" && path == "/api/v1/files/slow-upload") {
                ++m_statusRequests;
                const QJsonObject data{{QStringLiteral("file"), fileObject()},
                                       {QStringLiteral("uploaded_chunks"), uploadedChunks()}};
                respond(socket, QJsonObject{{QStringLiteral("data"), data}});
                *responded = true;
                return;
            }
            if (method == "PUT" && path.startsWith("/api/v1/files/slow-upload/chunks/")) {
                const int index = path.mid(path.lastIndexOf('/') + 1).toInt();
                *chunkIndex = index;
                ++m_attempts[index];
                auto *timer = new QTimer(socket);
                timer->setSingleShot(true);
                timer->setInterval(index == 0 ? 20 : 400);
                connect(timer, &QTimer::timeout, socket,
                        [this, socket, index, responded] {
                    m_confirmedChunks.insert(index);
                    *responded = true;
                    respond(socket, {});
                });
                timer->start();
                return;
            }
            if (method == "POST" && path == "/api/v1/files/slow-upload/complete") {
                m_completed = true;
                *responded = true;
                respond(socket, {});
                return;
            }
            *responded = true;
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket, handled, responded, chunkIndex] {
            if (*handled && !*responded && *chunkIndex >= 0)
                m_abortedChunks.insert(*chunkIndex);
            m_sockets.removeAll(socket);
            socket->deleteLater();
        });
    }

    static constexpr int ChunkSize = 4 * 1024 * 1024;
    QTcpServer m_server;
    QMap<int, int> m_attempts;
    QList<QPointer<QTcpSocket>> m_sockets;
    QSet<int> m_confirmedChunks;
    QSet<int> m_abortedChunks;
    int m_statusRequests = 0;
    int m_initRequests = 0;
    bool m_completed = false;
};

bool directoryEmpty(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}
} // namespace

class TransferManagerTest final : public QObject {
    Q_OBJECT
private slots:
    void limitsFileLevelConcurrency();
    void rejectsMissingUpload();
    void streamsLargeDownloadAtomically();
    void rejectsSizeMismatchAndCleansTemporaryFile();
    void pauseDiscardsPartialAndResumeRestarts();
    void slowUploadPauseAbortsAndResumeKeepsConfirmedChunks();
};

void TransferManagerTest::limitsFileLevelConcurrency()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    TransferManager manager(&api, &store);
    QCOMPARE(manager.concurrency(), 3);
    manager.setConcurrency(4);
    QCOMPARE(manager.concurrency(), 4);
    manager.setConcurrency(8);
    QCOMPARE(manager.concurrency(), 5);
    manager.setConcurrency(0);
    QCOMPARE(manager.concurrency(), 3);
}

void TransferManagerTest::rejectsMissingUpload()
{
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    TransferManager manager(&api, &store);
    QVERIFY(manager.addUpload(QStringLiteral("peer"), dir.filePath(QStringLiteral("missing"))) == nullptr);
    QVERIFY(manager.tasks().isEmpty());
}

void TransferManagerTest::streamsLargeDownloadAtomically()
{
    QByteArray body(8 * 1024 * 1024 + 123, '\0');
    for (qsizetype index = 0; index < body.size(); ++index)
        body[index] = static_cast<char>((index * 31) & 0xff);
    DownloadServer server(body);
    QVERIFY(server.start());
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("file"),
                                             QStringLiteral("large.bin"), body.size());
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 20000);
    QCOMPARE(task->transferredBytes(), qint64(body.size()));
    QFile output(task->localPath());
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.size(), qint64(body.size()));
    QCOMPARE(output.readAll(), body);
}

void TransferManagerTest::rejectsSizeMismatchAndCleansTemporaryFile()
{
    const QByteArray body("short body");
    DownloadServer server(body);
    QVERIFY(server.start());
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("file"),
                                             QStringLiteral("wrong-size.bin"), body.size() + 100);
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Failed, 10000);
    QVERIFY(task->error().contains(QStringLiteral("大小不匹配")));
    QVERIFY(!QFileInfo::exists(task->localPath()));
    QTRY_VERIFY_WITH_TIMEOUT(directoryEmpty(store.path(QStringLiteral("downloads"))), 5000);
}

void TransferManagerTest::pauseDiscardsPartialAndResumeRestarts()
{
    const QByteArray body(2 * 1024 * 1024, 'p');
    DownloadServer server(body, 2);
    QVERIFY(server.start());
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("file"),
                                             QStringLiteral("pause.bin"), body.size());
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(task->transferredBytes() > 0
                             && task->transferredBytes() < body.size(), 10000);
    manager.pause(task->id());
    QCOMPARE(task->state(), TransferTask::State::Paused);
    QTRY_VERIFY_WITH_TIMEOUT(directoryEmpty(store.path(QStringLiteral("downloads"))), 5000);

    manager.resume(task->id());
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 20000);
    QFile output(task->localPath());
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), body);
}

void TransferManagerTest::slowUploadPauseAbortsAndResumeKeepsConfirmedChunks()
{
    const QByteArray body(10 * 1024 * 1024 + 123, 'u');
    SlowUploadServer server;
    QVERIFY(server.start());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile source(dir.filePath(QStringLiteral("source.bin")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(body), qint64(body.size()));
    source.close();

    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addUpload(QStringLiteral("peer"), source.fileName());
    QVERIFY(task);

    QTRY_VERIFY_WITH_TIMEOUT(server.confirmedChunks().contains(0)
                             && server.attempts(1) == 1, 10000);
    manager.pause(task->id());
    QCOMPARE(task->state(), TransferTask::State::Paused);
    QTRY_VERIFY_WITH_TIMEOUT(server.abortedChunks().contains(1), 5000);
    const qint64 pausedBytes = task->transferredBytes();
    QTest::qWait(500);
    QCOMPARE(task->transferredBytes(), pausedBytes);
    QCOMPARE(task->state(), TransferTask::State::Paused);

    manager.resume(task->id());
    QTRY_VERIFY_WITH_TIMEOUT(server.confirmedChunks().contains(1)
                             && server.attempts(2) == 1, 15000);
    QCOMPARE(server.attempts(0), 1);

    // Also cover immediate resume; asynchronous abort callbacks use resumePending.
    manager.pause(task->id());
    manager.resume(task->id());
    QVERIFY(task->state() != TransferTask::State::Failed);
    QTRY_VERIFY_WITH_TIMEOUT(server.abortedChunks().contains(2), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 20000);

    QVERIFY(server.completed());
    QCOMPARE(server.initRequests(), 1);
    QVERIFY(server.statusRequests() >= 2);
    QCOMPARE(server.attempts(0), 1);
    QCOMPARE(server.attempts(1), 2); // Aborted chunks are retried; confirmed chunks are not.
    QCOMPARE(server.attempts(2), 2);
    QCOMPARE(task->transferredBytes(), qint64(body.size()));
}

QTEST_GUILESS_MAIN(TransferManagerTest)
#include "test_transfer_manager.moc"
