#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

struct StressProgress {
    QString phase;
    qint64 target = 0;
    qint64 online = 0;
    qint64 messagesSent = 0;
    qint64 messagesDelivered = 0;
    double ackP95Ms = 0.0;
    double ackP99Ms = 0.0;
    qint64 filesQueued = 0;
    qint64 filesActive = 0;
    qint64 filesCompleted = 0;
    qint64 filesFailed = 0;
    qint64 bytesCompleted = 0;
    double fileMibPerSec = 0.0;
    double backendRssMib = 0.0;
    double backendCpuPercent = 0.0;
    double goHeapMib = 0.0;
    qint64 goroutines = 0;
    double fileBudgetUsedMib = 0.0;
    double fileBudgetLimitMib = 0.0;
    qint64 fileChunkSlotsUsed = 0;
    qint64 fileChunkSlotsLimit = 0;
    bool fileChunkSlotsKnown = false;
};

struct StressFinal {
    QString status;
    QString conclusion;
    QString reportDir;
    QString html;
    qint64 target = 0;
    QString reason;
};

Q_DECLARE_METATYPE(StressProgress)
Q_DECLARE_METATYPE(StressFinal)

// StressNdjsonCodec 只解码单行 runner NDJSON；粘包缓冲和进程生命周期仍由控制器负责。
namespace StressNdjsonCodec {
enum class EventType { Ignored, Invalid, Progress, Final };
struct Event {
    EventType type = EventType::Ignored;
    StressProgress progress;
    StressFinal final;
    QString error;
};

Event decodeLine(QByteArray line);
} // namespace StressNdjsonCodec
