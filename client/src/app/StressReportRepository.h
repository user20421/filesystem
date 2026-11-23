#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

struct StressReportEntry {
    QDateTime timestamp;
    QString directory;
    QString html;
    qint64 target = 0;
    QString conclusion;
    QString reason;
};

// StressReportRepository 按既有 summary.json schema 读取报告，并保持时间、目录倒序规则。
class StressReportRepository {
public:
    explicit StressReportRepository(QString reportRoot = {});
    QList<StressReportEntry> readHistory() const;

private:
    QString m_reportRoot;
};
