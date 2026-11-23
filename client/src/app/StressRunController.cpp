#include "StressRunController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

namespace {
QString discoverProjectRoot()
{
    const QString configured = qEnvironmentVariable("SECURE_IM_PROJECT_ROOT").trimmed();
    if (!configured.isEmpty())
        return QDir(configured).absolutePath();

    QStringList starts{QDir::currentPath(), QCoreApplication::applicationDirPath()};
    for (const QString &start : starts) {
        QDir directory(start);
        for (int depth = 0; depth < 8; ++depth) {
            if (QFileInfo::exists(directory.filePath(QStringLiteral("client/CMakeLists.txt")))
                && QFileInfo::exists(directory.filePath(QStringLiteral("deploy"))))
                return directory.absolutePath();
            if (!directory.cdUp())
                break;
        }
    }
    return QDir::currentPath();
}

} // namespace

StressRunConfig StressRunConfig::forProject(const QString &projectRoot)
{
    StressRunConfig config;
    config.projectRoot = QDir(projectRoot.isEmpty() ? discoverProjectRoot() : projectRoot).absolutePath();
    QDir root(config.projectRoot);
    config.server = qEnvironmentVariable("SECURE_IM_STRESS_SERVER",
        QStringLiteral("http://127.0.0.1:18080"));
    config.envFile = root.filePath(QStringLiteral("deploy/.env.stress"));
    config.reportRoot = root.filePath(QStringLiteral("runtime/stress-reports"));
#ifdef Q_OS_WIN
    config.runnerPath = root.filePath(QStringLiteral(
        "client/build/qt-windows/bin/secure-im-stress-runner.exe"));
    config.powerShellPath = QStringLiteral("powershell.exe");
#else
    config.runnerPath = root.filePath(QStringLiteral(
        "client/build/qt-windows/bin/secure-im-stress-runner"));
    config.powerShellPath = QStringLiteral("pwsh");
#endif
    config.setupScriptPath = root.filePath(QStringLiteral("scripts/stress-up.ps1"));
    const QString runnerOverride = qEnvironmentVariable("SECURE_IM_STRESS_RUNNER").trimmed();
    if (!runnerOverride.isEmpty())
        config.runnerPath = runnerOverride;
    const QString reportOverride = qEnvironmentVariable("SECURE_IM_STRESS_REPORT_ROOT").trimmed();
    if (!reportOverride.isEmpty())
        config.reportRoot = reportOverride;
    const QString setupOverride = qEnvironmentVariable("SECURE_IM_STRESS_SETUP_SCRIPT").trimmed();
    if (!setupOverride.isEmpty())
        config.setupScriptPath = setupOverride;
    const QString shellOverride = qEnvironmentVariable("SECURE_IM_STRESS_POWERSHELL").trimmed();
    if (!shellOverride.isEmpty())
        config.powerShellPath = shellOverride;
    if (qEnvironmentVariable("SECURE_IM_STRESS_SKIP_SETUP") == QLatin1String("1"))
        config.setupScriptPath.clear();
    return config;
}

StressRunController::StressRunController(const StressRunConfig &config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_killTimer = new QTimer(this);
    m_killTimer->setSingleShot(true);
    m_killTimer->setInterval(1500);
    connect(m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process->state() != QProcess::NotRunning) {
            emit diagnostic(QStringLiteral("进程未响应终止请求，已强制结束。"));
            m_process->kill();
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &StressRunController::readStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_standardError.append(QString::fromUtf8(m_process->readAllStandardError()));
        if (m_standardError.size() > 16384)
            m_standardError = m_standardError.right(16384);
    });
    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                handleFinished(exitCode, static_cast<int>(status));
            });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                handleProcessError(static_cast<int>(error));
            });
}

StressRunController::~StressRunController()
{
    stopAndWait();
}

bool StressRunController::isRunning() const
{
    return m_stage != Stage::Idle;
}

bool StressRunController::start(qint64 target)
{
    if (isRunning())
        return false;
    if (target <= 0) {
        emit diagnostic(QStringLiteral("测试人数必须大于 0。"));
        return false;
    }

    m_stdoutBuffer.clear();
    m_standardError.clear();
    m_setupOutput.clear();
    m_prepareOnly = false;
    m_stopRequested = false;
    m_hasFinal = false;
    m_finishEmitted = false;
    m_final = StressFinal{};
    m_process->setWorkingDirectory(m_config.projectRoot);
    m_process->setProperty("stressTarget", target);
    emit runStarted();
    startSetup();
    return true;
}

bool StressRunController::prepare()
{
    if (isRunning())
        return false;
    m_stdoutBuffer.clear();
    m_standardError.clear();
    m_setupOutput.clear();
    m_prepareOnly = true;
    m_stopRequested = false;
    m_hasFinal = false;
    m_finishEmitted = false;
    m_final = StressFinal{};
    m_process->setWorkingDirectory(m_config.projectRoot);
    startSetup();
    return true;
}

void StressRunController::startSetup()
{
    if (m_config.setupScriptPath.trimmed().isEmpty()) {
        if (m_prepareOnly)
            finish(true, QStringLiteral("环境已就绪。"));
        else
            startRunner();
        return;
    }
    m_stage = Stage::Setup;
    emit diagnostic(QStringLiteral("正在准备压力测试环境…"));
    m_process->setProgram(m_config.powerShellPath);
    m_process->setArguments({QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-File"), m_config.setupScriptPath});
    m_process->start(QIODevice::ReadWrite);
}

// startRunner 在环境准备完成后启动真实负载进程，避免 setup 与 runner 同时写配置或占用同一报告目录。
void StressRunController::startRunner()
{
    if (m_prepareOnly) {
        finish(true, QStringLiteral("环境已就绪。"));
        return;
    }
    const qint64 target = m_process->property("stressTarget").toLongLong();
    m_stage = Stage::Runner;
    m_stdoutBuffer.clear();
    m_standardError.clear();
    emit diagnostic(QStringLiteral("环境已就绪，正在启动压力 runner。"));
    m_process->setProgram(m_config.runnerPath);
    m_process->setArguments({QStringLiteral("--target"), QString::number(target),
        QStringLiteral("--server"), m_config.server,
        QStringLiteral("--env-file"), m_config.envFile,
        QStringLiteral("--report-root"), m_config.reportRoot});
    m_process->start(QIODevice::ReadWrite);
}

void StressRunController::stop()
{
    if (!isRunning() || m_stopRequested)
        return;
    m_stopRequested = true;
    emit diagnostic(QStringLiteral("正在停止压力测试并回收输出…"));
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        m_killTimer->start();
    } else {
        finish(false, QStringLiteral("压力测试已停止。"));
    }
}

void StressRunController::stopAndWait()
{
    stop();
    if (m_process->state() != QProcess::NotRunning) {
        if (!m_process->waitForFinished(2000)) {
            m_process->kill();
            m_process->waitForFinished(1500);
        }
    }
    m_killTimer->stop();
    if (isRunning())
        finish(false, QStringLiteral("压力测试已停止。"));
}

void StressRunController::readStandardOutput()
{
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (m_stage != Stage::Runner) {
        m_setupOutput.append(QString::fromUtf8(bytes));
        if (m_setupOutput.size() > 16384)
            m_setupOutput = m_setupOutput.right(16384);
        return; // Setup output is diagnostic only, never runner NDJSON.
    }
    m_stdoutBuffer.append(bytes);
    consumeBuffer(false);
}

void StressRunController::drainStandardOutput()
{
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (m_stage != Stage::Runner) {
        m_setupOutput.append(QString::fromUtf8(bytes));
        if (m_setupOutput.size() > 16384)
            m_setupOutput = m_setupOutput.right(16384);
        return;
    }
    m_stdoutBuffer.append(bytes);
    consumeBuffer(true);
}

void StressRunController::consumeBuffer(bool drainFinalLine)
{
    qsizetype newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        consumeLine(line);
    }
    if (drainFinalLine && !m_stdoutBuffer.trimmed().isEmpty()) {
        const QByteArray line = m_stdoutBuffer;
        m_stdoutBuffer.clear();
        consumeLine(line);
    }
}

// consumeLine 逐行解析 NDJSON，容忍跨 read 的粘包、CRLF 和末尾半行；非法行只记录诊断，不能误当成最终报告。
void StressRunController::consumeLine(QByteArray line)
{
    const StressNdjsonCodec::Event event = StressNdjsonCodec::decodeLine(std::move(line));
    switch (event.type) {
    case StressNdjsonCodec::EventType::Invalid:
        emit diagnostic(QStringLiteral("忽略一行非法 NDJSON：%1").arg(event.error));
        break;
    case StressNdjsonCodec::EventType::Progress:
        emit progressReceived(event.progress);
        break;
    case StressNdjsonCodec::EventType::Final:
        m_final = event.final;
        m_hasFinal = true;
        emit finalReceived(m_final);
        break;
    case StressNdjsonCodec::EventType::Ignored:
        break;
    }
}

void StressRunController::handleFinished(int exitCode, int exitStatus)
{
    if (m_stage == Stage::Idle || m_finishEmitted)
        return;
    m_killTimer->stop();
    drainStandardOutput();

    if (m_stopRequested) {
        finish(false, QStringLiteral("压力测试已停止。"));
        return;
    }
    if (m_stage == Stage::Setup) {
        if (exitStatus == static_cast<int>(QProcess::NormalExit) && exitCode == 0) {
            if (m_prepareOnly)
                finish(true, QStringLiteral("环境已就绪。"));
            else
                startRunner();
        } else {
            finish(false, QStringLiteral("环境准备失败：%1").arg(processErrorText(exitCode, exitStatus)));
        }
        return;
    }

    if (exitStatus != static_cast<int>(QProcess::NormalExit) || exitCode != 0) {
        finish(false, QStringLiteral("压力 runner 异常结束：%1").arg(processErrorText(exitCode, exitStatus)));
        return;
    }
    if (!m_hasFinal) {
        finish(false, QStringLiteral("压力 runner 已退出，但输出缺少 final 结果。"));
        return;
    }
    const QString status = m_final.status.toLower();
    const bool success = status == QLatin1String("success")
        || status == QLatin1String("passed") || status == QLatin1String("pass")
        || status == QLatin1String("ok") || status == QLatin1String("fluctuation")
        || status == QLatin1String("warning");
    finish(success, success ? QStringLiteral("压力测试已完成。")
                            : QStringLiteral("压力测试未通过：%1").arg(m_final.reason));
}

void StressRunController::handleProcessError(int error)
{
    if (m_stage == Stage::Idle || m_finishEmitted)
        return;
    if (error == static_cast<int>(QProcess::FailedToStart)) {
        const QString subject = m_stage == Stage::Setup
            ? QStringLiteral("无法启动环境准备脚本") : QStringLiteral("无法启动压力 runner");
        finish(false, QStringLiteral("%1：%2").arg(subject, m_process->errorString()));
    } else if (error != static_cast<int>(QProcess::Crashed) || !m_stopRequested) {
        emit diagnostic(QStringLiteral("进程状态：%1").arg(m_process->errorString()));
    }
}

QString StressRunController::processErrorText(int exitCode, int exitStatus) const
{
    QString text = exitStatus == static_cast<int>(QProcess::NormalExit)
        ? QStringLiteral("退出码 %1").arg(exitCode) : QStringLiteral("进程崩溃");
    QStringList outputs;
    if (!m_setupOutput.trimmed().isEmpty())
        outputs.append(m_setupOutput.trimmed());
    if (!m_standardError.trimmed().isEmpty())
        outputs.append(m_standardError.trimmed());
    if (!outputs.isEmpty())
        text += QStringLiteral("；%1").arg(outputs.join(QStringLiteral("\n")).right(4000));
    return text;
}

void StressRunController::finish(bool success, const QString &message)
{
    if (m_finishEmitted)
        return;
    m_finishEmitted = true;
    m_stage = Stage::Idle;
    m_killTimer->stop();
    emit runFinished(success, message);
}
