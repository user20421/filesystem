#include "SystemTestPage.h"

#include "StressAssessment.h"
#include "StressReportRepository.h"

#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kStableSeconds = 5 * 60;

QString remainingText(int seconds)
{
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

SystemTestPage::ReportOpener SystemTestPage::s_reportOpener;

namespace {
QString integerText(qint64 value)
{
    return QLocale().toString(value);
}

QString decimalText(double value, int precision = 1)
{
    return QLocale().toString(value, 'f', precision);
}

QString phaseDisplayText(const QString &phase)
{
    if (phase == QLatin1String("rampup"))
        return QStringLiteral("连接爬坡");
    if (phase == QLatin1String("stable"))
        return QStringLiteral("稳定测试");
    if (phase == QLatin1String("drain"))
        return QStringLiteral("收尾排空");
    if (phase == QLatin1String("cleanup"))
        return QStringLiteral("清理数据");
    if (phase == QLatin1String("preflight"))
        return QStringLiteral("准备环境");
    if (phase == QLatin1String("idle"))
        return QStringLiteral("待命");
    return phase;
}

QString statusDisplayText(const QString &status)
{
    const QString normalized = status.trimmed().toLower();
    if (normalized == QLatin1String("success") || normalized == QLatin1String("passed")
        || normalized == QLatin1String("pass") || normalized == QLatin1String("ok"))
        return QStringLiteral("成功");
    if (normalized == QLatin1String("fluctuation") || normalized == QLatin1String("warning")
        || normalized == QLatin1String("warn") || normalized == QLatin1String("unstable")
        || normalized == QLatin1String("degraded"))
        return QStringLiteral("波动");
    if (normalized == QLatin1String("failure") || normalized == QLatin1String("failed")
        || normalized == QLatin1String("fail") || normalized == QLatin1String("error"))
        return QStringLiteral("失败");
    return status;
}

QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setProperty("card", true);
    return card;
}
} // namespace

SystemTestPage::SystemTestPage(const StressRunConfig &config, QWidget *parent)
    : QWidget(parent), m_controller(new StressRunController(config, this))
{
    setObjectName(QStringLiteral("systemTestPage"));
    buildUi();
    connect(m_target, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SystemTestPage::updateDistribution);
    connect(m_start, &QPushButton::clicked, this, [this] {
        m_hasFinal = false;
        m_lastFinal = StressFinal{};
        m_lastProgress = StressProgress{};
        m_stableStarted = QDateTime{};
        m_countdownBar->setValue(0);
        m_countdownLabel->setText(QStringLiteral("稳定阶段倒计时：等待进入稳定阶段"));
        m_conclusion->setText(QStringLiteral("测试进行中"));
        m_conclusion->setProperty("state", QStringLiteral("running"));
        m_advice->setText(QStringLiteral("正在准备环境并启动 runner；关闭窗口会停止测试。"));
        refreshHistory();
        if (!m_controller->start(m_target->value()))
            setRunning(m_controller->isRunning());
    });
    connect(m_stop, &QPushButton::clicked, m_controller, &StressRunController::stop);
    connect(m_controller, &StressRunController::runStarted, this, [this] {
        setRunning(true);
        m_phase->setText(QStringLiteral("准备环境"));
        m_chunkSlots->setText(QStringLiteral("未知"));
    });
    connect(m_controller, &StressRunController::progressReceived,
            this, &SystemTestPage::updateProgress);
    connect(m_controller, &StressRunController::finalReceived,
            this, &SystemTestPage::updateFinal);
    connect(m_controller, &StressRunController::diagnostic, this, [this](const QString &message) {
        m_phase->setToolTip(message);
        if (!m_controller->hasFinal())
            m_advice->setText(message);
    });
    connect(m_controller, &StressRunController::runFinished,
            this, &SystemTestPage::finishRun);
    connect(m_openHtml, &QPushButton::clicked, this, &SystemTestPage::openSelectedHtml);
    connect(m_openDirectory, &QPushButton::clicked, this, &SystemTestPage::openSelectedDirectory);
    connect(m_history, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool selected = !m_history->selectedItems().isEmpty();
        m_openHtml->setEnabled(selected);
        m_openDirectory->setEnabled(selected);
    });
    updateDistribution();
    refreshHistory();
}

void SystemTestPage::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(14);

    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("系统压力测试"), content);
    title->setObjectName(QStringLiteral("stressTitle"));
    titles->addWidget(title);
    auto *subtitle = new QLabel(QStringLiteral("验证大规模在线、消息 ACK、文件传输与后端资源边界"), content);
    subtitle->setObjectName(QStringLiteral("mutedLabel"));
    subtitle->setWordWrap(true);
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch();
    m_phase = new QLabel(QStringLiteral("待命"), content);
    m_phase->setObjectName(QStringLiteral("stressPhaseLabel"));
    m_phase->setAlignment(Qt::AlignCenter);
    header->addWidget(m_phase, 0, Qt::AlignTop);
    layout->addLayout(header);

    auto *configuration = makeCard(content);
    auto *configLayout = new QVBoxLayout(configuration);
    configLayout->setContentsMargins(20, 16, 20, 16);
    configLayout->setSpacing(10);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    controls->addWidget(new QLabel(QStringLiteral("总人数"), configuration));
    m_target = new QSpinBox(configuration);
    m_target->setObjectName(QStringLiteral("stressTargetSpinBox"));
    m_target->setRange(1000, 100000);
    m_target->setSingleStep(1000);
    m_target->setValue(1000);
    m_target->setSuffix(QStringLiteral(" 人"));
    m_target->setGroupSeparatorShown(true);
    m_target->setAlignment(Qt::AlignCenter);
    m_target->setMinimumSize(150, 40);
    m_target->setButtonSymbols(QSpinBox::PlusMinus);
    m_target->setStyleSheet(QStringLiteral(
        "QSpinBox { font-size: 16px; font-weight: 600; padding: 6px 10px; }"
        "QSpinBox::up-button, QSpinBox::down-button {"
        " background: #e0e9ff; border: 1px solid #aebee8; width: 26px; color: #3658d2; }"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
        " background: #c7d6fb; }"
        "QSpinBox::up-button:pressed, QSpinBox::down-button:pressed {"
        " background: #aebfdc; }"));
    controls->addWidget(m_target);
    const QList<QPair<QString, int>> shortcuts{
        {QStringLiteral("1k"), 1000}, {QStringLiteral("5k"), 5000},
        {QStringLiteral("10k"), 10000}, {QStringLiteral("30k"), 30000}};
    for (const auto &shortcut : shortcuts) {
        auto *button = new QPushButton(shortcut.first, configuration);
        button->setObjectName(QStringLiteral("stressTarget%1").arg(shortcut.second));
        connect(button, &QPushButton::clicked, this, [this, value = shortcut.second] {
            m_target->setValue(value);
        });
        controls->addWidget(button);
    }
    controls->addSpacing(10);
    m_start = new QPushButton(QStringLiteral("开始测试"), configuration);
    m_start->setObjectName(QStringLiteral("stressTestButton"));
    m_start->setProperty("primary", true);
    controls->addWidget(m_start);
    m_stop = new QPushButton(QStringLiteral("停止"), configuration);
    m_stop->setObjectName(QStringLiteral("stressStopButton"));
    m_stop->setEnabled(false);
    controls->addWidget(m_stop);
    controls->addStretch();
    configLayout->addLayout(controls);
    m_distribution = new QLabel(configuration);
    m_distribution->setObjectName(QStringLiteral("stressDistributionLabel"));
    configLayout->addWidget(m_distribution);
    auto *resources = new QLabel(QStringLiteral(
        "固定资源：后端 4 CPU / 3 GiB ｜ 文件预算 500 MiB ｜ 分块 5 MiB ｜ "
        "400 个文件任务 ｜ 96 个分块通道 ｜ 单文件 100 MiB ｜ 稳定运行 5 分钟"), configuration);
    resources->setObjectName(QStringLiteral("stressResourceLabel"));
    resources->setWordWrap(true);
    configLayout->addWidget(resources);
    layout->addWidget(configuration);

    auto *countdownCard = makeCard(content);
    auto *countdownLayout = new QVBoxLayout(countdownCard);
    countdownLayout->setContentsMargins(20, 10, 20, 12);
    m_countdownLabel = new QLabel(QStringLiteral("稳定阶段倒计时：等待开始"), countdownCard);
    m_countdownLabel->setObjectName(QStringLiteral("stressCountdownLabel"));
    countdownLayout->addWidget(m_countdownLabel);
    m_countdownBar = new QProgressBar(countdownCard);
    m_countdownBar->setObjectName(QStringLiteral("stressCountdownBar"));
    m_countdownBar->setRange(0, kStableSeconds);
    m_countdownBar->setValue(0);
    m_countdownBar->setFormat(QStringLiteral("已进行 %v 秒 / 5 分钟"));
    countdownLayout->addWidget(m_countdownBar);
    layout->addWidget(countdownCard);

    m_countdownTimer = new QTimer(this);
    m_countdownTimer->setInterval(1000);
    connect(m_countdownTimer, &QTimer::timeout, this, &SystemTestPage::updateCountdown);
    m_countdownTimer->start();

    auto *metrics = new QGridLayout;
    metrics->setHorizontalSpacing(12);
    metrics->setVerticalSpacing(12);
    const auto metric = [content](const QString &titleText, const QString &name,
                                  QLabel **value, QLabel **detail = nullptr) {
        auto *card = makeCard(content);
        card->setObjectName(QStringLiteral("stressMetricCard"));
        card->setMinimumHeight(76);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *box = new QVBoxLayout(card);
        box->setContentsMargins(14, 10, 14, 10);
        box->setSpacing(3);
        auto *caption = new QLabel(titleText, card);
        caption->setObjectName(QStringLiteral("mutedLabel"));
        caption->setWordWrap(true);
        box->addWidget(caption);
        *value = new QLabel(QStringLiteral("—"), card);
        (*value)->setObjectName(name);
        (*value)->setProperty("metricValue", true);
        (*value)->setWordWrap(true);
        box->addWidget(*value);
        if (detail) {
            *detail = new QLabel(QStringLiteral("等待实时数据"), card);
            (*detail)->setObjectName(name + QStringLiteral("Detail"));
            (*detail)->setObjectName(QStringLiteral("mutedLabel"));
            (*detail)->setWordWrap(true);
            box->addWidget(*detail);
        }
        return card;
    };
    metrics->addWidget(metric(QStringLiteral("在线人数"), QStringLiteral("stressOnlineValue"),
                              &m_online), 0, 0);
    metrics->addWidget(metric(QStringLiteral("消息发送 / 送达"), QStringLiteral("stressMessagesValue"),
                              &m_messages, &m_messageDetail), 0, 1);
    metrics->addWidget(metric(QStringLiteral("文件完成 / 失败"), QStringLiteral("stressFilesValue"),
                              &m_files, &m_fileDetail), 0, 2);
    metrics->addWidget(metric(QStringLiteral("ACK P95 / P99"), QStringLiteral("stressLatencyValue"),
                              &m_latency), 0, 3);
    metrics->addWidget(metric(QStringLiteral("文件吞吐"), QStringLiteral("stressThroughputValue"),
                              &m_throughput, &m_bytes), 1, 0);
    metrics->addWidget(metric(QStringLiteral("后端 RSS / CPU"), QStringLiteral("stressBackendValue"),
                              &m_backend), 1, 1);
    metrics->addWidget(metric(QStringLiteral("Go 堆 / 协程数"), QStringLiteral("stressGoValue"),
                              &m_go), 1, 2);
    metrics->addWidget(metric(QStringLiteral("文件预算"), QStringLiteral("stressBudgetValue"),
                              &m_budget, &m_chunkSlots), 1, 3);
    m_chunkSlots->setObjectName(QStringLiteral("stressChunkSlotsValue"));
    m_chunkSlots->setText(QStringLiteral("—"));
    for (int column = 0; column < 4; ++column)
        metrics->setColumnStretch(column, 1);
    for (int row = 0; row < 2; ++row)
        metrics->setRowStretch(row, 1);
    layout->addLayout(metrics);

    auto *result = makeCard(content);
    result->setObjectName(QStringLiteral("stressResultCard"));
    auto *resultLayout = new QHBoxLayout(result);
    resultLayout->setContentsMargins(18, 12, 18, 12);
    m_conclusion = new QLabel(QStringLiteral("尚未运行"), result);
    m_conclusion->setObjectName(QStringLiteral("stressConclusionLabel"));
    m_conclusion->setProperty("state", QStringLiteral("idle"));
    m_conclusion->setAlignment(Qt::AlignCenter);
    resultLayout->addWidget(m_conclusion, 0, Qt::AlignTop);
    m_advice = new QLabel(QStringLiteral("选择人数后开始；报告和瓶颈建议会显示在这里。"), result);
    m_advice->setObjectName(QStringLiteral("stressAdviceLabel"));
    m_advice->setWordWrap(true);
    resultLayout->addWidget(m_advice, 1);
    layout->addWidget(result);

    auto *historyCard = makeCard(content);
    auto *historyLayout = new QVBoxLayout(historyCard);
    historyLayout->setContentsMargins(18, 12, 18, 14);
    auto *historyHeader = new QHBoxLayout;
    historyHeader->setSpacing(8);
    auto *historyTitle = new QLabel(QStringLiteral("历史报告"), historyCard);
    historyTitle->setObjectName(QStringLiteral("sectionTitle"));
    historyHeader->addWidget(historyTitle);
    historyHeader->addStretch();
    auto *historyHint = new QLabel(QStringLiteral("按时间倒序"), historyCard);
    historyHint->setObjectName(QStringLiteral("mutedLabel"));
    historyHeader->addWidget(historyHint);
    m_openHtml = new QPushButton(QStringLiteral("查看 HTML"), historyCard);
    m_openHtml->setObjectName(QStringLiteral("openStressHtmlButton"));
    m_openHtml->setEnabled(false);
    historyHeader->addWidget(m_openHtml);
    m_openDirectory = new QPushButton(QStringLiteral("打开文件夹"), historyCard);
    m_openDirectory->setObjectName(QStringLiteral("openStressReportButton"));
    m_openDirectory->setEnabled(false);
    historyHeader->addWidget(m_openDirectory);
    historyLayout->addLayout(historyHeader);
    m_history = new QTreeWidget(historyCard);
    m_history->setObjectName(QStringLiteral("stressHistoryList"));
    m_history->setHeaderLabels({QStringLiteral("时间"), QStringLiteral("人数"),
                                QStringLiteral("结论"), QStringLiteral("说明")});
    m_history->setRootIsDecorated(false);
    m_history->setAlternatingRowColors(true);
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->setMinimumHeight(120);
    m_history->setColumnWidth(0, 190);
    m_history->setColumnWidth(1, 90);
    m_history->setColumnWidth(2, 120);
    historyLayout->addWidget(m_history);
    layout->addWidget(historyCard);

    scroll->setWidget(content);
    outer->addWidget(scroll);
}

void SystemTestPage::updateDistribution()
{
    const qint64 target = m_target->value();
    const qint64 idleUsers = target * 80 / 100;
    const qint64 messagingUsers = target * 18 / 100;
    const qint64 fileUsers = target - idleUsers - messagingUsers;
    m_distribution->setText(QStringLiteral(
        "自动分配 80 / 18 / 2：空闲 %1 人 · 消息 %2 人 · 文件 %3 人")
        .arg(integerText(idleUsers), integerText(messagingUsers), integerText(fileUsers)));
}

void SystemTestPage::setRunning(bool running)
{
    m_target->setEnabled(!running);
    m_start->setEnabled(!running);
    m_stop->setEnabled(running);
    if (!running)
        m_chunkSlots->setText(QStringLiteral("—"));
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->objectName().startsWith(QStringLiteral("stressTarget")))
            button->setEnabled(!running);
    }
}

void SystemTestPage::updateCountdown()
{
    if (!m_countdownBar || !m_countdownLabel)
        return;
    if (!m_controller->isRunning()) {
        m_countdownLabel->setText(QStringLiteral("稳定阶段倒计时：测试已结束"));
        return;
    }
    if (m_stableStarted.isValid()) {
        const int elapsed = qBound(0, static_cast<int>(m_stableStarted.secsTo(QDateTime::currentDateTime())), kStableSeconds);
        m_countdownBar->setValue(elapsed);
        m_countdownLabel->setText(QStringLiteral("稳定阶段剩余 %1").arg(remainingText(kStableSeconds - elapsed)));
    } else {
        m_countdownBar->setValue(0);
        m_countdownLabel->setText(QStringLiteral("稳定阶段倒计时：准备与连接爬坡中"));
    }
}

void SystemTestPage::updateProgress(const StressProgress &progress)
{
    if (progress.phase == QStringLiteral("stable") && !m_stableStarted.isValid()) {
        m_stableStarted = QDateTime::currentDateTime();
        updateCountdown();
    }
    m_lastProgress = progress;
    m_phase->setText(progress.phase.isEmpty() ? QStringLiteral("运行中") : phaseDisplayText(progress.phase));
    m_online->setText(QStringLiteral("%1 / %2").arg(
        integerText(progress.online), integerText(progress.target)));
    m_messages->setText(QStringLiteral("%1 / %2").arg(
        integerText(progress.messagesSent), integerText(progress.messagesDelivered)));
    const double delivery = progress.messagesSent > 0
        ? 100.0 * static_cast<double>(progress.messagesDelivered)
            / static_cast<double>(progress.messagesSent) : 0.0;
    m_messageDetail->setText(QStringLiteral("送达率 %1%").arg(decimalText(delivery)));
    m_files->setText(QStringLiteral("%1 / %2").arg(
        integerText(progress.filesCompleted), integerText(progress.filesFailed)));
    m_fileDetail->setText(QStringLiteral("排队 %1 · 活动 %2")
        .arg(integerText(progress.filesQueued), integerText(progress.filesActive)));
    m_latency->setText(QStringLiteral("%1 / %2 ms")
        .arg(decimalText(progress.ackP95Ms), decimalText(progress.ackP99Ms)));
    m_throughput->setText(QStringLiteral("%1 MiB/s").arg(decimalText(progress.fileMibPerSec, 2)));
    m_bytes->setText(QStringLiteral("已完成 %1 MiB")
        .arg(decimalText(static_cast<double>(progress.bytesCompleted) / (1024.0 * 1024.0), 2)));
    m_backend->setText(QStringLiteral("%1 MiB / %2%")
        .arg(decimalText(progress.backendRssMib), decimalText(progress.backendCpuPercent)));
    m_go->setText(QStringLiteral("%1 MiB / %2")
        .arg(decimalText(progress.goHeapMib), integerText(progress.goroutines)));
    m_budget->setText(QStringLiteral("%1 / %2 MiB")
        .arg(decimalText(progress.fileBudgetUsedMib), decimalText(progress.fileBudgetLimitMib)));
    m_chunkSlots->setText(progress.fileChunkSlotsKnown
        ? QStringLiteral("分块通道 %1 / %2").arg(integerText(progress.fileChunkSlotsUsed),
                                      integerText(progress.fileChunkSlotsLimit))
        : QStringLiteral("分块通道未知"));
    if (!m_hasFinal)
        m_advice->setText(bottleneckAdvice());
}

void SystemTestPage::updateFinal(const StressFinal &result)
{
    m_hasFinal = true;
    m_lastFinal = result;
    if (m_lastProgress.target == 0)
        m_lastProgress.target = result.target;
    const QString conclusion = (result.conclusion + QLatin1Char(' ') + result.status).toLower();
    QString text = QStringLiteral("失败");
    QString state = QStringLiteral("failed");
    if (conclusion.contains(QStringLiteral("warn"))
        || conclusion.contains(QStringLiteral("fluct"))
        || conclusion.contains(QStringLiteral("unstable"))
        || conclusion.contains(QStringLiteral("degraded"))
        || conclusion.contains(QStringLiteral("波动"))) {
        text = QStringLiteral("波动");
        state = QStringLiteral("warning");
    } else if (conclusion.contains(QStringLiteral("success"))
               || conclusion.contains(QStringLiteral("pass"))
               || conclusion.contains(QStringLiteral("ok"))
               || conclusion.contains(QStringLiteral("成功"))) {
        text = QStringLiteral("成功");
        state = QStringLiteral("success");
    }
    m_conclusion->setText(text);
    m_conclusion->setProperty("state", state);
    m_conclusion->style()->unpolish(m_conclusion);
    m_conclusion->style()->polish(m_conclusion);
    m_advice->setText(bottleneckAdvice());
}

void SystemTestPage::finishRun(bool success, const QString &message)
{
    setRunning(false);
    refreshHistory();
    if (message.contains(QStringLiteral("已停止"))) {
        m_phase->setText(QStringLiteral("已停止"));
        if (!m_hasFinal) {
            m_conclusion->setText(QStringLiteral("已停止"));
            m_conclusion->setProperty("state", QStringLiteral("idle"));
        }
        m_advice->setText(QStringLiteral("测试已由用户停止；可调整人数后重新开始。"));
        return;
    }
    m_phase->setText(success ? QStringLiteral("已完成") : QStringLiteral("失败"));
    if (!m_hasFinal) {
        m_lastFinal.reason = message;
        m_conclusion->setText(QStringLiteral("失败"));
        m_conclusion->setProperty("state", QStringLiteral("failed"));
        m_advice->setText(message + QStringLiteral(" 请检查环境脚本、runner 路径及后端日志。"));
    } else {
        m_advice->setText(bottleneckAdvice());
    }
    m_conclusion->style()->unpolish(m_conclusion);
    m_conclusion->style()->polish(m_conclusion);
}

QString SystemTestPage::bottleneckAdvice() const
{
    return StressAssessment::bottleneckAdvice(m_lastProgress, m_lastFinal, m_conclusion->text());
}

void SystemTestPage::refreshHistory()
{
    if (!m_history)
        return;
    m_history->clear();
    const auto entries = StressReportRepository(m_controller->config().reportRoot).readHistory();
    for (const auto &entry : entries) {
        auto *item = new QTreeWidgetItem(m_history);
        item->setText(0, entry.timestamp.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        item->setText(1, integerText(entry.target));
        item->setText(2, entry.conclusion.isEmpty() ? QStringLiteral("—") : statusDisplayText(entry.conclusion));
        item->setText(3, entry.reason);
        item->setToolTip(3, entry.reason);
        item->setData(0, Qt::UserRole, entry.directory);
        item->setData(0, Qt::UserRole + 1, entry.html);
    }
    m_openHtml->setEnabled(false);
    m_openDirectory->setEnabled(false);
}

void SystemTestPage::openSelectedHtml()
{
    const auto selected = m_history->selectedItems();
    if (selected.isEmpty())
        return;
    const QString html = selected.first()->data(0, Qt::UserRole + 1).toString();
    if (!html.isEmpty() && !openReportUrl(QUrl::fromLocalFile(html)))
        m_advice->setText(QStringLiteral("无法打开 HTML 报告，请改用“打开文件夹”。"));
}

void SystemTestPage::openSelectedDirectory()
{
    const auto selected = m_history->selectedItems();
    if (selected.isEmpty())
        return;
    const QString directory = selected.first()->data(0, Qt::UserRole).toString();
    if (!directory.isEmpty())
        openReportUrl(QUrl::fromLocalFile(directory));
}

bool SystemTestPage::openReportUrl(const QUrl &url)
{
    if (s_reportOpener)
        return s_reportOpener(url);
    return QDesktopServices::openUrl(url);
}

void SystemTestPage::setReportOpenerForTests(ReportOpener opener)
{
    s_reportOpener = std::move(opener);
}

void SystemTestPage::shutdown()
{
    if (m_countdownTimer)
        m_countdownTimer->stop();
    m_controller->stopAndWait();
}
