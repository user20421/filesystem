#pragma once

#include "StressRunController.h"

#include <QDateTime>
#include <QUrl>
#include <QWidget>
#include <functional>

class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;

class SystemTestPage final : public QWidget {
    Q_OBJECT
public:
    explicit SystemTestPage(QWidget *parent = nullptr);
    explicit SystemTestPage(const StressRunConfig &config, QWidget *parent = nullptr);

    void shutdown();
    StressRunController *controller() const { return m_controller; }
    void refreshHistory();

    using ReportOpener = std::function<bool(const QUrl &)>;
    static void setReportOpenerForTests(ReportOpener opener);

private:
    struct HistoryEntry {
        QDateTime timestamp;
        QString directory;
        QString html;
        qint64 target = 0;
        QString conclusion;
        QString reason;
    };

    void buildUi();
    void updateDistribution();
    void updateCountdown();
    void updateProgress(const StressProgress &progress);
    void updateFinal(const StressFinal &result);
    void finishRun(bool success, const QString &message);
    void setRunning(bool running);
    void openSelectedHtml();
    void openSelectedDirectory();
    QList<HistoryEntry> readHistory() const;
    QString bottleneckAdvice() const;
    static bool openReportUrl(const QUrl &url);

    StressRunController *m_controller = nullptr;
    QSpinBox *m_target = nullptr;
    QLabel *m_distribution = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QLabel *m_phase = nullptr;
    QLabel *m_conclusion = nullptr;
    QLabel *m_advice = nullptr;
    QLabel *m_online = nullptr;
    QLabel *m_messages = nullptr;
    QLabel *m_messageDetail = nullptr;
    QLabel *m_files = nullptr;
    QLabel *m_fileDetail = nullptr;
    QLabel *m_latency = nullptr;
    QLabel *m_throughput = nullptr;
    QLabel *m_backend = nullptr;
    QLabel *m_go = nullptr;
    QLabel *m_budget = nullptr;
    QLabel *m_bytes = nullptr;
    QTreeWidget *m_history = nullptr;
    QProgressBar *m_countdownBar = nullptr;
    QLabel *m_countdownLabel = nullptr;
    QTimer *m_countdownTimer = nullptr;
    QDateTime m_stableStarted;
    QPushButton *m_openHtml = nullptr;
    QPushButton *m_openDirectory = nullptr;
    StressProgress m_lastProgress;
    StressFinal m_lastFinal;
    bool m_hasFinal = false;

    static ReportOpener s_reportOpener;
};
