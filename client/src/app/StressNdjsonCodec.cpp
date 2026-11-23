#include "StressNdjsonCodec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace StressNdjsonCodec {
namespace {
qint64 integer(const QJsonObject &object, const char *key)
{
    return static_cast<qint64>(object.value(QLatin1String(key)).toDouble());
}

double number(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

QJsonObject payload(const QJsonObject &line, const char *type)
{
    const QJsonValue nested = line.value(QLatin1String(type));
    return nested.isObject() ? nested.toObject() : line;
}
} // namespace

Event decodeLine(QByteArray line)
{
    Event event;
    line = line.trimmed();
    if (line.isEmpty())
        return event;

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        event.type = EventType::Invalid;
        event.error = parseError.errorString();
        return event;
    }

    const QJsonObject root = document.object();
    const QString type = root.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("progress")) {
        const QJsonObject value = payload(root, "progress");
        event.type = EventType::Progress;
        StressProgress &progress = event.progress;
        progress.phase = value.value(QStringLiteral("phase")).toString();
        progress.target = integer(value, "target");
        progress.online = integer(value, "online");
        progress.messagesSent = integer(value, "messages_sent");
        progress.messagesDelivered = integer(value, "messages_delivered");
        progress.ackP95Ms = number(value, "ack_p95_ms");
        progress.ackP99Ms = number(value, "ack_p99_ms");
        progress.filesQueued = integer(value, "files_queued");
        progress.filesActive = integer(value, "files_active");
        progress.filesCompleted = integer(value, "files_completed");
        progress.filesFailed = integer(value, "files_failed");
        progress.bytesCompleted = integer(value, "bytes_completed");
        progress.fileMibPerSec = number(value, "file_mib_per_sec");
        progress.backendRssMib = number(value, "backend_rss_mib");
        progress.backendCpuPercent = number(value, "backend_cpu_percent");
        progress.goHeapMib = number(value, "go_heap_mib");
        progress.goroutines = integer(value, "goroutines");
        progress.fileBudgetUsedMib = number(value, "file_budget_used_mib");
        progress.fileBudgetLimitMib = number(value, "file_budget_limit_mib");
        const QJsonValue slotsUsed = value.value(QStringLiteral("file_chunk_slots_used"));
        const QJsonValue slotsLimit = value.value(QStringLiteral("file_chunk_slots_limit"));
        progress.fileChunkSlotsKnown = slotsUsed.isDouble() && slotsLimit.isDouble();
        if (progress.fileChunkSlotsKnown) {
            progress.fileChunkSlotsUsed = static_cast<qint64>(slotsUsed.toDouble());
            progress.fileChunkSlotsLimit = static_cast<qint64>(slotsLimit.toDouble());
        }
    } else if (type == QLatin1String("final")) {
        const QJsonObject value = payload(root, "final");
        event.type = EventType::Final;
        StressFinal &final = event.final;
        final.status = value.value(QStringLiteral("status")).toString();
        final.conclusion = value.value(QStringLiteral("conclusion")).toString();
        final.reportDir = value.value(QStringLiteral("report_dir")).toString();
        final.html = value.value(QStringLiteral("html")).toString();
        final.target = integer(value, "target");
        final.reason = value.value(QStringLiteral("reason")).toString();
    }
    return event;
}
} // namespace StressNdjsonCodec
