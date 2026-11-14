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

class StressRunController final : public QObject {
    Q_OBJECT
public:
    explicit StressRunController(QObject *parent = nullptr);
    explicit StressRunController(const StressRunConfig &config, QObject *parent = nullptr);
    ~StressRunController() override;

    bool start(qint64 target);
    void stop();
    bool isRunning() const;
    bool hasFinal() const { return m_hasFinal; }
    StressFinal finalResult() const { return m_final; }
    StressRunConfig config() const { return m_config; }
    QString runnerArgumentsForTests() const;

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
    bool m_stopRequested = false;
    bool m_hasFinal = false;
    bool m_finishEmitted = false;
    StressFinal m_final;
};
