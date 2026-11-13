#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class QThread;

// Blocking digest and local file work is dispatched to QThreadPool or a
// dedicated sequential writer worker.  Results return to receiver's thread.
namespace Workers {
using HashCallback = std::function<void(bool, QString, QString)>;
using ChunkCallback = std::function<void(bool, QByteArray, QString, QString)>;
using WriteCallback = std::function<void(bool, QString)>;

void hashFile(const QString &path, QObject *receiver, HashCallback callback);
void readChunk(const QString &path, qint64 offset, int size, QObject *receiver,
               ChunkCallback callback);
void writeFile(const QString &path, const QByteArray &data, QObject *receiver,
               WriteCallback callback);

using StreamWriteCallback = std::function<void(bool, qint64, QString)>;
class AsyncFileWriter final : public QObject {
public:
    AsyncFileWriter(const QString &path, qint64 expectedSize, QObject *receiver,
                    StreamWriteCallback callback, QObject *parent = nullptr);
    ~AsyncFileWriter() override;
    void append(const QByteArray &data);
    void finish();

private:
    QThread *m_thread;
    QObject *m_worker;
    QPointer<QObject> m_receiver;
    StreamWriteCallback m_callback;
};
}
