#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QThread;

// Workers 将哈希计算和文件读取放入公共线程池，将下载写入交给专用顺序 writer。
// 结果通过队列回调返回接收者所在线程，避免阻塞 UI。
namespace Workers {
using HashCallback = std::function<void(bool, QString, QString)>;
using ChunkCallback = std::function<void(bool, QByteArray, QString, QString)>;

void hashFile(const QString &path, QObject *receiver, HashCallback callback);
void readChunk(const QString &path, qint64 offset, int size, QObject *receiver,
               ChunkCallback callback);

using StreamWriteCallback = std::function<void(bool, qint64, QString)>;
class AsyncFileWriter final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxQueuedBytes = 1024 * 1024;

    AsyncFileWriter(const QString &path, qint64 expectedSize, QObject *receiver,
                    StreamWriteCallback callback, QObject *parent = nullptr,
                    int writeDelayMs = 0);
    ~AsyncFileWriter() override;
    bool append(const QByteArray &data);
    void finish();
    qint64 queuedBytes() const { return m_queuedBytes; }
    qint64 peakQueuedBytes() const { return m_peakQueuedBytes; }
    qint64 availableBytes() const { return MaxQueuedBytes - m_queuedBytes; }

signals:
    void queuedBytesChanged(qint64 bytes);

private:
    QThread *m_thread;
    QObject *m_worker;
    QPointer<QObject> m_receiver;
    StreamWriteCallback m_callback;
    qint64 m_queuedBytes = 0;
    qint64 m_peakQueuedBytes = 0;
};
}
