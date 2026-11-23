// 暂停、继续、取消与重试生命周期；保留旧请求结算和删除后的重试屏障。
#include "TransferManager.h"

#include "LocalStore.h"

#include <QFileInfo>

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
