#include "StressAssessment.h"

namespace StressAssessment {
QString bottleneckAdvice(const StressProgress &progress, const StressFinal &final,
                         const QString &displayedConclusion)
{
    const qsizetype suggestion = final.conclusion.indexOf(QStringLiteral("建议："));
    if (suggestion >= 0)
        return QStringLiteral("瓶颈建议：%1").arg(final.conclusion.mid(suggestion + 3));
    if (!final.reason.trimmed().isEmpty()
        && final.reason != QStringLiteral("全部成功阈值满足"))
        return QStringLiteral("瓶颈建议：%1").arg(final.reason);
    if (progress.filesFailed > 0
        || (progress.fileBudgetLimitMib > 0
            && progress.fileBudgetUsedMib >= progress.fileBudgetLimitMib * 0.9))
        return QStringLiteral("瓶颈建议：文件失败或预算接近上限，检查 500 MiB 配额、96 通道限制及存储吞吐。");
    if (progress.target > 0 && progress.online < progress.target)
        return QStringLiteral("瓶颈建议：在线人数未达目标，优先检查连接建立、账号准备和后端连接容量。");
    if (progress.ackP99Ms >= 2000.0)
        return QStringLiteral("瓶颈建议：ACK P99 偏高，检查后端 CPU、Go 堆和协程数是否持续增长。");
    if (progress.backendCpuPercent >= 85.0 || progress.backendRssMib >= 3584.0)
        return QStringLiteral("瓶颈建议：后端资源接近 6 CPU / 3 GiB 边界，降低人数并对比资源峰值。");
    if (progress.messagesDelivered < progress.messagesSent)
        return QStringLiteral("瓶颈建议：消息存在未送达，检查 WebSocket 稳定性、ACK 延迟和错误日志。");
    if (displayedConclusion == QStringLiteral("波动"))
        return QStringLiteral("瓶颈建议：结果存在波动，请复测并对比在线率、P99、送达率和资源峰值。");
    return QStringLiteral("瓶颈建议：未观察到明显瓶颈；可逐步增加人数，持续观察 P99、RSS 与文件失败数。");
}
} // namespace StressAssessment
