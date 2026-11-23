package stress

import (
	_ "embed"
	"encoding/csv"
	"encoding/json"
	"html/template"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

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
	mu                                      sync.Mutex
	Dir                                     string
	runID                                   string
	started                                 time.Time
	csvFile                                 *os.File
	csv                                     *csv.Writer
	lastChunkSlotsUsed, lastChunkSlotsLimit *int64
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
	return r.csv.Write([]string{"timestamp", "phase", "target", "online", "connection_failed", "reconnects", "messages_sent", "messages_delivered", "message_duplicates", "message_lost", "ack_p95_ms", "ack_p99_ms", "delivery_p95_ms", "files_queued", "files_active", "files_completed", "files_failed", "bytes_completed", "file_mib_per_sec", "file_queue_p95_ms", "backend_rss_mib", "backend_cpu_percent", "go_heap_mib", "goroutines", "file_budget_used_mib", "file_budget_limit_mib", "file_chunk_slots_used", "file_chunk_slots_limit"})
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

func optionalInt64(v *int64) string {
	if v == nil {
		return "unknown"
	}
	return strconv.FormatInt(*v, 10)
}

func (r *Report) AddProgress(p Progress) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if p.FileChunkSlotsUsed != nil && p.FileChunkSlotsLimit != nil {
		r.lastChunkSlotsUsed = cloneInt64(p.FileChunkSlotsUsed)
		r.lastChunkSlotsLimit = cloneInt64(p.FileChunkSlotsLimit)
	}
	err := r.csv.Write([]string{p.Timestamp.Format(time.RFC3339Nano), p.Phase, strconv.Itoa(p.Target), strconv.FormatInt(p.Online, 10), strconv.FormatInt(p.ConnectionFailed, 10), strconv.FormatInt(p.Reconnects, 10), strconv.FormatInt(p.MessagesSent, 10), strconv.FormatInt(p.MessagesDelivered, 10), strconv.FormatInt(p.MessageDuplicates, 10), strconv.FormatInt(p.MessageLost, 10), number(p.AckP95MS), number(p.AckP99MS), number(p.DeliveryP95MS), strconv.FormatInt(p.FilesQueued, 10), strconv.FormatInt(p.FilesActive, 10), strconv.FormatInt(p.FilesCompleted, 10), strconv.FormatInt(p.FilesFailed, 10), strconv.FormatInt(p.BytesCompleted, 10), number(p.FileMiBPerSec), number(p.FileQueueP95MS), optional(p.BackendRSSMiB), optional(p.BackendCPUPercent), optional(p.GoHeapMiB), optionalInt(p.Goroutines), optional(p.FileBudgetUsedMiB), optional(p.FileBudgetLimitMiB), optionalInt64(p.FileChunkSlotsUsed), optionalInt64(p.FileChunkSlotsLimit)})
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
		Summary                         Summary
		Config                          Config
		Generated                       time.Time
		ChunkSlotsUsed, ChunkSlotsLimit *int64
	}{summary, cfg, time.Now().UTC(), r.lastChunkSlotsUsed, r.lastChunkSlotsLimit})
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

func statusText(status string) string {
	switch status {
	case "success":
		return "成功"
	case "fluctuation":
		return "波动"
	case "failure":
		return "失败"
	default:
		return status
	}
}

var criterionNames = map[string]string{
	"stable_duration":        "稳定时长完整且未中断",
	"connections_99_percent": "稳定期在线连接达到目标的99%",
	"messages_99_9_percent":  "消息投递率达到99.9%",
	"files_100_percent":      "计划文件全部完成且队列排空",
	"ack_latency":            "ACK延迟达标",
	"no_health_failure":      "无后端健康检查失败",
	"no_abnormal_duplicates": "无异常重复消息",
	"backend_memory_limit":   "后端内存未超限",
	"file_budget":            "文件内存预算未超限",
	"data_integrity":         "数据完整性校验通过",
	"cleanup":                "测试数据清理成功",
}

func criterionText(name string) string {
	if text := criterionNames[name]; text != "" {
		return text
	}
	return name
}

func displayOptional(value *float64) string {
	if value == nil {
		return "未知"
	}
	return optional(value)
}

func displayOptionalInt64(value *int64) string {
	if value == nil {
		return "未知"
	}
	return optionalInt64(value)
}

//go:embed report.html.tmpl
var reportHTML string

var reportTemplate = template.Must(template.New("report").Funcs(template.FuncMap{
	"status": statusText, "criterion": criterionText,
	"optional": displayOptional, "optionalInt64": displayOptionalInt64,
}).Parse(reportHTML))
