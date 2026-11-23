#include "StressAssessment.h"
#include "StressNdjsonCodec.h"
#include "StressReportRepository.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
void writeSummary(const QString &root, const QString &name, const QJsonObject &summary)
{
    const QString directory = QDir(root).filePath(name);
    QVERIFY(QDir().mkpath(directory));
    QFile file(QDir(directory).filePath(QStringLiteral("summary.json")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QJsonDocument(summary).toJson(QJsonDocument::Compact)),
             QJsonDocument(summary).toJson(QJsonDocument::Compact).size());
}
} // namespace

class ApplicationLogicTest final : public QObject {
    Q_OBJECT
private slots:
    void decodesNestedAndFlatNdjson();
    void rejectsInvalidAndIgnoresUnknownNdjson();
    void readsReportSchemasAndPreservesSorting();
    void preservesAssessmentPriorityAndThresholds();
};

void ApplicationLogicTest::decodesNestedAndFlatNdjson()
{
    const auto nested = StressNdjsonCodec::decodeLine(R"({"type":"progress","progress":{
        "phase":"stable","target":1000,"online":999,"file_chunk_slots_used":7,
        "file_chunk_slots_limit":96}})");
    QVERIFY(nested.type == StressNdjsonCodec::EventType::Progress);
    QCOMPARE(nested.progress.phase, QStringLiteral("stable"));
    QCOMPARE(nested.progress.target, qint64(1000));
    QCOMPARE(nested.progress.online, qint64(999));
    QVERIFY(nested.progress.fileChunkSlotsKnown);
    QCOMPARE(nested.progress.fileChunkSlotsUsed, qint64(7));
    QCOMPARE(nested.progress.fileChunkSlotsLimit, qint64(96));

    const auto legacy = StressNdjsonCodec::decodeLine(
        R"({"type":"progress","phase":"rampup","file_chunk_slots_used":7})");
    QVERIFY(legacy.type == StressNdjsonCodec::EventType::Progress);
    QVERIFY(!legacy.progress.fileChunkSlotsKnown);

    const auto final = StressNdjsonCodec::decodeLine(R"({"type":"final","final":{
        "status":"warning","conclusion":"波动","report_dir":"run-1","target":1000,
        "reason":"检查消息持久化"}})");
    QVERIFY(final.type == StressNdjsonCodec::EventType::Final);
    QCOMPARE(final.final.status, QStringLiteral("warning"));
    QCOMPARE(final.final.conclusion, QStringLiteral("波动"));
    QCOMPARE(final.final.reportDir, QStringLiteral("run-1"));
    QCOMPARE(final.final.target, qint64(1000));
}

void ApplicationLogicTest::rejectsInvalidAndIgnoresUnknownNdjson()
{
    QVERIFY(StressNdjsonCodec::decodeLine(" \r\n").type
            == StressNdjsonCodec::EventType::Ignored);
    QVERIFY(StressNdjsonCodec::decodeLine(R"({"type":"diagnostic"})").type
            == StressNdjsonCodec::EventType::Ignored);
    const auto invalid = StressNdjsonCodec::decodeLine("{bad");
    QVERIFY(invalid.type == StressNdjsonCodec::EventType::Invalid);
    QVERIFY(!invalid.error.isEmpty());
}

void ApplicationLogicTest::readsReportSchemasAndPreservesSorting()
{
    QTemporaryDir root;
    writeSummary(root.path(), QStringLiteral("older"), QJsonObject{
        {QStringLiteral("started_at"), QStringLiteral("2026-11-12T08:00:00Z")},
        {QStringLiteral("target"), 5000}, {QStringLiteral("conclusion"), QStringLiteral("failed")},
        {QStringLiteral("html"), QStringLiteral("index.html")},
        {QStringLiteral("result"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("older reason")}}}});
    writeSummary(root.path(), QStringLiteral("newer"), QJsonObject{
        {QStringLiteral("final"), QJsonObject{
            {QStringLiteral("timestamp"), 1794560400},
            {QStringLiteral("target"), 10000}, {QStringLiteral("status"), QStringLiteral("warning")},
            {QStringLiteral("reason"), QStringLiteral("newer reason")}}}});

    const auto entries = StressReportRepository(root.path()).readHistory();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).target, qint64(10000));
    QCOMPARE(entries.at(0).conclusion, QStringLiteral("warning"));
    QCOMPARE(entries.at(0).reason, QStringLiteral("newer reason"));
    QVERIFY(entries.at(0).html.endsWith(QStringLiteral("newer/report.html")));
    QCOMPARE(entries.at(1).target, qint64(5000));
    QCOMPARE(entries.at(1).conclusion, QStringLiteral("failed"));
    QVERIFY(entries.at(1).html.endsWith(QStringLiteral("older/index.html")));
    QVERIFY(entries.at(0).timestamp > entries.at(1).timestamp);
}

void ApplicationLogicTest::preservesAssessmentPriorityAndThresholds()
{
    StressProgress progress;
    StressFinal final;
    final.conclusion = QStringLiteral("失败；建议：检查消息持久化");
    QCOMPARE(StressAssessment::bottleneckAdvice(progress, final, QStringLiteral("失败")),
             QStringLiteral("瓶颈建议：检查消息持久化"));

    final = StressFinal{};
    progress.filesFailed = 1;
    QVERIFY(StressAssessment::bottleneckAdvice(progress, final, QStringLiteral("失败"))
            .contains(QStringLiteral("文件失败")));
    progress = StressProgress{};
    progress.target = 1000;
    progress.online = 999;
    QVERIFY(StressAssessment::bottleneckAdvice(progress, final, QStringLiteral("失败"))
            .contains(QStringLiteral("在线人数")));
    progress.online = 1000;
    progress.ackP99Ms = 2000.0;
    QVERIFY(StressAssessment::bottleneckAdvice(progress, final, QStringLiteral("波动"))
            .contains(QStringLiteral("ACK P99")));
    progress = StressProgress{};
    QVERIFY(StressAssessment::bottleneckAdvice(progress, final, QStringLiteral("波动"))
            .contains(QStringLiteral("结果存在波动")));
}

QTEST_GUILESS_MAIN(ApplicationLogicTest)
#include "test_application_logic.moc"
