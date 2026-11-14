#include "TransferManager.h"

#include "LocalStore.h"
#include "Workers.h"

#include <QFileInfo>
#include <QUuid>

TransferTask::TransferTask(Direction direction, QObject *parent)
    : QObject(parent), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_direction(direction) {}

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
    : QObject(parent), m_api(api), m_store(store), m_concurrency(store->concurrency()) {}

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
        task->m_resumePending = false;
        if (task->m_reply) {
            QNetworkReply *reply = task->m_reply.data();
            task->m_reply.clear();
            reply->disconnect();
            reply->abort();
            reply->deleteLater();
        }
        discardWriter(task);
        task->m_state = TransferTask::State::Cancelled;
    }
    m_active.clear();
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
    if (m_reservedDownloads.contains(task->m_localPath)) {
        task->m_localPath = m_store->uniqueDownloadPath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('-') + task->m_fileName);
    }
    m_reservedDownloads.insert(task->m_localPath);
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

void TransferManager::setConcurrency(int value)
{
    m_concurrency = qBound(3, value, 5);
    m_store->setConcurrency(m_concurrency);
    pump();
}

void TransferManager::pump()
{
    if (m_closing)
        return;
    for (TransferTask *task : m_tasks) {
        if (m_active.size() >= m_concurrency)
            break;
        if (task->m_state == TransferTask::State::Queued)
            startTask(task);
    }
}

void TransferManager::startTask(TransferTask *task)
{
    if (m_closing || m_active.contains(task->id()))
        return;
    m_active.insert(task->id());
    task->m_error.clear();
    if (task->direction() == TransferTask::Direction::Upload)
        startUpload(task);
    else
        startDownload(task);
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
    Workers::hashFile(task->m_localPath, this,
                      [this, guarded](bool ok, const QString &digest, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused) {
            release(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        task->m_sha256 = digest;
        task->m_reply = m_api->initFile(task->m_peerId, task->m_fileName, task->m_size, digest,
            [this, guarded](bool success, const FileInitInfo &info, const QString &apiError) {
            if (!guarded)
                return;
            TransferTask *task = guarded.data();
            task->m_reply.clear();
            if (success) {
                task->m_fileId = info.fileId;
                task->m_chunkSize = qBound(1, info.chunkSize, 64 * 1024 * 1024);
                task->m_uploadedChunks = info.uploadedChunks;
            }
            if (task->m_state == TransferTask::State::Cancelled) {
                finishCancellation(task);
                return;
            }
            if (task->m_state == TransferTask::State::Paused) {
                release(task);
                return;
            }
            if (!success) {
                fail(task, apiError);
                return;
            }
            task->m_totalChunks = task->m_size == 0 ? 0
                : static_cast<int>((task->m_size + task->m_chunkSize - 1) / task->m_chunkSize);
            updateUploadedProgress(task);
            task->m_state = TransferTask::State::Uploading;
            notify(task);
            sendNextChunk(task);
        });
    });
}

void TransferManager::refreshUploadStatus(TransferTask *task)
{
    QPointer<TransferTask> guarded(task);
    task->m_reply = m_api->fileStatus(task->m_fileId,
        [this, guarded](bool ok, const FileStatus &status, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        task->m_reply.clear();
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused) {
            release(task);
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
            task->m_state = TransferTask::State::Completed;
            task->m_transferred = task->m_size;
            notify(task);
            release(task);
            emit uploadCompleted(task);
            return;
        }
        task->m_state = TransferTask::State::Uploading;
        notify(task);
        sendNextChunk(task);
    });
}

void TransferManager::updateUploadedProgress(TransferTask *task)
{
    task->m_nextChunk = 0;
    task->m_transferred = 0;
    for (int index = 0; index < task->m_totalChunks; ++index) {
        if (task->m_uploadedChunks.contains(index)) {
            const qint64 offset = static_cast<qint64>(index) * task->m_chunkSize;
            task->m_transferred += qMin<qint64>(task->m_chunkSize, task->m_size - offset);
        }
    }
    // Zero is a valid first chunk; find the actual first missing chunk explicitly.
    while (task->m_nextChunk < task->m_totalChunks
           && task->m_uploadedChunks.contains(task->m_nextChunk))
        ++task->m_nextChunk;
}

void TransferManager::sendNextChunk(TransferTask *task)
{
    if (task->m_state == TransferTask::State::Paused
        || task->m_state == TransferTask::State::Cancelled) {
        release(task);
        return;
    }
    while (task->m_nextChunk < task->m_totalChunks
           && task->m_uploadedChunks.contains(task->m_nextChunk))
        ++task->m_nextChunk;
    if (task->m_nextChunk >= task->m_totalChunks) {
        QPointer<TransferTask> guarded(task);
        task->m_reply = m_api->completeFile(task->m_fileId,
            [this, guarded](bool ok, const QString &error) {
            if (!guarded)
                return;
            TransferTask *task = guarded.data();
            task->m_reply.clear();
            if (task->m_state == TransferTask::State::Cancelled) {
                finishCancellation(task);
                return;
            }
            if (task->m_state == TransferTask::State::Paused) {
                release(task);
                return;
            }
            if (!ok) {
                fail(task, error);
                return;
            }
            task->m_transferred = task->m_size;
            task->m_state = TransferTask::State::Completed;
            notify(task);
            release(task);
            emit uploadCompleted(task);
        });
        return;
    }

    const int index = task->m_nextChunk;
    const qint64 offset = static_cast<qint64>(index) * task->m_chunkSize;
    const int bytes = static_cast<int>(qMin<qint64>(task->m_chunkSize, task->m_size - offset));
    QPointer<TransferTask> guarded(task);
    Workers::readChunk(task->m_localPath, offset, bytes, this,
        [this, guarded, index](bool ok, const QByteArray &data, const QString &digest,
                               const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused) {
            release(task);
            return;
        }
        if (!ok) {
            fail(task, error);
            return;
        }
        task->m_reply = m_api->putChunk(task->m_fileId, index, data, digest,
            [this, guarded, index](bool uploaded, const QString &apiError) {
            if (!guarded)
                return;
            TransferTask *task = guarded.data();
            task->m_reply.clear();
            if (task->m_state == TransferTask::State::Cancelled) {
                finishCancellation(task);
                return;
            }
            if (!uploaded) {
                if (task->m_state == TransferTask::State::Paused)
                    release(task);
                else
                    fail(task, apiError);
                return;
            }
            task->m_uploadedChunks.insert(index);
            const qint64 chunkOffset = static_cast<qint64>(index) * task->m_chunkSize;
            task->m_transferred += qMin<qint64>(task->m_chunkSize, task->m_size - chunkOffset);
            task->m_nextChunk = index + 1;
            notify(task);
            if (task->m_state == TransferTask::State::Paused)
                release(task);
            else
                sendNextChunk(task);
        });
    });
}

void TransferManager::startDownload(TransferTask *task)
{
    task->m_state = TransferTask::State::Downloading;
    task->m_transferred = 0;
    discardWriter(task);
    QPointer<TransferTask> guarded(task);
    const qint64 expectedSize = task->m_size > 0 ? task->m_size : -1;
    task->m_writer = new Workers::AsyncFileWriter(task->m_localPath, expectedSize, this,
        [this, guarded](bool ok, qint64 bytes, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused) {
            release(task);
            return;
        }
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
        release(task);
    }, task);

    task->m_reply = m_api->downloadFile(task->m_fileId,
        [this, guarded](bool ok, const QString &error) {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        QNetworkReply *reply = task->m_reply.data();
        if (ok && reply && task->m_writer && task->m_state == TransferTask::State::Downloading)
            task->m_writer->append(reply->readAll());
        task->m_reply.clear();
        if (task->m_state == TransferTask::State::Cancelled) {
            finishCancellation(task);
            return;
        }
        if (task->m_state == TransferTask::State::Paused) {
            release(task);
            return;
        }
        if (task->m_state == TransferTask::State::Failed)
            return; // A writer error already completed the failure transition.
        if (!ok) {
            fail(task, error);
            return;
        }
        if (task->m_writer)
            task->m_writer->finish();
    }, [this, guarded](qint64 received, qint64) {
        if (guarded && guarded->m_state == TransferTask::State::Downloading) {
            guarded->m_transferred = received;
            notify(guarded);
        }
    });
    QNetworkReply *reply = task->m_reply.data();
    connect(reply, &QNetworkReply::readyRead, this, [guarded, reply] {
        if (!guarded)
            return;
        TransferTask *task = guarded.data();
        if (task->m_writer && task->m_state == TransferTask::State::Downloading)
            task->m_writer->append(reply->readAll());
    });
}

void TransferManager::pause(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state == TransferTask::State::Completed
        || task->m_state == TransferTask::State::Failed
        || task->m_state == TransferTask::State::Cancelled)
        return;
    task->m_resumePending = false;
    task->m_state = TransferTask::State::Paused;
    if (task->direction() == TransferTask::Direction::Download)
        discardWriter(task);
    if (task->m_reply)
        task->m_reply->abort();
    notify(task);
    // Hash/read workers have no QNetworkReply to abort. Their completion
    // callbacks release the active slot after observing Paused.
    if (!m_active.contains(id))
        pump();
}

void TransferManager::resume(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state != TransferTask::State::Paused)
        return;
    if (m_active.contains(id) || task->m_reply) {
        // The aborted reply (or a preparing worker) still owns the active slot.
        // Let its callback observe Paused so OperationCanceled is not a failure.
        task->m_resumePending = true;
        return;
    }
    task->m_state = TransferTask::State::Queued;
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
    task->m_resumePending = false;
    task->m_error.clear();
    task->m_state = TransferTask::State::Queued;
    if (wasCancelled && task->direction() == TransferTask::Direction::Upload) {
        task->m_fileId.clear();
        task->m_uploadedChunks.clear();
        task->m_transferred = 0;
    }
    if (task->direction() == TransferTask::Direction::Download) {
        task->m_transferred = 0;
        // A partially downloaded QSaveFile was never committed; a fresh unique
        // destination avoids overwriting a successful earlier download.
        if (QFileInfo::exists(task->m_localPath)) {
            task->m_localPath = m_store->uniqueDownloadPath(task->m_fileName);
            m_reservedDownloads.insert(task->m_localPath);
        }
    }
    notify(task);
    pump();
}

void TransferManager::cancel(const QString &id)
{
    TransferTask *task = this->task(id);
    if (!task || task->m_state == TransferTask::State::Completed
        || task->m_state == TransferTask::State::Cancelled)
        return;
    task->m_resumePending = false;
    task->m_state = TransferTask::State::Cancelled;
    if (task->direction() == TransferTask::Direction::Download)
        discardWriter(task);
    if (task->m_reply)
        task->m_reply->abort();
    notify(task);
    if (!m_active.contains(id)
        || (task->direction() == TransferTask::Direction::Download && !task->m_reply))
        finishCancellation(task);
}

void TransferManager::fail(TransferTask *task, const QString &error)
{
    task->m_resumePending = false;
    discardWriter(task);
    task->m_error = error.isEmpty() ? QStringLiteral("未知传输错误") : error;
    task->m_state = TransferTask::State::Failed;
    notify(task);
    release(task);
}

void TransferManager::finishCancellation(TransferTask *task)
{
    discardWriter(task);
    if (!m_closing && task->direction() == TransferTask::Direction::Upload
        && !task->m_fileId.isEmpty()) {
        const QString fileId = task->m_fileId;
        m_api->deleteFile(fileId, [](bool, const QString &) {});
    }
    release(task);
}

void TransferManager::release(TransferTask *task)
{
    task->m_reply.clear();
    m_active.remove(task->id());
    if (!m_closing && task->m_state == TransferTask::State::Paused
        && task->m_resumePending) {
        task->m_resumePending = false;
        task->m_state = TransferTask::State::Queued;
        notify(task);
    }
    if (!m_closing)
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
