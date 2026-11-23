// 传输任务与 Manager 生命周期；任务创建、查询及关闭入口保留在这里。
#include "TransferManager.h"

#include "LocalStore.h"
#include "Workers.h"

#include <QFileInfo>
#include <QUuid>

TransferTask::TransferTask(Direction direction, QObject *parent)
    : QObject(parent), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_direction(direction) {}
qint64 TransferTask::queuedDownloadBytes() const
{
    return m_writer ? m_writer->queuedBytes() : 0;
}

qint64 TransferTask::peakQueuedDownloadBytes() const
{
    return m_writer ? m_writer->peakQueuedBytes() : 0;
}

QString TransferTask::stateText() const
{
    switch (m_state) {
    case State::Queued: return QStringLiteral("排队");
    case State::Preparing: return QStringLiteral("准备");
    case State::Uploading: return QStringLiteral("上传");
    case State::Downloading: return QStringLiteral("下载");
    case State::Paused: return QStringLiteral("已暂停");
    case State::Completed: return QStringLiteral("已完成");
    case State::Failed: return QStringLiteral("失败");
    case State::Cancelled: return QStringLiteral("已取消");
    }
    return {};
}

TransferManager::TransferManager(ApiClient *api, LocalStore *store, QObject *parent)
    : QObject(parent), m_api(api), m_store(store) {}
TransferManager::~TransferManager()
{
    shutdown();
}

void TransferManager::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    const QList<TransferTask *> tasks = m_tasks;
    for (TransferTask *task : tasks) {
        ++task->m_generation;
        task->m_resumePending = false;
        task->m_hashPending = false;
        task->m_completePending = false;
        task->m_retryAfterDelete = false;
        task->m_networkReadPaused = false;
        if (task->m_reply) {
            ApiRequestHandle *request = task->m_reply.data();
            task->m_reply.clear();
            request->abort();
        }
        const auto requests = task->m_chunkReplies;
        for (const QPointer<ApiRequestHandle> &guarded : requests) {
            if (ApiRequestHandle *request = guarded.data())
                request->abort();
        }
        task->m_chunkReplies.clear();
        task->m_inflightChunks.clear();
        if (task->m_deleteReply) {
            ApiRequestHandle *request = task->m_deleteReply.data();
            task->m_deleteReply.clear();
            request->abort();
        }
        discardWriter(task);
        task->m_state = TransferTask::State::Cancelled;
    }
    m_activeDownloads.clear();
}

QString TransferManager::downloadReservationKey(const QString &path)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    return clean.toCaseFolded();
#else
    return clean;
#endif
}

TransferTask *TransferManager::addUpload(const QString &peerId, const QString &sourcePath)
{
    if (m_closing)
        return nullptr;
    const QFileInfo info(sourcePath);
    if (!info.isFile() || !info.isReadable())
        return nullptr;
    auto *task = new TransferTask(TransferTask::Direction::Upload, this);
    task->m_peerId = peerId;
    task->m_fileName = LocalStore::sanitizeFileName(info.fileName());
    task->m_localPath = info.absoluteFilePath();
    task->m_size = info.size();
    m_tasks.append(task);
    emit taskAdded(task);
    pump();
    return task;
}

TransferTask *TransferManager::addDownload(const QString &peerId, const QString &fileId,
                                           const QString &fileName, qint64 size)
{
    if (m_closing || fileId.isEmpty())
        return nullptr;
    auto *task = new TransferTask(TransferTask::Direction::Download, this);
    task->m_peerId = peerId;
    task->m_fileId = fileId;
    task->m_fileName = LocalStore::sanitizeFileName(fileName);
    task->m_size = size;
    task->m_localPath = m_store->uniqueDownloadPath(task->m_fileName);
    if (m_reservedDownloads.contains(downloadReservationKey(task->m_localPath))) {
        task->m_localPath = m_store->uniqueDownloadPath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-') + task->m_fileName);
    }
    m_reservedDownloads.insert(downloadReservationKey(task->m_localPath));
    m_tasks.append(task);
    emit taskAdded(task);
    pump();
    return task;
}

TransferTask *TransferManager::task(const QString &id) const
{
    for (TransferTask *task : m_tasks) {
        if (task->id() == id)
            return task;
    }
    return nullptr;
}
