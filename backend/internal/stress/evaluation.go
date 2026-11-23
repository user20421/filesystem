package stress

import (
	"fmt"
	"strings"
)

type RunFacts struct {
	StableFull, TargetReached, Interrupted, DataError, Drained bool
	CleanupError                                               string
	FailureReason                                              string
	BackendRSSMaxMiB                                           *float64
	BackendCPUMax                                              *float64
}

type Result struct {
	Status      string          `json:"status"`
	Conclusion  string          `json:"conclusion"`
	Reason      string          `json:"reason"`
	Criteria    map[string]bool `json:"criteria"`
	Bottlenecks []string        `json:"bottlenecks"`
}

// Evaluate 按连接、消息、文件、延迟、资源和数据完整性判定结果，不写任何 artifact。
func Evaluate(cfg Config, t Totals, f RunFacts) Result {
	connections := t.StableSamples > 0 && t.StableOnlineAverage >= float64(cfg.Target)*.99
	messages := t.MessagesSent == 0 || float64(t.MessagesDelivered)/float64(t.MessagesSent) >= .999
	allocation, _ := Allocate(cfg.Target, cfg.IdlePercent, cfg.ChatPercent, cfg.FilePercent)
	files := t.FilesFailed == 0 && (allocation.File == 0 || t.FilesCompleted == int64(allocation.File)) && f.Drained
	budget := !t.FileBudgetKnown || t.FileBudgetHighMiB <= float64(cfg.FileBudgetMiB)
	latency := t.AckP95MS <= 2000 && t.AckP99MS <= 5000
	resource := f.BackendRSSMaxMiB == nil || *f.BackendRSSMaxMiB <= float64(cfg.BackendMemoryLimitMiB)
	criteria := map[string]bool{"stable_duration": f.StableFull && !f.Interrupted, "connections_99_percent": connections && f.TargetReached,
		"messages_99_9_percent": messages, "files_100_percent": files, "ack_latency": latency, "no_health_failure": t.HealthFailures == 0,
		"no_abnormal_duplicates": t.MessageDuplicates == 0, "backend_memory_limit": resource, "file_budget": budget,
		"data_integrity": !f.DataError, "cleanup": f.CleanupError == ""}
	r := Result{Criteria: criteria}
	var failures, fluctuations []string
	if !criteria["stable_duration"] {
		failures = append(failures, "目标连接未完整维持稳定时长或运行被提前停止")
	}
	if !criteria["connections_99_percent"] {
		failures = append(failures, "稳定期在线连接低于目标的99%")
	}
	if !messages {
		failures = append(failures, "消息投递率低于99.9%")
	}
	if !files {
		failures = append(failures, "文件未全部完成或收尾队列未排空")
	}
	if t.MessageDuplicates > 0 || f.DataError {
		failures = append(failures, "发现异常重复或数据校验错误")
	}
	if t.HealthFailures > 0 {
		failures = append(failures, "后端健康检查失败")
	}
	if !budget {
		failures = append(failures, "文件内存预算超限")
	}
	if !latency {
		fluctuations = append(fluctuations, "ACK延迟超过P95 2秒或P99 5秒")
	}
	if !resource {
		fluctuations = append(fluctuations, fmt.Sprintf("后端RSS超过%d MiB", cfg.BackendMemoryLimitMiB))
	}
	if t.FileQueueP95MS > 5000 {
		fluctuations = append(fluctuations, "文件排队P95超过5秒")
	}
	if f.CleanupError != "" {
		fluctuations = append(fluctuations, "测试数据清理失败: "+f.CleanupError)
	}
	if f.FailureReason != "" {
		failures = append(failures, f.FailureReason)
	}
	switch {
	case len(failures) > 0:
		r.Status = "failure"
		r.Reason = strings.Join(failures, "；")
	case len(fluctuations) > 0:
		r.Status = "fluctuation"
		r.Reason = strings.Join(fluctuations, "；")
	default:
		r.Status = "success"
		r.Reason = "全部成功阈值满足"
	}
	if t.AckP95MS > 2000 || t.AckP99MS > 5000 {
		r.Bottlenecks = append(r.Bottlenecks, "检查消息持久化、MySQL锁等待与WebSocket写队列")
	}
	if t.FileQueueP95MS > 5000 {
		r.Bottlenecks = append(r.Bottlenecks, "降低文件生产速率或增加后端上传并发，检查MinIO吞吐")
	}
	if f.BackendRSSMaxMiB != nil && *f.BackendRSSMaxMiB > float64(cfg.BackendMemoryLimitMiB) {
		r.Bottlenecks = append(r.Bottlenecks, "检查后端文件缓冲、连接对象与Go堆/GC")
	}
	if t.Reconnects > 0 {
		r.Bottlenecks = append(r.Bottlenecks, "检查慢消费者断连、代理超时和后端WebSocket错误")
	}
	reason := strings.ToLower(f.FailureReason)
	downloadTimeout := strings.Contains(reason, "download") && strings.Contains(reason, "i/o timeout")
	messageRejected := strings.Contains(reason, "message_rejected")
	if downloadTimeout {
		r.Bottlenecks = append(r.Bottlenecks, "文件下载发生i/o timeout：按文件ID检查下载读/写空闲超时、客户端断开及MinIO读取，不要按总下载时长强制断连")
	}
	if messageRejected {
		r.Bottlenecks = append(r.Bottlenecks, "消息被服务端message_rejected：按ERROR中的message_id关联后端日志，检查recipient校验、会话查询/创建、消息写入或重复查询阶段及原始SQL错误")
	}
	if t.FilesFailed > 0 && !downloadTimeout {
		r.Bottlenecks = append(r.Bottlenecks, "检查文件active限制、分块校验和对象存储错误")
	}
	if t.MessageLost > 0 && !messageRejected {
		r.Bottlenecks = append(r.Bottlenecks, "检查ACK/推送丢失、连接重连和消息唯一索引")
	}
	if len(r.Bottlenecks) == 0 {
		r.Bottlenecks = append(r.Bottlenecks, "未观察到明确瓶颈；可逐步提高连接数或文件吞吐并继续观察")
	}
	if f.BackendRSSMaxMiB == nil || !t.FileBudgetKnown {
		r.Bottlenecks = append(r.Bottlenecks, "后端/docker资源或文件预算数据未知，不纳入伪造统计；请提供/debug/stats及docker权限")
	}
	r.Conclusion = fmt.Sprintf("状态=%s；%s。建议：%s", statusText(r.Status), r.Reason, strings.Join(r.Bottlenecks, "；"))
	return r
}
