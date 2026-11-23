#include "SimulatorWindow.h"
#include "StressRunController.h"
#include "SystemTestPage.h"
#include "support/TestEnvironment.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

#include <algorithm>
#include <memory>

namespace {
StressRunConfig fakeConfig(const QString &root)
{
    StressRunConfig config = StressRunConfig::forProject(root);
    config.setupScriptPath.clear();
    config.runnerPath = QCoreApplication::applicationDirPath() + QStringLiteral("/fake_stress_runner");
#ifdef Q_OS_WIN
    config.runnerPath += QStringLiteral(".exe");
#endif
    config.reportRoot = QDir(root).filePath(QStringLiteral("runtime/stress-reports"));
    config.envFile = QDir(root).filePath(QStringLiteral("deploy/.env"));
    return config;
}

void writeSummary(const QString &root, const QString &name, const QString &timestamp,
                  qint64 target, const QString &conclusion = QStringLiteral("success"))
{
    const QString directory = QDir(root).filePath(name);
    QVERIFY(QDir().mkpath(directory));
    const QJsonObject summary{{QStringLiteral("started_at"), timestamp},
        {QStringLiteral("target"), target}, {QStringLiteral("conclusion"), conclusion},
        {QStringLiteral("html"), QStringLiteral("index.html")},
        {QStringLiteral("result"), QJsonObject{
            {QStringLiteral("reason"), QStringLiteral("report %1").arg(name)}}}};
    QFile file(QDir(directory).filePath(QStringLiteral("summary.json")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(summary).toJson(QJsonDocument::Compact));
    QFile html(QDir(directory).filePath(QStringLiteral("index.html")));
    QVERIFY(html.open(QIODevice::WriteOnly));
    html.write("<html>report</html>");
}

} // namespace

class StressRunTest final : public QObject {
    Q_OBJECT
private slots:
    void runsSetupBeforeRunner();
    void parsesStickyNdjsonAndDrainsFinal();
    void showsChunkSlotStates();
    void displaysLocalizedPhasesAndMetricNames();
    void targetSpinUsesThousands();
    void reportsNonZeroExit();
    void rejectsMissingFinal();
    void showsFluctuationConclusion();
    void stopsActiveRunner();
    void destructorStopsActiveRunner();
    void refreshesHistoryAndOpensReports();
    void rootLoginThenWindowCloseStopsActiveRun();
};

void StressRunTest::runsSetupBeforeRunner()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    ScopedEnvironment setupCapture("SECURE_IM_FAKE_SETUP_FILE");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "sticky");
    const QString marker = root.filePath(QStringLiteral("setup.txt"));
    qputenv("SECURE_IM_FAKE_SETUP_FILE", marker.toUtf8());
    StressRunConfig config = fakeConfig(root.path());
    config.setupScriptPath = root.filePath(QStringLiteral("scripts/stress-up.ps1"));
    config.powerShellPath = config.runnerPath;
    StressRunController controller(config);
    QSignalSpy finished(&controller, &StressRunController::runFinished);
    QVERIFY(controller.start(1000));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 6000);
    QCOMPARE(finished.first().at(0).toBool(), true);
    QFile setup(marker);
    QVERIFY(setup.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(setup.readAll()), config.setupScriptPath);
    QVERIFY(controller.hasFinal());
}

void StressRunTest::parsesStickyNdjsonAndDrainsFinal()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    ScopedEnvironment capture("SECURE_IM_FAKE_ARGUMENTS_FILE");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "sticky");
    const QString capturePath = root.filePath(QStringLiteral("arguments.json"));
    qputenv("SECURE_IM_FAKE_ARGUMENTS_FILE", capturePath.toUtf8());
    StressRunController controller(fakeConfig(root.path()));
    QSignalSpy progress(&controller, &StressRunController::progressReceived);
    QSignalSpy finalResult(&controller, &StressRunController::finalReceived);
    QSignalSpy finished(&controller, &StressRunController::runFinished);
    QSignalSpy diagnostics(&controller, &StressRunController::diagnostic);
    QVERIFY(controller.start(1000));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 6000);
    QCOMPARE(finished.first().at(0).toBool(), true);
    QCOMPARE(progress.size(), 2);
    QCOMPARE(finalResult.size(), 1);
    QVERIFY(diagnostics.size() >= 1);

    const StressProgress first = qvariant_cast<StressProgress>(progress.first().at(0));
    QCOMPARE(first.phase, QStringLiteral("connected"));
    QCOMPARE(first.target, qint64(1000));
    QCOMPARE(first.online, qint64(997));
    QCOMPARE(first.messagesSent, qint64(12000));
    QCOMPARE(first.messagesDelivered, qint64(11990));
    QCOMPARE(first.ackP95Ms, 42.5);
    QCOMPARE(first.ackP99Ms, 91.25);
    QCOMPARE(first.filesQueued, qint64(3));
    QCOMPARE(first.filesActive, qint64(4));
    QCOMPARE(first.filesCompleted, qint64(55));
    QCOMPARE(first.filesFailed, qint64(1));
    QCOMPARE(first.bytesCompleted, qint64(10485760));
    QCOMPARE(first.fileMibPerSec, 12.5);
    QCOMPARE(first.backendRssMib, 768.5);
    QCOMPARE(first.backendCpuPercent, 63.25);
    QCOMPARE(first.goHeapMib, 128.75);
    QCOMPARE(first.goroutines, qint64(345));
    QCOMPARE(first.fileBudgetUsedMib, 120.5);
    QCOMPARE(first.fileBudgetLimitMib, 500.0);
    QCOMPARE(first.fileChunkSlotsUsed, qint64(7));
    QCOMPARE(first.fileChunkSlotsLimit, qint64(96));
    QCOMPARE(first.fileChunkSlotsKnown, true);
    const StressProgress legacy = qvariant_cast<StressProgress>(progress.at(1).at(0));
    QCOMPARE(legacy.fileChunkSlotsKnown, false);
    const StressFinal result = qvariant_cast<StressFinal>(finalResult.first().at(0));
    QCOMPARE(result.status, QStringLiteral("success"));
    QCOMPARE(result.target, qint64(1000));
    QCOMPARE(result.reportDir, QStringLiteral("run-1000"));

    QFile argumentsFile(capturePath);
    QVERIFY(argumentsFile.open(QIODevice::ReadOnly));
    const QJsonArray arguments = QJsonDocument::fromJson(argumentsFile.readAll()).array();
    QStringList actual;
    for (const auto &argument : arguments)
        actual.append(argument.toString());
    const StressRunConfig config = controller.config();
    QCOMPARE(actual, QStringList({QStringLiteral("--target"), QStringLiteral("1000"),
        QStringLiteral("--server"), config.server,
        QStringLiteral("--env-file"), config.envFile,
        QStringLiteral("--report-root"), config.reportRoot}));
    bool sawInvalidJson = false;
    for (const auto &entry : diagnostics)
        sawInvalidJson |= entry.at(0).toString().contains(QStringLiteral("非法 NDJSON"));
    QVERIFY(sawInvalidJson);
}

void StressRunTest::displaysLocalizedPhasesAndMetricNames()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "phases");
    SystemTestPage page(fakeConfig(root.path()));
    auto *phase = page.findChild<QLabel *>(QStringLiteral("stressPhaseLabel"));
    auto *start = page.findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    auto *stop = page.findChild<QPushButton *>(QStringLiteral("stressStopButton"));
    QVERIFY(phase && start && stop);
    const auto labels = page.findChildren<QLabel *>();
    QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
        return label->text() == QStringLiteral("Go 堆 / 协程数");
    }));

    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("准备环境"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("连接爬坡"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("稳定测试"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("收尾排空"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("清理数据"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(phase->text(), QStringLiteral("待命"), 5000);
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!page.controller()->isRunning(), 5000);
}

void StressRunTest::targetSpinUsesThousands()
{
    QTemporaryDir root;
    SystemTestPage page(fakeConfig(root.path()));
    auto *target = page.findChild<QSpinBox *>(QStringLiteral("stressTargetSpinBox"));
    QVERIFY(target);
    QCOMPARE(target->singleStep(), 1000);
    QCOMPARE(target->buttonSymbols(), QSpinBox::PlusMinus);
    QVERIFY(target->isGroupSeparatorShown());
    QVERIFY(target->height() >= 36);
    target->setValue(5000);
    QTest::keyClick(target, Qt::Key_Up);
    QCOMPARE(target->value(), 6000);
    QTest::keyClick(target, Qt::Key_Down);
    QCOMPARE(target->value(), 5000);
}

void StressRunTest::showsChunkSlotStates()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    SystemTestPage page(fakeConfig(root.path()));
    auto *chunkSlotsLabel = page.findChild<QLabel *>(QStringLiteral("stressChunkSlotsValue"));
    auto *start = page.findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    auto *stop = page.findChild<QPushButton *>(QStringLiteral("stressStopButton"));
    QVERIFY(chunkSlotsLabel && start && stop);
    QCOMPARE(page.findChildren<QFrame *>(QStringLiteral("stressMetricCard")).size(), 8);
    QCOMPARE(chunkSlotsLabel->text(), QStringLiteral("—"));

    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "hold");
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(chunkSlotsLabel->text(), QStringLiteral("分块通道 7 / 96"), 5000);
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!page.controller()->isRunning(), 5000);
    QCOMPARE(chunkSlotsLabel->text(), QStringLiteral("—"));

    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "unknown-hold");
    start->click();
    QTRY_COMPARE_WITH_TIMEOUT(chunkSlotsLabel->text(), QStringLiteral("分块通道未知"), 5000);
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!page.controller()->isRunning(), 5000);
    QCOMPARE(chunkSlotsLabel->text(), QStringLiteral("—"));
}

void StressRunTest::reportsNonZeroExit()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "nonzero");
    StressRunController controller(fakeConfig(root.path()));
    QSignalSpy finalResult(&controller, &StressRunController::finalReceived);
    QSignalSpy finished(&controller, &StressRunController::runFinished);
    QVERIFY(controller.start(7));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 6000);
    QCOMPARE(finalResult.size(), 1);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("退出码 7")));
    QVERIFY(!controller.isRunning());
}

void StressRunTest::rejectsMissingFinal()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "missing-final");
    StressRunController controller(fakeConfig(root.path()));
    QSignalSpy finished(&controller, &StressRunController::runFinished);
    QVERIFY(controller.start(9));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 6000);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("缺少 final")));
    QVERIFY(!controller.hasFinal());
}

void StressRunTest::showsFluctuationConclusion()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "fluctuation");
    SystemTestPage page(fakeConfig(root.path()));
    auto *start = page.findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    auto *countdown = page.findChild<QProgressBar *>(QStringLiteral("stressCountdownBar"));
    QVERIFY(start && countdown);
    QCOMPARE(countdown->maximum(), 300);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(page.controller()->hasFinal() && !page.controller()->isRunning(), 6000);
    QCOMPARE(page.findChild<QLabel *>(QStringLiteral("stressConclusionLabel"))->text(),
             QStringLiteral("波动"));
    QCOMPARE(page.findChild<QLabel *>(QStringLiteral("stressPhaseLabel"))->text(),
             QStringLiteral("已完成"));
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("stressAdviceLabel"))->text()
        .contains(QStringLiteral("检查消息持久化")));
}

void StressRunTest::stopsActiveRunner()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "hold");
    StressRunController controller(fakeConfig(root.path()));
    QSignalSpy progress(&controller, &StressRunController::progressReceived);
    QSignalSpy finished(&controller, &StressRunController::runFinished);
    QVERIFY(controller.start(1000));
    QTRY_VERIFY_WITH_TIMEOUT(progress.size() == 1, 5000);
    controller.stop();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 5000);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QCOMPARE(finished.first().at(1).toString(), QStringLiteral("压力测试已停止。"));
    QVERIFY(!controller.isRunning());
}

void StressRunTest::destructorStopsActiveRunner()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "hold");
    auto controller = std::make_unique<StressRunController>(fakeConfig(root.path()));
    QSignalSpy progress(controller.get(), &StressRunController::progressReceived);
    QVERIFY(controller->start(1000));
    QTRY_VERIFY_WITH_TIMEOUT(progress.size() == 1, 5000);
    controller.reset();
    QVERIFY(!controller);
}

void StressRunTest::refreshesHistoryAndOpensReports()
{
    QTemporaryDir root;
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "sticky");
    const StressRunConfig config = fakeConfig(root.path());
    writeSummary(config.reportRoot, QStringLiteral("older"),
        QStringLiteral("2026-11-12T08:00:00Z"), 5000, QStringLiteral("failed"));
    writeSummary(config.reportRoot, QStringLiteral("newer"),
        QStringLiteral("2026-11-13T09:00:00Z"), 10000, QStringLiteral("warning"));
    SystemTestPage page(config);
    auto *history = page.findChild<QTreeWidget *>(QStringLiteral("stressHistoryList"));
    auto *openHtml = page.findChild<QPushButton *>(QStringLiteral("openStressHtmlButton"));
    auto *openDirectory = page.findChild<QPushButton *>(QStringLiteral("openStressReportButton"));
    QVERIFY(history && openHtml && openDirectory);
    QCOMPARE(history->topLevelItemCount(), 2);
    QCOMPARE(history->topLevelItem(0)->text(1), QStringLiteral("10,000"));
    QCOMPARE(history->topLevelItem(1)->text(1), QStringLiteral("5,000"));
    QCOMPARE(history->topLevelItem(0)->text(2), QStringLiteral("波动"));
    QCOMPARE(history->topLevelItem(1)->text(2), QStringLiteral("失败"));
    QCOMPARE(history->topLevelItem(0)->text(3), QStringLiteral("report newer"));

    QList<QUrl> opened;
    SystemTestPage::setReportOpenerForTests([&opened](const QUrl &url) {
        opened.append(url);
        return true;
    });
    history->setCurrentItem(history->topLevelItem(0));
    openHtml->click();
    openDirectory->click();
    QCOMPARE(opened.size(), 2);
    QVERIFY(opened.at(0).toLocalFile().endsWith(QDir::cleanPath(QStringLiteral("newer/index.html"))));
    QCOMPARE(opened.at(1).toLocalFile(), QDir(config.reportRoot).absoluteFilePath(QStringLiteral("newer")));

    auto *start = page.findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    QVERIFY(start);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(page.controller()->hasFinal() && !page.controller()->isRunning(), 6000);
    QTRY_VERIFY_WITH_TIMEOUT(history->topLevelItemCount() == 3, 3000);
    QCOMPARE(history->topLevelItem(0)->text(1), QStringLiteral("1,000"));
    QCOMPARE(history->topLevelItem(0)->text(2), QStringLiteral("成功"));
    QCOMPARE(page.findChild<QLabel *>(QStringLiteral("stressConclusionLabel"))->text(),
             QStringLiteral("成功"));
    QCOMPARE(page.findChild<QLabel *>(QStringLiteral("stressOnlineValue"))->text(),
             QStringLiteral("1,000 / 1,000"));
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("stressAdviceLabel"))->text()
        .contains(QStringLiteral("文件失败")));
    SystemTestPage::setReportOpenerForTests({});
}

void StressRunTest::rootLoginThenWindowCloseStopsActiveRun()
{
    QTemporaryDir root;
    ScopedEnvironment project("SECURE_IM_PROJECT_ROOT");
    ScopedEnvironment runner("SECURE_IM_STRESS_RUNNER");
    ScopedEnvironment reports("SECURE_IM_STRESS_REPORT_ROOT");
    ScopedEnvironment skip("SECURE_IM_STRESS_SKIP_SETUP");
    ScopedEnvironment scenario("SECURE_IM_FAKE_STRESS_SCENARIO");
    const StressRunConfig config = fakeConfig(root.path());
    qputenv("SECURE_IM_PROJECT_ROOT", root.path().toUtf8());
    qputenv("SECURE_IM_STRESS_RUNNER", config.runnerPath.toUtf8());
    qputenv("SECURE_IM_STRESS_REPORT_ROOT", config.reportRoot.toUtf8());
    qputenv("SECURE_IM_STRESS_SKIP_SETUP", "1");
    qputenv("SECURE_IM_FAKE_STRESS_SCENARIO", "hold");

    QTcpServer authServer;
    QVERIFY(authServer.listen(QHostAddress::LocalHost, 0));
    QByteArray loginRequest;
    connect(&authServer, &QTcpServer::newConnection, &authServer, [&] {
        while (authServer.hasPendingConnections()) {
            QTcpSocket *socket = authServer.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                loginRequest.append(socket->readAll());
                if (!loginRequest.contains("\r\n\r\n"))
                    return;
                const QByteArray body = R"({"data":{"user":{"id":"root-id","username":"ROOT","display_name":"ROOT"},"tokens":{"access_token":"authenticated-root-token","refresh_token":"root-refresh"}}})";
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
        }
    });
    const QUrl serverUrl(QStringLiteral("http://127.0.0.1:%1").arg(authServer.serverPort()));
    SimulatorWindow window(root.filePath(QStringLiteral("simulator")), serverUrl);
    QVERIFY(window.isOpen());
    auto *login = window.findChild<QPushButton *>(QStringLiteral("loginButton"));
    QVERIFY(login);
    login->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.isAuthenticated(), 3000);
    QVERIFY(loginRequest.startsWith("POST /api/v1/auth/login "));
    QVERIFY(loginRequest.contains("\"username\":\"ROOT\""));
    QVERIFY(loginRequest.contains("\"password\":\"123456\""));

    SystemTestPage *page = window.systemTestPage();
    auto *start = page->findChild<QPushButton *>(QStringLiteral("stressTestButton"));
    QVERIFY(page && start);
    start->click();
    QSignalSpy progress(page->controller(), &StressRunController::progressReceived);
    QTRY_VERIFY_WITH_TIMEOUT(progress.size() == 1, 5000);
    QVERIFY(page->controller()->isRunning());
    window.close();
    QTRY_VERIFY_WITH_TIMEOUT(!page->controller()->isRunning(), 5000);
}

QTEST_MAIN(StressRunTest)
#include "test_stress_run.moc"
