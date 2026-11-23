// 上传准备、分块调度与完成流程；异步回调继续使用既有 generation 屏障。
#include "TransferManager.h"

#include "Workers.h"

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
