#pragma once

#include "ApiClient.h"

#include <QHash>
#include <QPointer>
#include <QSet>

class LocalStore;
namespace Workers { class AsyncFileWriter; }

class TransferTask final : public QObject {
    Q_OBJECT
public:
    enum class Direction { Upload, Download };
    enum class State { Queued, Preparing, Uploading, Downloading, Paused, Completed, Failed, Cancelled };

    QString id() const { return m_id; }
    Direction direction() const { return m_direction; }
    State state() const { return m_state; }
    QString peerId() const { return m_peerId; }
    QString fileId() const { return m_fileId; }
    QString fileName() const { return m_fileName; }
    QString localPath() const { return m_localPath; }
    qint64 size() const { return m_size; }
    qint64 transferredBytes() const { return m_transferred; }
    int nextChunk() const { return m_nextChunk; }
    int totalChunks() const { return m_totalChunks; }
    QString error() const { return m_error; }
    qint64 queuedDownloadBytes() const;
    qint64 peakQueuedDownloadBytes() const;
    bool isDownloadNetworkReadPaused() const { return m_networkReadPaused; }
    QString stateText() const;

private:
    friend class TransferManager;
    explicit TransferTask(Direction direction, QObject *parent);

    QString m_id;
    Direction m_direction;
    State m_state = State::Queued;
    QString m_peerId;
    QString m_fileId;
    QString m_fileName;
    QString m_localPath;
    QString m_sha256;
    qint64 m_size = 0;
    qint64 m_transferred = 0;
    int m_chunkSize = DefaultFileChunkSize;
    int m_nextChunk = 0;
    int m_totalChunks = 0;
    QSet<int> m_uploadedChunks;
    QSet<int> m_inflightChunks;
    QHash<int, QPointer<ApiRequestHandle>> m_chunkReplies;
    QString m_error;
    QPointer<ApiRequestHandle> m_reply;
    QPointer<ApiRequestHandle> m_deleteReply;
    Workers::AsyncFileWriter *m_writer = nullptr;
    quint64 m_generation = 0;
    bool m_hashPending = false;
    bool m_resumePending = false;
    bool m_waitingForFirstChannel = true;
    bool m_completePending = false;
    bool m_completionEmitted = false;
    bool m_cancellationFinalized = false;
    bool m_retryAfterDelete = false;
    bool m_networkReadPaused = false;
};

class TransferManager final : public QObject {
    Q_OBJECT
public:
    static constexpr int UploadChannelCount = 3;
    static constexpr int DownloadChannelCount = 3;

    TransferManager(ApiClient *api, LocalStore *store, QObject *parent = nullptr);
    ~TransferManager() override;

    // Stops replies and writer threads. Safe to call repeatedly during teardown.
    void shutdown();
    bool isClosing() const { return m_closing; }

    TransferTask *addUpload(const QString &peerId, const QString &sourcePath);
    TransferTask *addDownload(const QString &peerId, const QString &fileId,
                              const QString &fileName, qint64 size);
    QList<TransferTask *> tasks() const { return m_tasks; }
    TransferTask *task(const QString &id) const;

    void pause(const QString &id);
    void resume(const QString &id);
    void retry(const QString &id);
    void cancel(const QString &id);
    void setDownloadWriteDelayForTest(int delayMs) { m_downloadWriteDelayMs = delayMs; }

signals:
    void taskAdded(TransferTask *task);
    void taskChanged(TransferTask *task);
    void uploadCompleted(TransferTask *task);

private:
    void pump();
    void startTask(TransferTask *task);
    void startUpload(TransferTask *task);
    void refreshUploadStatus(TransferTask *task);
    void dispatchChunk(TransferTask *task);
    void maybeCompleteUpload(TransferTask *task);
    void finishUpload(TransferTask *task);
    void startDownload(TransferTask *task);
    void drainDownload(TransferTask *task);
    void resetCancelledUpload(TransferTask *task);
    void abortRequests(TransferTask *task);
    bool hasActiveWork(TransferTask *task) const;
    void settle(TransferTask *task);
    void fail(TransferTask *task, const QString &error);
    void finishCancellation(TransferTask *task);
    void notify(TransferTask *task) { if (!m_closing) emit taskChanged(task); }
    int activeUploadChannels() const;
    TransferTask *selectUploadTask() const;
    static int nextMissingChunk(TransferTask *task);
    static void updateUploadedProgress(TransferTask *task);
    static void discardWriter(TransferTask *task);
    static QString downloadReservationKey(const QString &path);

    ApiClient *m_api;
    LocalStore *m_store;
    QList<TransferTask *> m_tasks;
    QSet<QString> m_activeDownloads;
    QSet<QString> m_reservedDownloads;
    bool m_closing = false;
    int m_downloadWriteDelayMs = 0;
};
