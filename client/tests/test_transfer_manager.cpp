#include "LocalStore.h"
#include "TransferManager.h"

#include <QDir>
#include <QFile>
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

QTEST_GUILESS_MAIN(TransferManagerTest)
#include "test_transfer_manager.moc"
