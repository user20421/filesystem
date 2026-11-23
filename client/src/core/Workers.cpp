#include "Workers.h"

#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QSaveFile>
#include <QThread>
#include <QThreadPool>

namespace {
QThreadPool *fileWorkerPool()
{
    static const bool configured = [] {
        QThreadPool::globalInstance()->setMaxThreadCount(4);
        return true;
    }();
    Q_UNUSED(configured);
    return QThreadPool::globalInstance();
}

template<typename Result>
void deliver(QObject *receiver, Result callback)
{
    QPointer<QObject> guarded(receiver);
    QMetaObject::invokeMethod(receiver, [guarded, callback = std::move(callback)]() mutable {
        if (guarded)
            callback();
    }, Qt::QueuedConnection);
}

class HashTask final : public QRunnable {
public:
    HashTask(QString path, QObject *receiver, Workers::HashCallback callback)
        : m_path(std::move(path)), m_receiver(receiver), m_callback(std::move(callback)) {}
    void run() override
    {
        QFile file(m_path);
        QString error;
        if (!file.open(QIODevice::ReadOnly))
            error = file.errorString();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (error.isEmpty()) {
            const QByteArray block = file.read(1024 * 1024);
            if (block.isEmpty())
                break;
            hash.addData(block);
        }
        if (error.isEmpty() && file.error() != QFileDevice::NoError)
            error = file.errorString();
        const QString digest = error.isEmpty() ? QString::fromLatin1(hash.result().toHex()) : QString{};
        deliver(m_receiver, [callback = m_callback, error, digest]() mutable {
            callback(error.isEmpty(), digest, error);
        });
    }
private:
    QString m_path;
    QObject *m_receiver;
    Workers::HashCallback m_callback;
};

class ReadChunkTask final : public QRunnable {
public:
    ReadChunkTask(QString path, qint64 offset, int size, QObject *receiver,
                  Workers::ChunkCallback callback)
        : m_path(std::move(path)), m_offset(offset), m_size(size), m_receiver(receiver),
          m_callback(std::move(callback)) {}
    void run() override
    {
        QFile file(m_path);
        QByteArray data;
        QString error;
        if (!file.open(QIODevice::ReadOnly))
            error = file.errorString();
        if (error.isEmpty() && !file.seek(m_offset))
            error = file.errorString();
        if (error.isEmpty()) {
            data = file.read(m_size);
            if (data.isEmpty() && m_size > 0)
                error = QStringLiteral("读取分块失败或文件已改变");
        }
        const QString digest = error.isEmpty()
            ? QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())
            : QString{};
        deliver(m_receiver, [callback = m_callback, data, digest, error]() mutable {
            callback(error.isEmpty(), data, digest, error);
        });
    }
private:
    QString m_path;
    qint64 m_offset;
    int m_size;
    QObject *m_receiver;
    Workers::ChunkCallback m_callback;
};

class FileWriterWorker final : public QObject {
    Q_OBJECT
public:
    FileWriterWorker(const QString &path, qint64 expectedSize, int writeDelayMs)
        : m_file(path), m_expectedSize(expectedSize), m_writeDelayMs(writeDelayMs) {}

    void open()
    {
        if (!m_file.open(QIODevice::WriteOnly)) {
            fail(m_file.errorString());
            return;
        }
        m_open = true;
    }

    void append(const QByteArray &data)
    {
        if (!m_open || m_failed)
            return;
        if (m_writeDelayMs > 0)
            QThread::msleep(static_cast<unsigned long>(m_writeDelayMs));
        if (m_file.write(data) != data.size()) {
            fail(m_file.errorString());
            return;
        }
        m_bytes += data.size();
        emit written(data.size());
    }

    void finish()
    {
        if (!m_open || m_failed || m_notified)
            return;
        if (m_expectedSize >= 0 && m_bytes != m_expectedSize) {
            fail(QStringLiteral("下载大小不匹配：期望 %1 字节，实际 %2 字节")
                .arg(m_expectedSize).arg(m_bytes));
            return;
        }
        if (!m_file.commit()) {
            fail(m_file.errorString());
            return;
        }
        m_open = false;
        m_notified = true;
        emit completed(true, m_bytes, {});
    }

    void cancel()
    {
        m_failed = true;
        if (m_open)
            m_file.cancelWriting();
        m_open = false;
    }

signals:
    void written(qint64 bytes);
    void completed(bool ok, qint64 bytes, const QString &error);

private:
    void fail(const QString &error)
    {
        m_failed = true;
        if (m_open)
            m_file.cancelWriting();
        m_open = false;
        if (!m_notified) {
            m_notified = true;
            emit completed(false, m_bytes, error.isEmpty() ? QStringLiteral("文件写入失败") : error);
        }
    }

    QSaveFile m_file;
    qint64 m_expectedSize;
    int m_writeDelayMs = 0;
    qint64 m_bytes = 0;
    bool m_open = false;
    bool m_failed = false;
    bool m_notified = false;
};
} // namespace

Workers::AsyncFileWriter::AsyncFileWriter(const QString &path, qint64 expectedSize,
                                          QObject *receiver, StreamWriteCallback callback,
                                          QObject *parent, int writeDelayMs)
    : QObject(parent), m_thread(new QThread), m_receiver(receiver),
      m_callback(std::move(callback))
{
    auto *worker = new FileWriterWorker(path, expectedSize, writeDelayMs);
    m_worker = worker;
    worker->moveToThread(m_thread);
    connect(worker, &FileWriterWorker::written, this, [this](qint64 bytes) {
        m_queuedBytes = qMax<qint64>(0, m_queuedBytes - bytes);
        emit queuedBytesChanged(m_queuedBytes);
    });
    connect(worker, &FileWriterWorker::completed, this,
            [this](bool ok, qint64 bytes, const QString &error) {
        if (!m_receiver)
            return;
        QPointer<QObject> receiver = m_receiver;
        auto callback = m_callback;
        QMetaObject::invokeMethod(receiver, [receiver, callback, ok, bytes, error]() mutable {
            if (receiver)
                callback(ok, bytes, error);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
    QMetaObject::invokeMethod(worker, &FileWriterWorker::open, Qt::QueuedConnection);
}

Workers::AsyncFileWriter::~AsyncFileWriter()
{
    if (!m_thread)
        return;
    // Stop delivery before cancelling: a writer error may synchronously cause
    // TransferManager to discard this same object.
    disconnect(m_worker, nullptr, this, nullptr);
    m_callback = {};
    m_receiver.clear();
    if (m_thread->isRunning()) {
        auto *worker = static_cast<FileWriterWorker *>(m_worker);
        QMetaObject::invokeMethod(worker, [worker] { worker->cancel(); },
                                  Qt::BlockingQueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(5000)) {
            m_thread->requestInterruption();
            m_thread->wait(1000);
        }
    }
    delete m_worker;
    m_worker = nullptr;
    delete m_thread;
    m_thread = nullptr;
}

bool Workers::AsyncFileWriter::append(const QByteArray &data)
{
    if (data.isEmpty())
        return true;
    if (!m_thread->isRunning() || data.size() > availableBytes())
        return false;
    m_queuedBytes += data.size();
    m_peakQueuedBytes = qMax(m_peakQueuedBytes, m_queuedBytes);
    auto *worker = static_cast<FileWriterWorker *>(m_worker);
    QMetaObject::invokeMethod(worker, [worker, data] { worker->append(data); },
                              Qt::QueuedConnection);
    return true;
}

void Workers::AsyncFileWriter::finish()
{
    if (m_thread->isRunning()) {
        auto *worker = static_cast<FileWriterWorker *>(m_worker);
        QMetaObject::invokeMethod(worker, [worker] { worker->finish(); }, Qt::QueuedConnection);
    }
}

void Workers::hashFile(const QString &path, QObject *receiver, HashCallback callback)
{
    fileWorkerPool()->start(new HashTask(path, receiver, std::move(callback)));
}

void Workers::readChunk(const QString &path, qint64 offset, int size, QObject *receiver,
                        ChunkCallback callback)
{
    fileWorkerPool()->start(new ReadChunkTask(path, offset, size, receiver,
                                               std::move(callback)));
}

#include "Workers.moc"
