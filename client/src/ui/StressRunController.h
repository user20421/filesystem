#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

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

struct StressRunConfig {
    QString projectRoot;
    QString server;
    QString envFile;
    QString reportRoot;
    QString runnerPath;
    QString setupScriptPath;
    QString powerShellPath;

    // Test/development hooks. An empty setupScriptPath skips stress-up.ps1;
    // runnerPath and reportRoot may point at isolated temporary directories.
    static StressRunConfig forProject(const QString &projectRoot = {});
};

// StressRunController 管理环境准备与 runner 两阶段进程生命周期。
// 它负责 NDJSON 粘包解析、停止/终止、最终报告收集和只发一次的结束信号。
class StressRunController final : public QObject {
    Q_OBJECT
public:
    explicit StressRunController(const StressRunConfig &config, QObject *parent = nullptr);
    ~StressRunController() override;

    bool start(qint64 target);
    // prepare 只执行 Setup，不会启动 runner；失败输出保留到下一次调用。
    bool prepare();
    void stop();
    // 页面返回或关闭时同步回收进程，之后才能释放系统测试全局锁。
    void stopAndWait();
    bool isRunning() const;
    bool hasFinal() const { return m_hasFinal; }
    StressRunConfig config() const { return m_config; }

signals:
    void runStarted();
    void progressReceived(const StressProgress &progress);
    void finalReceived(const StressFinal &result);
    void diagnostic(const QString &message);
    void runFinished(bool success, const QString &message);

private:
    enum class Stage { Idle, Setup, Runner };

    void startSetup();
    void startRunner();
    void readStandardOutput();
    void drainStandardOutput();
    void consumeBuffer(bool drainFinalLine);
    void consumeLine(QByteArray line);
    void handleFinished(int exitCode, int exitStatus);
    void handleProcessError(int error);
    void finish(bool success, const QString &message);
    QString processErrorText(int exitCode, int exitStatus) const;

    StressRunConfig m_config;
    QProcess *m_process = nullptr;
    QTimer *m_killTimer = nullptr;
    Stage m_stage = Stage::Idle;
    QByteArray m_stdoutBuffer;
    QString m_standardError;
    QString m_setupOutput;
    bool m_prepareOnly = false;
    bool m_stopRequested = false;
    bool m_hasFinal = false;
    bool m_finishEmitted = false;
    StressFinal m_final;
};
