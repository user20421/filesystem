#include "StressReportRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

StressReportRepository::StressReportRepository(QString reportRoot)
    : m_reportRoot(std::move(reportRoot)) {}

QList<StressReportEntry> StressReportRepository::readHistory() const
{
    QList<StressReportEntry> entries;
    const QDir root(m_reportRoot);
    const auto directories = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name);
    for (const QFileInfo &directory : directories) {
        const QString summaryPath = QDir(directory.absoluteFilePath()).filePath(QStringLiteral("summary.json"));
        QFile file(summaryPath);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject())
            continue;
        QJsonObject summary = document.object();
        if (summary.value(QStringLiteral("final")).isObject())
            summary = summary.value(QStringLiteral("final")).toObject();
        StressReportEntry entry;
        entry.directory = directory.absoluteFilePath();
        entry.target = static_cast<qint64>(summary.value(QStringLiteral("target")).toDouble());
        const QJsonObject result = summary.value(QStringLiteral("result")).toObject();
        entry.conclusion = result.value(QStringLiteral("status")).toString();
        if (entry.conclusion.isEmpty())
            entry.conclusion = summary.value(QStringLiteral("status")).toString();
        if (entry.conclusion.isEmpty())
            entry.conclusion = summary.value(QStringLiteral("conclusion")).toString();
        entry.reason = summary.value(QStringLiteral("reason")).toString();
        if (entry.reason.isEmpty())
            entry.reason = result.value(QStringLiteral("reason")).toString();
        entry.html = summary.value(QStringLiteral("html")).toString();
        if (entry.html.isEmpty())
            entry.html = QDir(directory.absoluteFilePath()).filePath(QStringLiteral("report.html"));
        else if (QDir::isRelativePath(entry.html))
            entry.html = QDir(directory.absoluteFilePath()).filePath(entry.html);

        const QStringList timeKeys{QStringLiteral("started_at"), QStringLiteral("timestamp"),
            QStringLiteral("created_at"), QStringLiteral("finished_at"), QStringLiteral("time")};
        for (const QString &key : timeKeys) {
            const QJsonValue value = summary.value(key);
            if (value.isString()) {
                entry.timestamp = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
                if (!entry.timestamp.isValid())
                    entry.timestamp = QDateTime::fromString(value.toString(), Qt::ISODate);
            } else if (value.isDouble()) {
                const qint64 raw = static_cast<qint64>(value.toDouble());
                entry.timestamp = raw > 100000000000LL
                    ? QDateTime::fromMSecsSinceEpoch(raw) : QDateTime::fromSecsSinceEpoch(raw);
            }
            if (entry.timestamp.isValid())
                break;
        }
        if (!entry.timestamp.isValid())
            entry.timestamp = QFileInfo(summaryPath).lastModified();
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const StressReportEntry &left, const StressReportEntry &right) {
        if (left.timestamp != right.timestamp)
            return left.timestamp > right.timestamp;
        return left.directory > right.directory;
    });
    return entries;
}
