// 3+3 通道调度：只移动既有选择逻辑，不改变 FIFO、剩余量优先或 generation。
#include "TransferManager.h"

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
