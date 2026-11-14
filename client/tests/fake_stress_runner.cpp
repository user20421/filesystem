#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace {
void writeAll(QFile *output, const QByteArray &data)
{
    output->write(data);
    output->flush();
}

QString argument(const QStringList &arguments, const QString &name)
{
    const qsizetype index = arguments.indexOf(name);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

void writeFakeReport(const QStringList &arguments)
{
    const QString target = argument(arguments, QStringLiteral("--target"));
    const QString directory = QDir(argument(arguments, QStringLiteral("--report-root")))
        .filePath(QStringLiteral("run-%1").arg(target));
    QDir().mkpath(directory);
    QFile html(QDir(directory).filePath(QStringLiteral("index.html")));
    if (html.open(QIODevice::WriteOnly))
        html.write("<html><body>fake stress report</body></html>");
    const QJsonObject summary{
        {QStringLiteral("started_at"), QStringLiteral("2026-11-14T10:00:00Z")},
        {QStringLiteral("target"), target.toLongLong()},
        {QStringLiteral("status"), QStringLiteral("success")},
        {QStringLiteral("conclusion"), QStringLiteral("success")},
        {QStringLiteral("html"), QStringLiteral("index.html")},
        {QStringLiteral("reason"), QString()}
    };
    QFile summaryFile(QDir(directory).filePath(QStringLiteral("summary.json")));
    if (summaryFile.open(QIODevice::WriteOnly))
        summaryFile.write(QJsonDocument(summary).toJson(QJsonDocument::Compact));
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
    const QString capture = qEnvironmentVariable("SECURE_IM_FAKE_ARGUMENTS_FILE");
    if (!capture.isEmpty()) {
        QFile file(capture);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(QJsonArray::fromStringList(arguments))
                .toJson(QJsonDocument::Compact));
        }
    }

    const qsizetype scriptIndex = arguments.indexOf(QStringLiteral("-File"));
    if (scriptIndex >= 0 && scriptIndex + 1 < arguments.size()) {
        QFile marker(qEnvironmentVariable("SECURE_IM_FAKE_SETUP_FILE"));
        if (marker.open(QIODevice::WriteOnly))
            marker.write(arguments.at(scriptIndex + 1).toUtf8());
        return 0;
    }

    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 90;
    const QString scenario = qEnvironmentVariable("SECURE_IM_FAKE_STRESS_SCENARIO",
        QStringLiteral("sticky"));
    if (scenario == QLatin1String("hold")) {
        writeAll(&output, "{\"type\":\"progress\",\"progress\":{\"phase\":\"steady\",\"target\":1000,\"online\":800}}\n");
        return QCoreApplication::exec();
    }
    if (scenario == QLatin1String("nonzero")) {
        writeAll(&output, "{\"type\":\"final\",\"final\":{\"status\":\"failed\",\"conclusion\":\"failed\",\"target\":7,\"reason\":\"injected failure\"}}\n");
        QTimer::singleShot(30, &application, [] { QCoreApplication::exit(7); });
        return QCoreApplication::exec();
    }
    if (scenario == QLatin1String("missing-final")) {
        writeAll(&output, "{\"type\":\"progress\",\"progress\":{\"phase\":\"done\",\"target\":9,\"online\":9}}\r\n");
        QTimer::singleShot(30, &application, &QCoreApplication::quit);
        return QCoreApplication::exec();
    }
    if (scenario == QLatin1String("fluctuation")) {
        writeAll(&output, "{\"type\":\"final\",\"status\":\"fluctuation\","
            "\"conclusion\":\"状态=fluctuation；ACK延迟偏高。建议：检查消息持久化\","
            "\"target\":1000,\"reason\":\"ACK延迟偏高\"}\n");
        QTimer::singleShot(30, &application, &QCoreApplication::quit);
        return QCoreApplication::exec();
    }

    writeAll(&output, "{\"type\":\"progress\",\"progress\":{\"phase\":\"conn");
    QTimer::singleShot(20, &application, [&output, arguments] {
        writeAll(&output,
            "ected\",\"target\":1000,\"online\":997,\"messages_sent\":12000,"
            "\"messages_delivered\":11990,\"ack_p95_ms\":42.5,\"ack_p99_ms\":91.25,"
            "\"files_queued\":3,\"files_active\":4,\"files_completed\":55,\"files_failed\":1,"
            "\"bytes_completed\":10485760,\"file_mib_per_sec\":12.5,\"backend_rss_mib\":768.5,"
            "\"backend_cpu_percent\":63.25,\"go_heap_mib\":128.75,\"goroutines\":345,"
            "\"file_budget_used_mib\":120.5,\"file_budget_limit_mib\":500}}\r\n"
            "{this is not json}\r\n"
            "{\"type\":\"progress\",\"phase\":\"steady\",\"target\":1000,\"online\":1000,\"files_failed\":1}\r\n");
        writeFakeReport(arguments);
    });
    QTimer::singleShot(50, &application, [&output] {
        // Deliberately omit the trailing newline: finished() must drain this final half line.
        writeAll(&output, "{\"type\":\"final\",\"status\":\"success\","
            "\"conclusion\":\"success\",\"report_dir\":\"run-1000\",\"html\":\"index.html\","
            "\"target\":1000,\"reason\":\"\"}");
    });
    QTimer::singleShot(80, &application, &QCoreApplication::quit);
    return QCoreApplication::exec();
}
