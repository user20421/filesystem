#pragma once

#include "ApiClient.h"

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
    int m_chunkSize = 4 * 1024 * 1024;
    int m_nextChunk = 0;
    int m_totalChunks = 0;
    QSet<int> m_uploadedChunks;
    QString m_error;
    QPointer<QNetworkReply> m_reply;
    Workers::AsyncFileWriter *m_writer = nullptr;
    bool m_resumePending = false;
};

class TransferManager final : public QObject {
    Q_OBJECT
public:
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

    int concurrency() const { return m_concurrency; }
    void setConcurrency(int value);

    void pause(const QString &id);
    void resume(const QString &id);
    void retry(const QString &id);
    void cancel(const QString &id);

signals:
    void taskAdded(TransferTask *task);
    void taskChanged(TransferTask *task);
    void uploadCompleted(TransferTask *task);

private:
    void pump();
    void startTask(TransferTask *task);
    void startUpload(TransferTask *task);
    void refreshUploadStatus(TransferTask *task);
    void sendNextChunk(TransferTask *task);
    void startDownload(TransferTask *task);
    void release(TransferTask *task);
    void fail(TransferTask *task, const QString &error);
    void finishCancellation(TransferTask *task);
    void notify(TransferTask *task) { if (!m_closing) emit taskChanged(task); }
    static void updateUploadedProgress(TransferTask *task);
    static void discardWriter(TransferTask *task);

    ApiClient *m_api;
    LocalStore *m_store;
    QList<TransferTask *> m_tasks;
    QSet<QString> m_active;
    QSet<QString> m_reservedDownloads;
    int m_concurrency = 3;
    bool m_closing = false;
};
