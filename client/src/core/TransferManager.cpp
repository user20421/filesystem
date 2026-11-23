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

int TransferManager::activeUploadChannels() const
{
    int result = 0;
    for (TransferTask *task : m_tasks)
        result += task->m_inflightChunks.size();
    return result;
}

int TransferManager::nextMissingChunk(TransferTask *task)
{
    for (int index = 0; index < task->m_totalChunks; ++index) {
        if (!task->m_uploadedChunks.contains(index) && !task->m_inflightChunks.contains(index))
            return index;
    }
    return -1;
}

// selectUploadTask 先让尚未获得通道的新文件按 FIFO 开始，其余空闲通道分配给剩余数据最多的文件。
TransferTask *TransferManager::selectUploadTask() const
{
    // A newly ready file gets its first channel in FIFO order.  Only after all
    // such files have started do remaining channels go to the largest backlog.
    for (TransferTask *task : m_tasks) {
        if (task->m_state == TransferTask::State::Uploading
            && task->m_waitingForFirstChannel && nextMissingChunk(task) >= 0)
            return task;
    }
    TransferTask *best = nullptr;
    qint64 bestRemaining = -1;
    for (TransferTask *task : m_tasks) {
        if (task->m_state != TransferTask::State::Uploading || nextMissingChunk(task) < 0)
            continue;
        const qint64 remaining = task->m_size - task->m_transferred;
        if (!best || remaining > bestRemaining) {
            best = task;
            bestRemaining = remaining;
        }
    }
    return best;
}

void TransferManager::pump()
{
    if (m_closing)
        return;
    for (TransferTask *task : m_tasks) {
        if (task->m_state != TransferTask::State::Queued || hasActiveWork(task))
            continue;
        if (task->direction() == TransferTask::Direction::Download
            && m_activeDownloads.size() >= DownloadChannelCount)
            continue;
        startTask(task);
    }
    while (activeUploadChannels() < UploadChannelCount) {
        TransferTask *task = selectUploadTask();
        if (!task)
            break;
        dispatchChunk(task);
    }
    for (TransferTask *task : m_tasks)
        maybeCompleteUpload(task);
}

void TransferManager::startTask(TransferTask *task)
{
    task->m_error.clear();
    if (task->direction() == TransferTask::Direction::Upload) {
        startUpload(task);
    } else {
        m_activeDownloads.insert(task->id());
        startDownload(task);
    }
    notify(task);
}

void TransferManager::startUpload(TransferTask *task)
{
    task->m_state = TransferTask::State::Preparing;
    if (!task->m_fileId.isEmpty()) {
        refreshUploadStatus(task);
        return;
    }
    QPointer<TransferTask> guarded(task);
    const quint64 generation = task->m_generation;
    task->m_hashPending = true;
    Workers::hashFile(task->m_localPath, this,
                      [this, guarded, generation](bool ok, const QString &digest, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        task->m_hashPending = false;
        if (generation != task->m_generation || task->m_state != TransferTask::State::Preparing) {
            settle(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        task->m_reply = m_api->initFile(task->m_peerId, task->m_fileName, task->m_size, digest,
            [this, guarded, generation](bool success, const FileInitInfo &info, const QString &apiError) {
            if (!guarded)
                return;
            TransferTask *task = guarded.data();
            task->m_reply.clear();
            if (generation != task->m_generation || task->m_state != TransferTask::State::Preparing) {
                settle(task);
                return;
            }
            if (!success) {
                fail(task, apiError);
                return;
            }
            task->m_fileId = info.fileId;
            task->m_chunkSize = qBound(1, info.chunkSize, 64 * 1024 * 1024);
            task->m_uploadedChunks = info.uploadedChunks;
            task->m_totalChunks = task->m_size == 0 ? 0
                : static_cast<int>((task->m_size + task->m_chunkSize - 1) / task->m_chunkSize);
            updateUploadedProgress(task);
            if (info.complete) {
                finishUpload(task);
                return;
            }
            task->m_state = TransferTask::State::Uploading;
            notify(task);
            pump();
        });
        if (!task->m_reply)
            fail(task, QStringLiteral("无法创建上传初始化请求"));
    });
}

void TransferManager::refreshUploadStatus(TransferTask *task)
{
    QPointer<TransferTask> guarded(task);
    const quint64 generation = task->m_generation;
    task->m_reply = m_api->fileStatus(task->m_fileId,
        [this, guarded, generation](bool ok, const FileStatus &status, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        task->m_reply.clear();
        if (generation != task->m_generation || task->m_state != TransferTask::State::Preparing) {
            settle(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        task->m_chunkSize = qBound(1, status.chunkSize, 64 * 1024 * 1024);
        task->m_uploadedChunks = status.uploadedChunks;
        task->m_totalChunks = task->m_size == 0 ? 0
            : static_cast<int>((task->m_size + task->m_chunkSize - 1) / task->m_chunkSize);
        updateUploadedProgress(task);
        if (status.complete) {
            finishUpload(task);
            return;
        }
        task->m_state = TransferTask::State::Uploading;
        notify(task);
        pump();
    });
    if (!task->m_reply)
        fail(task, QStringLiteral("无法创建上传状态请求"));
}

void TransferManager::updateUploadedProgress(TransferTask *task)
{
    task->m_transferred = 0;
    for (int index = 0; index < task->m_totalChunks; ++index) {
        if (task->m_uploadedChunks.contains(index)) {
            const qint64 offset = static_cast<qint64>(index) * task->m_chunkSize;
            task->m_transferred += qMin<qint64>(task->m_chunkSize, task->m_size - offset);
        }
    }
}

// dispatchChunk 在异步读盘前就占用上传通道，并记录分块 generation，避免暂停或取消后重复分配与回调。
void TransferManager::dispatchChunk(TransferTask *task)
{
    const int index = nextMissingChunk(task);
    if (index < 0)
        return;
    task->m_waitingForFirstChannel = false;
    task->m_inflightChunks.insert(index);
    const qint64 offset = static_cast<qint64>(index) * task->m_chunkSize;
    const int bytes = static_cast<int>(qMin<qint64>(task->m_chunkSize, task->m_size - offset));
    QPointer<TransferTask> guarded(task);
    const quint64 generation = task->m_generation;
    Workers::readChunk(task->m_localPath, offset, bytes, this,
        [this, guarded, generation, index](bool ok, const QByteArray &data, const QString &digest,
                                           const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (!task->m_inflightChunks.contains(index))
            return; // A duplicate worker callback cannot release another operation.
        if (generation != task->m_generation || task->m_state != TransferTask::State::Uploading) {
            task->m_inflightChunks.remove(index);
            task->m_chunkReplies.remove(index);
            updateUploadedProgress(task);
            settle(task);
            return;
        }
        if (!ok) {
            task->m_inflightChunks.remove(index);
            fail(task, error);
            return;
        }
        task->m_chunkReplies.insert(index, m_api->putChunk(task->m_fileId, index, data, digest,
            [this, guarded, generation, index](bool uploaded, const QString &apiError) {
            if (!guarded)
                return;
            TransferTask *task = guarded.data();
            if (!task->m_inflightChunks.contains(index))
                return;
            task->m_chunkReplies.remove(index);
            task->m_inflightChunks.remove(index);
            if (generation != task->m_generation || task->m_state != TransferTask::State::Uploading) {
                updateUploadedProgress(task);
                settle(task);
                return;
            }
            if (!uploaded) {
                fail(task, apiError);
                return;
            }
            task->m_uploadedChunks.insert(index);
            updateUploadedProgress(task);
            notify(task);
            maybeCompleteUpload(task);
            settle(task);
        }));
        if (!task->m_chunkReplies.value(index)) {
            task->m_inflightChunks.remove(index);
            task->m_chunkReplies.remove(index);
            fail(task, QStringLiteral("无法创建分块上传请求"));
        }
    });
}

void TransferManager::maybeCompleteUpload(TransferTask *task)
{
    if (!task || task->direction() != TransferTask::Direction::Upload
        || task->m_state != TransferTask::State::Uploading || task->m_completePending
        || task->m_reply || !task->m_inflightChunks.isEmpty())
        return;
    for (int index = 0; index < task->m_totalChunks; ++index) {
        if (!task->m_uploadedChunks.contains(index))
            return;
    }
    QPointer<TransferTask> guarded(task);
    const quint64 generation = task->m_generation;
    task->m_completePending = true;
    task->m_reply = m_api->completeFile(task->m_fileId,
        [this, guarded, generation](bool ok, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        task->m_reply.clear();
        task->m_completePending = false;
        if (generation != task->m_generation || task->m_state != TransferTask::State::Uploading) {
            settle(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        finishUpload(task);
    });
    if (!task->m_reply) {
        task->m_completePending = false;
        fail(task, QStringLiteral("无法创建上传完成请求"));
    }
}

void TransferManager::finishUpload(TransferTask *task)
{
    if (task->m_completionEmitted)
        return;
    task->m_completionEmitted = true;
    task->m_transferred = task->m_size;
    task->m_state = TransferTask::State::Completed;
    task->m_completePending = false;
    notify(task);
    settle(task);
    if (!m_closing)
        emit uploadCompleted(task);
}

void TransferManager::startDownload(TransferTask *task)
{
    task->m_state = TransferTask::State::Downloading;
    task->m_transferred = 0;
    discardWriter(task);
    QPointer<TransferTask> guarded(task);
    const quint64 generation = task->m_generation;
    const qint64 expectedSize = task->m_size > 0 ? task->m_size : -1;
    task->m_networkReadPaused = false;
    task->m_writer = new Workers::AsyncFileWriter(task->m_localPath, expectedSize, this,
        [this, guarded, generation](bool ok, qint64 bytes, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (generation != task->m_generation
            || task->m_state == TransferTask::State::Cancelled
            || task->m_state == TransferTask::State::Paused) {
            settle(task);
            return;
        }
        if (task->m_state == TransferTask::State::Failed)
            return;
        if (!ok) {
            if (task->m_reply)
                task->m_reply->abort();
            fail(task, error);
            return;
        }
        task->m_size = bytes;
        task->m_transferred = bytes;
        discardWriter(task);
        task->m_state = TransferTask::State::Completed;
        notify(task);
        settle(task);
    }, task, m_downloadWriteDelayMs);
    connect(task->m_writer, &Workers::AsyncFileWriter::queuedBytesChanged, this,
            [this, guarded, generation](qint64) {
        if (guarded && generation == guarded->m_generation)
            drainDownload(guarded.data());
    });

    task->m_reply = m_api->downloadFile(task->m_fileId,
        [this, guarded, generation](bool ok, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        ApiRequestHandle *request = task->m_reply.data();
        if (ok && request && task->m_writer && task->m_state == TransferTask::State::Downloading)
            drainDownload(task);
        task->m_reply.clear();
        if (generation != task->m_generation
            || task->m_state == TransferTask::State::Cancelled
            || task->m_state == TransferTask::State::Paused
            || task->m_state == TransferTask::State::Failed) {
            settle(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        if (task->m_writer)
            task->m_writer->finish();
    }, [this, guarded, generation](qint64 received, qint64) {
        if (guarded && generation == guarded->m_generation
            && guarded->m_state == TransferTask::State::Downloading) {
            guarded->m_transferred = received;
            notify(guarded);
        }
    });
    ApiRequestHandle *request = task->m_reply.data();
    if (!request) {
        fail(task, QStringLiteral("无法创建下载请求"));
        return;
    }
    connect(request, &ApiRequestHandle::readyRead, this, [this, guarded, generation] {
        if (guarded && generation == guarded->m_generation)
            drainDownload(guarded.data());
    });
    drainDownload(task);
}

// drainDownload 按顺序写盘：队列达到 1 MiB 暂停网络读取，降到半水位后恢复；这与用户主动暂停是两种独立状态。
void TransferManager::drainDownload(TransferTask *task)
{
    if (!task || task->m_state != TransferTask::State::Downloading
        || !task->m_writer || !task->m_reply)
        return;
    ApiRequestHandle *request = task->m_reply.data();
    while (task->m_writer && request->bytesAvailable() > 0
           && task->m_writer->availableBytes() > 0) {
        const qint64 amount = qMin<qint64>(64 * 1024,
            qMin(request->bytesAvailable(), task->m_writer->availableBytes()));
        const QByteArray data = request->read(amount);
        if (data.isEmpty() || !task->m_writer->append(data))
            break;
    }
    if (!task->m_writer || !task->m_reply)
        return;

    // Network flow control is independent of the user-visible Paused state.
    // QNetworkReply's bounded read buffer stops the socket once we stop
    // draining it; resume only after the writer has consumed half the queue.
    const qint64 queued = task->m_writer->queuedBytes();
    if (!task->m_networkReadPaused && queued >= Workers::AsyncFileWriter::MaxQueuedBytes) {
        task->m_networkReadPaused = true;
        request->setReadPaused(true);
    } else if (task->m_networkReadPaused
               && queued <= Workers::AsyncFileWriter::MaxQueuedBytes / 2) {
        task->m_networkReadPaused = false;
        request->setReadPaused(false);
    }
}

void TransferManager::abortRequests(TransferTask *task)
{
    if (task->m_reply)
        task->m_reply->abort();
    const auto requests = task->m_chunkReplies;
    for (const QPointer<ApiRequestHandle> &request : requests) {
        if (request)
            request->abort();
    }
}

bool TransferManager::hasActiveWork(TransferTask *task) const
{
    if (!task)
        return false;
    if (task->direction() == TransferTask::Direction::Upload)
        return task->m_hashPending || task->m_reply || task->m_deleteReply
            || !task->m_inflightChunks.isEmpty();
    return task->m_reply || task->m_writer;
}

void TransferManager::pause(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state == TransferTask::State::Completed
        || task->m_state == TransferTask::State::Failed
        || task->m_state == TransferTask::State::Cancelled)
        return;
    ++task->m_generation;
    task->m_resumePending = false;
    task->m_state = TransferTask::State::Paused;
    task->m_completePending = false;
    if (task->direction() == TransferTask::Direction::Download)
        discardWriter(task);
    abortRequests(task);
    notify(task);
    settle(task);
}

void TransferManager::resume(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state != TransferTask::State::Paused)
        return;
    if (hasActiveWork(task)) {
        task->m_resumePending = true;
        return;
    }
    task->m_state = TransferTask::State::Queued;
    task->m_waitingForFirstChannel = true;
    notify(task);
    pump();
}

void TransferManager::retry(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || (task->m_state != TransferTask::State::Failed
                  && task->m_state != TransferTask::State::Cancelled))
        return;
    const bool wasCancelled = task->m_state == TransferTask::State::Cancelled;
    ++task->m_generation;
    task->m_resumePending = false;
    task->m_completePending = false;
    task->m_cancellationFinalized = false;
    task->m_waitingForFirstChannel = true;
    task->m_error.clear();
    task->m_state = TransferTask::State::Queued;
    if (wasCancelled && task->direction() == TransferTask::Direction::Upload) {
        if (task->m_deleteReply)
            task->m_retryAfterDelete = true;
        else
            resetCancelledUpload(task);
    }
    if (task->direction() == TransferTask::Direction::Download) {
        task->m_transferred = 0;
        if (QFileInfo::exists(task->m_localPath)) {
            task->m_localPath = m_store->uniqueDownloadPath(task->m_fileName);
            m_reservedDownloads.insert(downloadReservationKey(task->m_localPath));
        }
    }
    notify(task);
    settle(task);
}

void TransferManager::cancel(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state == TransferTask::State::Completed
        || task->m_state == TransferTask::State::Cancelled)
        return;
    ++task->m_generation;
    task->m_resumePending = false;
    task->m_completePending = false;
    task->m_state = TransferTask::State::Cancelled;
    if (task->direction() == TransferTask::Direction::Download)
        discardWriter(task);
    abortRequests(task);
    notify(task);
    settle(task);
}

void TransferManager::fail(TransferTask *task, const QString &error)
{
    ++task->m_generation;
    task->m_resumePending = false;
    task->m_completePending = false;
    task->m_error = error.isEmpty() ? QStringLiteral("未知传输错误") : error;
    task->m_state = TransferTask::State::Failed;
    if (task->direction() == TransferTask::Direction::Download)
        discardWriter(task);
    abortRequests(task);
    notify(task);
    settle(task);
}

void TransferManager::settle(TransferTask *task)
{
    if (!task || m_closing)
        return;
    if (!hasActiveWork(task)) {
        if (task->direction() == TransferTask::Direction::Download)
            m_activeDownloads.remove(task->id());
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused && task->m_resumePending) {
            task->m_resumePending = false;
            task->m_state = TransferTask::State::Queued;
            task->m_waitingForFirstChannel = true;
            notify(task);
        }
    }
    pump();
}

void TransferManager::resetCancelledUpload(TransferTask *task)
{
    task->m_retryAfterDelete = false;
    task->m_fileId.clear();
    task->m_uploadedChunks.clear();
    task->m_transferred = 0;
}

// finishCancellation 等待旧分块和 DELETE 全部结束后才允许立即重试，避免新旧上传共享同一内容时发生清理竞态。
void TransferManager::finishCancellation(TransferTask *task)
{
    // 取消上传先等待旧分块回调全部退出，再删除服务端引用；立即重试会等删除完成后才重置 fileId。
    if (hasActiveWork(task))
        return;
    if (!task->m_cancellationFinalized) {
        task->m_cancellationFinalized = true;
        if (task->direction() == TransferTask::Direction::Upload && !task->m_fileId.isEmpty()) {
            const QString fileId = task->m_fileId;
            QPointer<TransferTask> guarded(task);
            task->m_deleteReply = m_api->deleteFile(fileId,
                [this, guarded](bool, const QString &) {
                if (!guarded)
                    return;
                TransferTask *task = guarded.data();
                task->m_deleteReply.clear();
                if (task->m_retryAfterDelete && !task->m_deleteReply
                    && task->m_state == TransferTask::State::Queued)
                    resetCancelledUpload(task);
                settle(task);
            });
            if (!task->m_deleteReply && task->m_retryAfterDelete)
                resetCancelledUpload(task);
        }
    }
    pump();
}

void TransferManager::discardWriter(TransferTask *task)
{
    if (!task || !task->m_writer)
        return;
    auto *writer = task->m_writer;
    task->m_writer = nullptr;
    delete writer;
}
