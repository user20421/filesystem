#pragma once

#include "StressNdjsonCodec.h"

#include <QString>

// StressAssessment 从最终结论和最后一帧指标生成既有瓶颈建议，不依赖 Widgets。
namespace StressAssessment {
QString bottleneckAdvice(const StressProgress &progress, const StressFinal &final,
                         const QString &displayedConclusion);
} // namespace StressAssessment
