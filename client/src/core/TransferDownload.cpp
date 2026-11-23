// 顺序 writer 与下载背压；网络读暂停和用户暂停仍是独立状态。
#include "TransferManager.h"

#include "Workers.h"

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

    // 达到 1 MiB 停读，writer 消费到半水位后恢复；QNetworkReply 的有界缓冲区负责阻断 socket。
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

void TransferManager::discardWriter(TransferTask *task)
{
    if (!task || !task->m_writer)
        return;
    auto *writer = task->m_writer;
    task->m_writer = nullptr;
    delete writer;
}
