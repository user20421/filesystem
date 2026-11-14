package stress

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"html/template"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
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

func Evaluate(cfg Config, t Totals, f RunFacts) Result {
	connections := t.StableSamples > 0 && t.StableOnlineAverage >= float64(cfg.Target)*.99
	messages := t.MessagesSent == 0 || float64(t.MessagesDelivered)/float64(t.MessagesSent) >= .999
	allocation, _ := Allocate(cfg.Target, cfg.IdlePercent, cfg.ChatPercent, cfg.FilePercent)
	files := t.FilesFailed == 0 && (allocation.File == 0 || t.FilesCompleted > 0) && f.Drained
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
	if t.FilesFailed > 0 {
		r.Bottlenecks = append(r.Bottlenecks, "检查文件active限制、分块校验和对象存储错误")
	}
	if t.MessageLost > 0 {
		r.Bottlenecks = append(r.Bottlenecks, "检查ACK/推送丢失、连接重连和消息唯一索引")
	}
	if len(r.Bottlenecks) == 0 {
		r.Bottlenecks = append(r.Bottlenecks, "未观察到明确瓶颈；可逐步提高连接数或文件吞吐并继续观察")
	}
	if f.BackendRSSMaxMiB == nil || !t.FileBudgetKnown {
		r.Bottlenecks = append(r.Bottlenecks, "后端/docker资源或文件预算数据未知，不纳入伪造统计；请提供/debug/stats及docker权限")
	}
	r.Conclusion = fmt.Sprintf("状态=%s；%s。建议：%s", r.Status, r.Reason, strings.Join(r.Bottlenecks, "；"))
	return r
}

type Summary struct {
	RunID            string     `json:"run_id"`
	Status           string     `json:"status"`
	Target           int        `json:"target"`
	StartedAt        time.Time  `json:"started_at"`
	FinishedAt       time.Time  `json:"finished_at"`
	Allocation       Allocation `json:"allocation"`
	Totals           Totals     `json:"totals"`
	Result           Result     `json:"result"`
	CleanupError     string     `json:"cleanup_error,omitempty"`
	BackendRSSMaxMiB *float64   `json:"backend_rss_max_mib"`
	BackendCPUMax    *float64   `json:"backend_cpu_max_percent"`
}

type Report struct {
	mu      sync.Mutex
	Dir     string
	runID   string
	started time.Time
	csvFile *os.File
	csv     *csv.Writer
}

func NewReport(root, runID string, cfg Config) (*Report, error) {
	dir := filepath.Join(root, runID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	file, err := os.Create(filepath.Join(dir, "metrics.csv"))
	if err != nil {
		return nil, err
	}
	r := &Report{Dir: dir, runID: runID, started: time.Now().UTC(), csvFile: file, csv: csv.NewWriter(file)}
	if err := r.writeHeader(); err != nil {
		file.Close()
		return nil, err
	}
	cfg.RunID = runID
	body, _ := json.MarshalIndent(cfg, "", "  ")
	body = append(body, '\n')
	if err := os.WriteFile(filepath.Join(dir, "config.json"), body, 0o644); err != nil {
		file.Close()
		return nil, err
	}
	return r, nil
}

func (r *Report) writeHeader() error {
	return r.csv.Write([]string{"timestamp", "phase", "target", "online", "connection_failed", "reconnects", "messages_sent", "messages_delivered", "message_duplicates", "message_lost", "ack_p95_ms", "ack_p99_ms", "delivery_p95_ms", "files_queued", "files_active", "files_completed", "files_failed", "bytes_completed", "file_mib_per_sec", "file_queue_p95_ms", "backend_rss_mib", "backend_cpu_percent", "go_heap_mib", "goroutines", "file_budget_used_mib", "file_budget_limit_mib"})
}

func optional(v *float64) string {
	if v == nil {
		return "unknown"
	}
	return strconv.FormatFloat(*v, 'f', 3, 64)
}
func number(v float64) string { return strconv.FormatFloat(v, 'f', 3, 64) }
func optionalInt(v *int) string {
	if v == nil {
		return "unknown"
	}
	return strconv.Itoa(*v)
}

func (r *Report) AddProgress(p Progress) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	err := r.csv.Write([]string{p.Timestamp.Format(time.RFC3339Nano), p.Phase, strconv.Itoa(p.Target), strconv.FormatInt(p.Online, 10), strconv.FormatInt(p.ConnectionFailed, 10), strconv.FormatInt(p.Reconnects, 10), strconv.FormatInt(p.MessagesSent, 10), strconv.FormatInt(p.MessagesDelivered, 10), strconv.FormatInt(p.MessageDuplicates, 10), strconv.FormatInt(p.MessageLost, 10), number(p.AckP95MS), number(p.AckP99MS), number(p.DeliveryP95MS), strconv.FormatInt(p.FilesQueued, 10), strconv.FormatInt(p.FilesActive, 10), strconv.FormatInt(p.FilesCompleted, 10), strconv.FormatInt(p.FilesFailed, 10), strconv.FormatInt(p.BytesCompleted, 10), number(p.FileMiBPerSec), number(p.FileQueueP95MS), optional(p.BackendRSSMiB), optional(p.BackendCPUPercent), optional(p.GoHeapMiB), optionalInt(p.Goroutines), optional(p.FileBudgetUsedMiB), optional(p.FileBudgetLimitMiB)})
	r.csv.Flush()
	if err == nil {
		err = r.csv.Error()
	}
	return err
}

func (r *Report) Finalize(cfg Config, allocation Allocation, totals Totals, facts RunFacts, result Result) (Summary, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.csvFile != nil {
		r.csv.Flush()
		_ = r.csvFile.Close()
		r.csvFile = nil
	}
	summary := Summary{RunID: r.runID, Status: result.Status, Target: cfg.Target, StartedAt: r.started, FinishedAt: time.Now().UTC(), Allocation: allocation, Totals: totals, Result: result, CleanupError: facts.CleanupError, BackendRSSMaxMiB: facts.BackendRSSMaxMiB, BackendCPUMax: facts.BackendCPUMax}
	body, err := json.MarshalIndent(summary, "", "  ")
	if err != nil {
		return summary, err
	}
	body = append(body, '\n')
	if err = os.WriteFile(filepath.Join(r.Dir, "summary.json"), body, 0o644); err != nil {
		return summary, err
	}
	if err = os.WriteFile(filepath.Join(r.Dir, "conclusion.txt"), []byte(result.Conclusion+"\n"), 0o644); err != nil {
		return summary, err
	}
	var page strings.Builder
	err = reportTemplate.Execute(&page, struct {
		Summary   Summary
		Config    Config
		Generated time.Time
	}{summary, cfg, time.Now().UTC()})
	if err != nil {
		return summary, err
	}
	if err = os.WriteFile(filepath.Join(r.Dir, "report.html"), []byte(page.String()), 0o644); err != nil {
		return summary, err
	}
	return summary, nil
}

func (r *Report) Close() {
	r.mu.Lock()
	if r.csvFile != nil {
		r.csv.Flush()
		_ = r.csvFile.Close()
		r.csvFile = nil
	}
	r.mu.Unlock()
}

var reportTemplate = template.Must(template.New("report").Funcs(template.FuncMap{"optional": optional}).Parse(`<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>Stress {{.Summary.RunID}}</title>
<style>body{font-family:system-ui;margin:2rem;color:#202124}table{border-collapse:collapse;margin:1rem 0}td,th{border:1px solid #ccc;padding:.45rem .7rem;text-align:left}.success{color:#087f23}.failure{color:#b3261e}.fluctuation{color:#9a6700}code{word-break:break-all}</style></head>
<body><h1>Secure IM 压测报告</h1><p>Run ID: <code>{{.Summary.RunID}}</code></p><p>生成时间: {{.Generated.Format "2006-01-02T15:04:05Z07:00"}}</p>
<h2 class="{{.Summary.Status}}">{{.Summary.Status}}</h2><p>{{.Summary.Result.Conclusion}}</p>
<h2>负载</h2><table><tr><th>目标</th><td>{{.Summary.Target}}</td><th>挂机/聊天/文件</th><td>{{.Summary.Allocation.Idle}} / {{.Summary.Allocation.Chat}} / {{.Summary.Allocation.File}}</td></tr>
<tr><th>稳定时长</th><td>{{.Config.StableDuration}}</td><th>聊天速率</th><td>{{.Config.ChatRate}} 条/s/user</td></tr>
<tr><th>文件</th><td>{{.Config.FileSize}} bytes / {{.Config.ChunkSize}} bytes chunks</td><th>worker/预算</th><td>{{.Config.FileWorkers}} / {{.Config.FileBudgetMiB}} MiB</td></tr></table>
<h2>判定</h2><table><tr><th>规则</th><th>结果</th></tr>{{range $name,$ok := .Summary.Result.Criteria}}<tr><td>{{$name}}</td><td>{{if $ok}}PASS{{else}}FAIL{{end}}</td></tr>{{end}}</table>
<h2>指标</h2><table><tr><th>消息 sent/delivered/lost/duplicates</th><td>{{.Summary.Totals.MessagesSent}} / {{.Summary.Totals.MessagesDelivered}} / {{.Summary.Totals.MessageLost}} / {{.Summary.Totals.MessageDuplicates}}</td></tr>
<tr><th>ACK P95/P99 ms</th><td>{{printf "%.3f" .Summary.Totals.AckP95MS}} / {{printf "%.3f" .Summary.Totals.AckP99MS}}</td><th>投递 P95 ms</th><td>{{printf "%.3f" .Summary.Totals.DeliveryP95MS}}</td></tr>
<tr><th>文件 completed/failed/bytes</th><td>{{.Summary.Totals.FilesCompleted}} / {{.Summary.Totals.FilesFailed}} / {{.Summary.Totals.BytesCompleted}}</td><th>排队 P95 ms</th><td>{{printf "%.3f" .Summary.Totals.FileQueueP95MS}}</td></tr>
<tr><th>后端 RSS max MiB / CPU max %</th><td>{{optional .Summary.BackendRSSMaxMiB}} / {{optional .Summary.BackendCPUMax}}</td><th>文件预算 high/limit MiB</th><td>{{if .Summary.Totals.FileBudgetKnown}}{{printf "%.3f" .Summary.Totals.FileBudgetHighMiB}} / {{printf "%.3f" .Summary.Totals.FileBudgetLimitMiB}}{{else}}unknown{{end}}</td></tr>
<tr><th>稳定在线 avg/min</th><td>{{printf "%.2f" .Summary.Totals.StableOnlineAverage}} / {{.Summary.Totals.StableOnlineMin}}</td><th>重连/连接失败/健康失败</th><td>{{.Summary.Totals.Reconnects}} / {{.Summary.Totals.ConnectionFailed}} / {{.Summary.Totals.HealthFailures}}</td></tr></table>
<p>详细每秒数据见 <a href="metrics.csv">metrics.csv</a>；原始汇总见 <a href="summary.json">summary.json</a>。</p></body></html>`))
