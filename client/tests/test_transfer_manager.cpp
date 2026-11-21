#include "LocalStore.h"
#include "TransferManager.h"
#include "Workers.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>

namespace {
QString chunkKey(const QString &fileId, int index)
{
    return fileId + QLatin1Char(':') + QString::number(index);
}

QString createSource(const QString &directory, const QString &name, qint64 size, char fill = 'u')
{
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    const QByteArray body(size, fill);
    if (file.write(body) != size)
        return {};
    return path;
}

bool directoryEmpty(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}

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
            request->append("HTTP/");
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

class ControlledTransferServer final : public QObject {
public:
    ControlledTransferServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections())
                accept(m_server.nextPendingConnection());
        });
    }
    ~ControlledTransferServer() override
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
    void setDownloadBody(const QString &fileId, const QByteArray &body) { m_downloadBodies.insert(fileId, body); }
    void expireDownloadOnce(const QString &fileId) { m_unauthorizedDownloads.insert(fileId); }
    void holdRefresh(bool hold = true) { m_holdRefresh = hold; }
    void holdDelete(const QString &fileId) { m_heldDeletes.insert(fileId); }
    int refreshRequests() const { return m_refreshRequests; }
    int downloadRequests(const QString &fileId) const { return m_downloadRequests.value(fileId); }
    QList<QByteArray> downloadAuthorizations(const QString &fileId) const {
        return m_downloadAuthorizations.value(fileId);
    }

    int activePuts() const { return m_activePuts.size(); }
    int maxActivePuts() const { return m_maxActivePuts; }
    QSet<QString> activePutKeys() const { return m_activePuts; }
    int attempts(const QString &fileId, int index) const { return m_attempts.value(chunkKey(fileId, index)); }
    int aborted(const QString &fileId, int index) const { return m_aborted.value(chunkKey(fileId, index)); }
    QSet<int> confirmed(const QString &fileId) const { return m_files.value(fileId).confirmed; }
    int completeRequests(const QString &fileId) const { return m_files.value(fileId).completeRequests; }
    int earlyCompletes(const QString &fileId) const { return m_files.value(fileId).earlyCompletes; }
    int deleteRequests(const QString &fileId) const { return m_files.value(fileId).deleteRequests; }
    int statusRequests() const { return m_statusRequests; }
    int initRequests() const { return m_initRequests; }
    int initChunkSize() const { return m_initChunkSize; }
    int activeDownloads() const { return m_activeDownloads; }
    int maxActiveDownloads() const { return m_maxActiveDownloads; }
    QSet<QString> activeDownloadIds() const {
        const auto keys = m_pendingDownloads.keys();
        return QSet<QString>(keys.cbegin(), keys.cend());
    }

    void respondPut(const QString &fileId, int index)
    {
        const QString key = chunkKey(fileId, index);
        QTcpSocket *socket = m_pendingPuts.value(key).data();
        QVERIFY(socket);
        QVERIFY(m_activePuts.remove(key));
        m_pendingPuts.remove(key);
        m_files[fileId].confirmed.insert(index);
        m_responded[socket] = true;
        respond(socket, {});
    }

    void respondDelete(const QString &fileId)
    {
        QTcpSocket *socket = m_pendingDeletes.value(fileId).data();
        QVERIFY(socket);
        m_pendingDeletes.remove(fileId);
        m_heldDeletes.remove(fileId);
        m_responded[socket] = true;
        respond(socket, {});
    }

    void respondRefresh()
    {
        QTcpSocket *socket = m_pendingRefresh.data();
        QVERIFY(socket);
        m_pendingRefresh.clear();
        m_holdRefresh = false;
        m_responded[socket] = true;
        respond(socket, {{QStringLiteral("data"), QJsonObject{
            {QStringLiteral("user"), QJsonObject{
                {QStringLiteral("id"), QStringLiteral("u1")},
                {QStringLiteral("username"), QStringLiteral("ALICE")}}},
            {QStringLiteral("tokens"), QJsonObject{
                {QStringLiteral("access_token"), QStringLiteral("new-access")},
                {QStringLiteral("refresh_token"), QStringLiteral("new-refresh")}}}}}});
    }

    void respondDownload(const QString &fileId)
    {
        QTcpSocket *socket = m_pendingDownloads.value(fileId).data();
        QVERIFY(socket);
        m_pendingDownloads.remove(fileId);
        --m_activeDownloads;
        m_responded[socket] = true;
        const QByteArray body = m_downloadBodies.value(fileId);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

private:
    struct FileState {
        qint64 size = 0;
        int totalChunks = 0;
        QSet<int> confirmed;
        int completeRequests = 0;
        int earlyCompletes = 0;
        int deleteRequests = 0;
        bool complete = false;
    };

    static void respond(QTcpSocket *socket, const QJsonObject &object, int status = 200)
    {
        const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\n"
            + "Content-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QJsonObject fileObject(const QString &fileId) const
    {
        const FileState file = m_files.value(fileId);
        return {{QStringLiteral("id"), fileId}, {QStringLiteral("size"), file.size},
                {QStringLiteral("chunk_size"), DefaultFileChunkSize},
                {QStringLiteral("instant"), fileId == QLatin1String("f-instant.bin")},
                {QStringLiteral("status"), file.complete ? QStringLiteral("completed")
                                                         : QStringLiteral("uploading")}};
    }

    QJsonArray uploadedChunks(const QString &fileId) const
    {
        QList<int> chunks = m_files.value(fileId).confirmed.values();
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
        auto method = std::make_shared<QByteArray>();
        auto path = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket, request, handled, method, path] {
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
            QByteArray authorization;
            qsizetype contentLength = 0;
            for (const QByteArray &line : lines) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:"))
                    contentLength = trimmed.mid(15).trimmed().toLongLong();
                if (trimmed.toLower().startsWith("authorization:"))
                    authorization = trimmed.mid(trimmed.indexOf(':') + 1).trimmed();
            }
            if (request->size() < headerEnd + 4 + contentLength)
                return;
            *handled = true;
            *method = firstLine.at(0);
            *path = firstLine.at(1);
            handle(socket, *method, *path,
                   request->mid(headerEnd + 4, static_cast<qsizetype>(contentLength)), authorization);
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                [this, socket, handled, method, path] {
            if (*handled && !m_responded.value(socket)) {
                if (*method == "PUT") {
                    const QString fileId = fileFromPath(*path);
                    const int index = chunkFromPath(*path);
                    const QString key = chunkKey(fileId, index);
                    if (m_activePuts.remove(key)) {
                        m_pendingPuts.remove(key);
                        ++m_aborted[key];
                    }
                } else if (*method == "GET" && path->endsWith("/download")) {
                    const QString fileId = fileFromPath(*path);
                    if (m_pendingDownloads.remove(fileId))
                        --m_activeDownloads;
                }
            }
            m_responded.remove(socket);
            m_sockets.removeAll(socket);
            socket->deleteLater();
        });
    }

    static QString fileFromPath(const QByteArray &path)
    {
        return QString::fromUtf8(path.split('/').value(4));
    }
    static int chunkFromPath(const QByteArray &path)
    {
        return path.mid(path.lastIndexOf('/') + 1).toInt();
    }

    void handle(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                const QByteArray &body, const QByteArray &authorization)
    {
        if (method == "POST" && path == "/api/v1/auth/refresh") {
            ++m_refreshRequests;
            if (m_holdRefresh) {
                m_pendingRefresh = socket;
            } else {
                m_responded[socket] = true;
                respond(socket, {{QStringLiteral("data"), QJsonObject{
                    {QStringLiteral("user"), QJsonObject{
                        {QStringLiteral("id"), QStringLiteral("u1")},
                        {QStringLiteral("username"), QStringLiteral("ALICE")}}},
                    {QStringLiteral("tokens"), QJsonObject{
                        {QStringLiteral("access_token"), QStringLiteral("new-access")},
                        {QStringLiteral("refresh_token"), QStringLiteral("new-refresh")}}}}}});
            }
            return;
        }
        if (method == "POST" && path == "/api/v1/files/init") {
            ++m_initRequests;
            const QJsonObject request = QJsonDocument::fromJson(body).object();
            m_initChunkSize = request.value(QStringLiteral("chunk_size")).toInt();
            const QString name = request.value(QStringLiteral("filename")).toString();
            const QString fileId = QStringLiteral("f-") + name;
            FileState file;
            file.deleteRequests = m_files.value(fileId).deleteRequests;
            file.size = request.value(QStringLiteral("size")).toVariant().toLongLong();
            file.totalChunks = file.size == 0 ? 0
                : static_cast<int>((file.size + DefaultFileChunkSize - 1) / DefaultFileChunkSize);
            file.complete = name.startsWith(QLatin1String("instant"))
                || name.startsWith(QLatin1String("completed"));
            m_files.insert(fileId, file);
            const QJsonObject data{{QStringLiteral("file"), fileObject(fileId)},
                                   {QStringLiteral("uploaded_chunks"), QJsonArray{}}};
            m_responded[socket] = true;
            respond(socket, QJsonObject{{QStringLiteral("data"), data}});
            return;
        }
        const QString fileId = fileFromPath(path);
        if (method == "GET" && path.endsWith("/download")) {
            ++m_downloadRequests[fileId];
            m_downloadAuthorizations[fileId].append(authorization);
            if (m_unauthorizedDownloads.remove(fileId)) {
                m_responded[socket] = true;
                respond(socket, {{QStringLiteral("error"), QStringLiteral("expired")}}, 401);
                return;
            }
            ++m_activeDownloads;
            m_maxActiveDownloads = qMax(m_maxActiveDownloads, m_activeDownloads);
            m_pendingDownloads.insert(fileId, socket);
            return;
        }
        if (method == "GET" && path == "/api/v1/files/" + fileId.toUtf8()) {
            ++m_statusRequests;
            const QJsonObject data{{QStringLiteral("file"), fileObject(fileId)},
                                   {QStringLiteral("uploaded_chunks"), uploadedChunks(fileId)}};
            m_responded[socket] = true;
            respond(socket, QJsonObject{{QStringLiteral("data"), data}});
            return;
        }
        if (method == "PUT" && path.startsWith("/api/v1/files/" + fileId.toUtf8() + "/chunks/")) {
            const int index = chunkFromPath(path);
            const QString key = chunkKey(fileId, index);
            ++m_attempts[key];
            m_activePuts.insert(key);
            m_maxActivePuts = qMax(m_maxActivePuts, m_activePuts.size());
            m_pendingPuts.insert(key, socket);
            return;
        }
        if (method == "POST" && path == "/api/v1/files/" + fileId.toUtf8() + "/complete") {
            FileState &file = m_files[fileId];
            ++file.completeRequests;
            if (file.confirmed.size() != file.totalChunks)
                ++file.earlyCompletes;
            file.complete = true;
            m_responded[socket] = true;
            respond(socket, {});
            return;
        }
        if (method == "DELETE" && path == "/api/v1/files/" + fileId.toUtf8()) {
            ++m_files[fileId].deleteRequests;
            if (m_heldDeletes.contains(fileId)) {
                m_pendingDeletes.insert(fileId, socket);
            } else {
                m_responded[socket] = true;
                respond(socket, {});
            }
            return;
        }
        m_responded[socket] = true;
        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<QPointer<QTcpSocket>> m_sockets;
    QHash<QTcpSocket *, bool> m_responded;
    QMap<QString, FileState> m_files;
    QMap<QString, int> m_attempts;
    QMap<QString, int> m_aborted;
    QSet<QString> m_activePuts;
    QMap<QString, QPointer<QTcpSocket>> m_pendingPuts;
    QMap<QString, QPointer<QTcpSocket>> m_pendingDownloads;
    QMap<QString, QPointer<QTcpSocket>> m_pendingDeletes;
    QSet<QString> m_heldDeletes;
    QMap<QString, QByteArray> m_downloadBodies;
    QMap<QString, int> m_downloadRequests;
    QMap<QString, QList<QByteArray>> m_downloadAuthorizations;
    QSet<QString> m_unauthorizedDownloads;
    QPointer<QTcpSocket> m_pendingRefresh;
    bool m_holdRefresh = false;
    int m_refreshRequests = 0;
    int m_maxActivePuts = 0;
    int m_statusRequests = 0;
    int m_initRequests = 0;
    int m_initChunkSize = 0;
    int m_activeDownloads = 0;
    int m_maxActiveDownloads = 0;
};
} // namespace

class TransferManagerTest final : public QObject {
    Q_OBJECT
private slots:
    void usesFixedChannelCounts();
    void rejectsMissingUpload();
    void singleFileRunsThreeChunksAndCompletesOnceOutOfOrder();
    void newReadyFilesGetFifoChannelsBeforeLargeBacklog();
    void pauseResumeAbortsAndKeepsConfirmedChunks();
    void cancelAndShutdownAbortAllPuts();
    void instantAndCompletedInitSkipChunkAndCompleteRequests();
    void uploadsAndThreeDownloadsUseIndependentLimits();
    void streamsLargeDownloadAtomically();
    void rejectsSizeMismatchAndCleansTemporaryFile();
    void pauseDiscardsPartialAndResumeRestarts();
    void streamingDownloadRetryUsesStableHandleWithoutMixingErrorBody();
    void cancelWhileWaitingForRefreshDoesNotReplayDownload();
    void immediateRetryWaitsForCancellationDelete();
    void windowsDownloadReservationsIgnoreCase();
    void slowWriterBoundsQueueAndAppliesNetworkBackpressure();
};

void TransferManagerTest::usesFixedChannelCounts()
{
    QCOMPARE(TransferManager::UploadChannelCount, 3);
    QCOMPARE(TransferManager::DownloadChannelCount, 3);
    QCOMPARE(DefaultFileChunkSize, 5 * 1024 * 1024);
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

void TransferManagerTest::singleFileRunsThreeChunksAndCompletesOnceOutOfOrder()
{
    QTemporaryDir dir;
    const QString path = createSource(dir.path(), QStringLiteral("parallel.bin"),
                                      4LL * DefaultFileChunkSize);
    QVERIFY(!path.isEmpty());
    ControlledTransferServer server;
    QVERIFY(server.start());
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    QSignalSpy completed(&manager, &TransferManager::uploadCompleted);
    TransferTask *task = manager.addUpload(QStringLiteral("peer"), path);
    QVERIFY(task);
    const QString fileId = QStringLiteral("f-parallel.bin");
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    QCOMPARE(task->totalChunks(), 4);
    QCOMPARE(server.activePutKeys(), QSet<QString>({chunkKey(fileId, 0), chunkKey(fileId, 1),
                                                    chunkKey(fileId, 2)}));
    QCOMPARE(server.initChunkSize(), DefaultFileChunkSize);
    QCOMPARE(QThreadPool::globalInstance()->maxThreadCount(), 4);

    server.respondPut(fileId, 2);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePutKeys().contains(chunkKey(fileId, 3)), 5000);
    QCOMPARE(server.maxActivePuts(), 3);
    QCOMPARE(server.completeRequests(fileId), 0);
    server.respondPut(fileId, 0);
    server.respondPut(fileId, 1);
    QTRY_VERIFY_WITH_TIMEOUT(server.confirmed(fileId).size() == 3, 5000);
    QCOMPARE(server.completeRequests(fileId), 0);
    server.respondPut(fileId, 3);
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 10000);
    QCOMPARE(server.completeRequests(fileId), 1);
    QCOMPARE(server.earlyCompletes(fileId), 0);
    QCOMPARE(completed.size(), 1);
    QCOMPARE(task->transferredBytes(), task->size());
}

void TransferManagerTest::newReadyFilesGetFifoChannelsBeforeLargeBacklog()
{
    QTemporaryDir dir;
    const QString big = createSource(dir.path(), QStringLiteral("big.bin"), 6LL * DefaultFileChunkSize);
    const QString first = createSource(dir.path(), QStringLiteral("first.bin"), 2LL * DefaultFileChunkSize);
    const QString second = createSource(dir.path(), QStringLiteral("second.bin"), DefaultFileChunkSize);
    QVERIFY(!big.isEmpty() && !first.isEmpty() && !second.isEmpty());
    ControlledTransferServer server;
    QVERIFY(server.start());
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *bigTask = manager.addUpload(QStringLiteral("peer"), big);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    TransferTask *firstTask = manager.addUpload(QStringLiteral("peer"), first);
    TransferTask *secondTask = manager.addUpload(QStringLiteral("peer"), second);
    QVERIFY(bigTask && firstTask && secondTask);
    QTRY_VERIFY_WITH_TIMEOUT(server.initRequests() == 3, 10000);
    QCOMPARE(server.activePuts(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(firstTask->state() == TransferTask::State::Uploading
                             && secondTask->state() == TransferTask::State::Uploading, 5000);

    const QString bigId = QStringLiteral("f-big.bin");
    const QString firstId = QStringLiteral("f-first.bin");
    const QString secondId = QStringLiteral("f-second.bin");
    server.respondPut(bigId, 0);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePutKeys().contains(chunkKey(firstId, 0)), 5000);
    QVERIFY(!server.activePutKeys().contains(chunkKey(secondId, 0)));
    QCOMPARE(server.maxActivePuts(), 3);

    // The second new file still precedes both the first file's second chunk and
    // the much larger existing backlog when another channel becomes available.
    server.respondPut(bigId, 1);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePutKeys().contains(chunkKey(secondId, 0)), 5000);
    QCOMPARE(server.attempts(firstId, 1), 0);
    QCOMPARE(server.maxActivePuts(), 3);

    // Once every new file has its first channel, the next free channel returns
    // to the file with the largest remaining byte backlog.
    server.respondPut(secondId, 0);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePutKeys().contains(chunkKey(bigId, 3)), 5000);
    QCOMPARE(server.attempts(firstId, 1), 0);
    QCOMPARE(server.maxActivePuts(), 3);
}

void TransferManagerTest::pauseResumeAbortsAndKeepsConfirmedChunks()
{
    QTemporaryDir dir;
    const QString path = createSource(dir.path(), QStringLiteral("resume.bin"),
                                      4LL * DefaultFileChunkSize);
    ControlledTransferServer server;
    QVERIFY(server.start());
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addUpload(QStringLiteral("peer"), path);
    QVERIFY(task);
    const QString fileId = QStringLiteral("f-resume.bin");
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    server.respondPut(fileId, 0);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePutKeys().contains(chunkKey(fileId, 3)), 5000);
    const QSet<QString> oldOperations = server.activePutKeys();

    manager.pause(task->id());
    QCOMPARE(task->state(), TransferTask::State::Paused);
    manager.resume(task->id()); // Immediate resume waits for every old callback.
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 0, 5000);
    for (const QString &key : oldOperations) {
        const QStringList parts = key.split(QLatin1Char(':'));
        QCOMPARE(server.aborted(parts.first(), parts.last().toInt()), 1);
    }
    const qint64 confirmedBytes = DefaultFileChunkSize;
    QCOMPARE(task->transferredBytes(), confirmedBytes);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 10000);
    QCOMPARE(server.attempts(fileId, 0), 1);
    for (const QString &key : oldOperations) {
        const QStringList parts = key.split(QLatin1Char(':'));
        QTRY_COMPARE_WITH_TIMEOUT(server.attempts(parts.first(), parts.last().toInt()), 2, 5000);
    }
    QVERIFY(server.statusRequests() >= 1);

    // A delayed resume also stays paused until every aborted PUT is released,
    // and an aborted response cannot advance confirmed-byte progress.
    manager.pause(task->id());
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 0, 5000);
    const qint64 pausedBytes = task->transferredBytes();
    QTest::qWait(150);
    QCOMPARE(task->state(), TransferTask::State::Paused);
    QCOMPARE(task->transferredBytes(), pausedBytes);
    manager.resume(task->id());
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 10000);
    QVERIFY(server.statusRequests() >= 2);

    int guard = 0;
    while (task->state() != TransferTask::State::Completed && guard++ < 8) {
        QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() > 0
                                 || task->state() == TransferTask::State::Completed, 5000);
        const QSet<QString> active = server.activePutKeys();
        for (const QString &key : active) {
            const QStringList parts = key.split(QLatin1Char(':'));
            server.respondPut(parts.first(), parts.last().toInt());
        }
    }
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 10000);
    QCOMPARE(server.attempts(fileId, 0), 1);
    QCOMPARE(server.completeRequests(fileId), 1);
    QCOMPARE(task->transferredBytes(), task->size());
}

void TransferManagerTest::cancelAndShutdownAbortAllPuts()
{
    QTemporaryDir dir;
    const QString first = createSource(dir.path(), QStringLiteral("cancel.bin"),
                                       4LL * DefaultFileChunkSize);
    const QString second = createSource(dir.path(), QStringLiteral("close.bin"),
                                        4LL * DefaultFileChunkSize);
    ControlledTransferServer server;
    QVERIFY(server.start());
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    QSignalSpy completed(&manager, &TransferManager::uploadCompleted);
    TransferTask *cancelled = manager.addUpload(QStringLiteral("peer"), first);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    manager.cancel(cancelled->id());
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 0, 5000);
    const QString cancelledId = QStringLiteral("f-cancel.bin");
    QCOMPARE(server.aborted(cancelledId, 0), 1);
    QCOMPARE(server.aborted(cancelledId, 1), 1);
    QCOMPARE(server.aborted(cancelledId, 2), 1);
    QTRY_COMPARE_WITH_TIMEOUT(server.deleteRequests(cancelledId), 1, 5000);
    QCOMPARE(server.completeRequests(cancelledId), 0);

    manager.addUpload(QStringLiteral("peer"), second);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    manager.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 0, 5000);
    const QString closedId = QStringLiteral("f-close.bin");
    QCOMPARE(server.aborted(closedId, 0), 1);
    QCOMPARE(server.aborted(closedId, 1), 1);
    QCOMPARE(server.aborted(closedId, 2), 1);
    QCOMPARE(completed.size(), 0);
}

void TransferManagerTest::instantAndCompletedInitSkipChunkAndCompleteRequests()
{
    QTemporaryDir dir;
    const QString instant = createSource(dir.path(), QStringLiteral("instant.bin"), 123);
    const QString completed = createSource(dir.path(), QStringLiteral("completed.bin"), 456);
    ControlledTransferServer server;
    QVERIFY(server.start());
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    QSignalSpy notifications(&manager, &TransferManager::uploadCompleted);
    TransferTask *first = manager.addUpload(QStringLiteral("peer"), instant);
    TransferTask *second = manager.addUpload(QStringLiteral("peer"), completed);
    QTRY_VERIFY_WITH_TIMEOUT(first->state() == TransferTask::State::Completed
                             && second->state() == TransferTask::State::Completed, 10000);
    QCOMPARE(notifications.size(), 2);
    QCOMPARE(server.activePuts(), 0);
    QCOMPARE(server.maxActivePuts(), 0);
    QCOMPARE(server.completeRequests(QStringLiteral("f-instant.bin")), 0);
    QCOMPARE(server.completeRequests(QStringLiteral("f-completed.bin")), 0);
    QCOMPARE(first->transferredBytes(), first->size());
    QCOMPARE(second->transferredBytes(), second->size());
}

void TransferManagerTest::uploadsAndThreeDownloadsUseIndependentLimits()
{
    QTemporaryDir dir;
    const QString source = createSource(dir.path(), QStringLiteral("independent.bin"),
                                        4LL * DefaultFileChunkSize);
    ControlledTransferServer server;
    QVERIFY(server.start());
    const QByteArray body(256 * 1024, 'd');
    for (int index = 0; index < 4; ++index)
        server.setDownloadBody(QStringLiteral("download-%1").arg(index), body);
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *upload = manager.addUpload(QStringLiteral("peer"), source);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 3, 15000);
    QList<TransferTask *> downloads;
    for (int index = 0; index < 4; ++index) {
        downloads.append(manager.addDownload(QStringLiteral("peer"),
            QStringLiteral("download-%1").arg(index), QStringLiteral("download-%1.bin").arg(index),
            body.size()));
    }
    QTRY_VERIFY_WITH_TIMEOUT(server.activeDownloads() == 3, 10000);
    QCOMPARE(server.activePuts(), 3);
    QCOMPARE(upload->state(), TransferTask::State::Uploading);
    QCOMPARE(downloads.at(3)->state(), TransferTask::State::Queued);
    QCOMPARE(server.maxActiveDownloads(), 3);

    server.respondDownload(QStringLiteral("download-0"));
    QTRY_VERIFY_WITH_TIMEOUT(server.activeDownloadIds().contains(QStringLiteral("download-3")), 5000);
    QCOMPARE(server.activeDownloads(), 3);
    QCOMPARE(server.maxActiveDownloads(), 3);
    int guard = 0;
    while (server.activeDownloads() > 0 && guard++ < 6) {
        const QSet<QString> active = server.activeDownloadIds();
        for (const QString &fileId : active)
            server.respondDownload(fileId);
        QCoreApplication::processEvents();
    }
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(downloads.cbegin(), downloads.cend(), [](TransferTask *task) {
        return task->state() == TransferTask::State::Completed;
    }), 10000);
    QCOMPARE(server.activePuts(), 3); // Upload channels were never consumed by downloads.
    QCOMPARE(upload->state(), TransferTask::State::Uploading);
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

void TransferManagerTest::streamingDownloadRetryUsesStableHandleWithoutMixingErrorBody()
{
    const QByteArray body(256 * 1024 + 17, 'r');
    ControlledTransferServer server;
    QVERIFY(server.start());
    server.setDownloadBody(QStringLiteral("retry-file"), body);
    server.expireDownloadOnce(QStringLiteral("retry-file"));
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    AuthSession session;
    session.accessToken = QStringLiteral("old-access");
    session.refreshToken = QStringLiteral("old-refresh");
    session.user.id = QStringLiteral("u1");
    session.user.username = QStringLiteral("ALICE");
    api.setAuthSession(session);
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("retry-file"),
                                             QStringLiteral("retry.bin"), body.size());
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(server.activeDownloadIds().contains(QStringLiteral("retry-file")), 5000);
    QCOMPARE(server.refreshRequests(), 1);
    server.respondDownload(QStringLiteral("retry-file"));
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 10000);
    QCOMPARE(server.downloadRequests(QStringLiteral("retry-file")), 2);
    QCOMPARE(server.downloadAuthorizations(QStringLiteral("retry-file")),
             QList<QByteArray>({QByteArray("Bearer old-access"), QByteArray("Bearer new-access")}));
    QFile output(task->localPath());
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), body);
}

void TransferManagerTest::cancelWhileWaitingForRefreshDoesNotReplayDownload()
{
    ControlledTransferServer server;
    QVERIFY(server.start());
    server.setDownloadBody(QStringLiteral("cancel-retry"), QByteArray("complete-body"));
    server.expireDownloadOnce(QStringLiteral("cancel-retry"));
    server.holdRefresh();
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    AuthSession session;
    session.accessToken = QStringLiteral("old-access");
    session.refreshToken = QStringLiteral("old-refresh");
    session.user.id = QStringLiteral("u1");
    api.setAuthSession(session);
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("cancel-retry"),
                                             QStringLiteral("cancel.bin"), 13);
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(server.refreshRequests() == 1, 5000);
    manager.cancel(task->id());
    QCOMPARE(task->state(), TransferTask::State::Cancelled);
    server.respondRefresh();
    QTest::qWait(150);
    QCOMPARE(server.downloadRequests(QStringLiteral("cancel-retry")), 1);
    QCOMPARE(server.refreshRequests(), 1);
    QCOMPARE(task->state(), TransferTask::State::Cancelled);
    QVERIFY(!QFileInfo::exists(task->localPath()));
}

void TransferManagerTest::immediateRetryWaitsForCancellationDelete()
{
    QTemporaryDir dir;
    const QString path = createSource(dir.path(), QStringLiteral("retry-delete.bin"),
                                      DefaultFileChunkSize);
    QVERIFY(!path.isEmpty());
    ControlledTransferServer server;
    QVERIFY(server.start());
    const QString fileId = QStringLiteral("f-retry-delete.bin");
    server.holdDelete(fileId);
    LocalStore store(dir.filePath(QStringLiteral("store")));
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *task = manager.addUpload(QStringLiteral("peer"), path);
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(server.activePuts() == 1, 10000);

    manager.cancel(task->id());
    QTRY_COMPARE_WITH_TIMEOUT(server.deleteRequests(fileId), 1, 5000);
    manager.retry(task->id());
    QCOMPARE(task->state(), TransferTask::State::Queued);
    QTest::qWait(150);
    QCOMPARE(server.initRequests(), 1); // init before DELETE completion would conflict with deleting.

    server.respondDelete(fileId);
    QTRY_VERIFY_WITH_TIMEOUT(server.initRequests() == 2 && server.activePuts() == 1, 10000);
    QCOMPARE(server.statusRequests(), 0); // The cancelled fileId was discarded before re-init.
    server.respondPut(fileId, 0);
    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 10000);
    QCOMPARE(server.deleteRequests(fileId), 1);
    QCOMPARE(server.completeRequests(fileId), 1);
}

void TransferManagerTest::windowsDownloadReservationsIgnoreCase()
{
#ifndef Q_OS_WIN
    QSKIP("Windows case-insensitive reservation regression");
#else
    ControlledTransferServer server;
    QVERIFY(server.start());
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    TransferTask *lower = manager.addDownload(QStringLiteral("peer"), QStringLiteral("lower"),
                                              QStringLiteral("report.pdf"), 1);
    TransferTask *upper = manager.addDownload(QStringLiteral("peer"), QStringLiteral("upper"),
                                              QStringLiteral("REPORT.PDF"), 1);
    QVERIFY(lower && upper);
    QVERIFY(lower->localPath().compare(upper->localPath(), Qt::CaseInsensitive) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(server.activeDownloads() == 2, 5000);
#endif
}

void TransferManagerTest::slowWriterBoundsQueueAndAppliesNetworkBackpressure()
{
    const QByteArray body(4 * 1024 * 1024, 'b');
    DownloadServer server(body);
    QVERIFY(server.start());
    QTemporaryDir dir;
    LocalStore store(dir.path());
    QVERIFY(store.initialize());
    ApiClient api;
    api.setServerUrl(server.url());
    TransferManager manager(&api, &store);
    manager.setDownloadWriteDelayForTest(10);
    TransferTask *task = manager.addDownload(QStringLiteral("peer"), QStringLiteral("file"),
                                             QStringLiteral("backpressure.bin"), body.size());
    QVERIFY(task);
    QTRY_VERIFY_WITH_TIMEOUT(
        task->peakQueuedDownloadBytes() >= Workers::AsyncFileWriter::MaxQueuedBytes, 10000);
    QCOMPARE(task->peakQueuedDownloadBytes(), Workers::AsyncFileWriter::MaxQueuedBytes);
    QVERIFY(task->queuedDownloadBytes() <= Workers::AsyncFileWriter::MaxQueuedBytes);
    QVERIFY(task->isDownloadNetworkReadPaused());
    QCOMPARE(task->state(), TransferTask::State::Downloading); // Not a user pause.

    QTRY_VERIFY_WITH_TIMEOUT(task->state() == TransferTask::State::Completed, 15000);
    QFile output(task->localPath());
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), body);
}

QTEST_GUILESS_MAIN(TransferManagerTest)
#include "test_transfer_manager.moc"
