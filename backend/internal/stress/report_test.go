package stress

import (
	"bytes"
	"encoding/csv"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func successfulFixture() (Config, Totals, RunFacts) {
	cfg := DefaultConfig()
	cfg.Target = 10
	totals := Totals{MessagesSent: 1000, MessagesDelivered: 1000, FilesCompleted: 2, StableSamples: 5, StableOnlineAverage: 10, StableOnlineMin: 10, AckP95MS: 100, AckP99MS: 200, FileBudgetHighMiB: 10, FileBudgetKnown: true}
	return cfg, totals, RunFacts{StableFull: true, TargetReached: true, Drained: true}
}
func TestEvaluateStatuses(t *testing.T) {
	cfg, totals, facts := successfulFixture()
	if got := Evaluate(cfg, totals, facts); got.Status != "success" || !strings.Contains(got.Conclusion, "状态=成功") {
		t.Fatalf("success fixture: %+v", got)
	}
	totals.AckP95MS = 2001
	if got := Evaluate(cfg, totals, facts); got.Status != "fluctuation" || !strings.Contains(got.Conclusion, "状态=波动") || strings.Contains(got.Conclusion, "状态=fluctuation") {
		t.Fatalf("latency should fluctuate: %+v", got)
	}
	cfg, totals, facts = successfulFixture()
	rss := 4097.0
	facts.BackendRSSMaxMiB = &rss
	if got := Evaluate(cfg, totals, facts); got.Status != "fluctuation" {
		t.Fatalf("RSS should fluctuate: %+v", got)
	}
	failureTests := []func(*Totals, *RunFacts){
		func(t *Totals, f *RunFacts) { f.StableFull = false }, func(t *Totals, f *RunFacts) { t.StableOnlineAverage = 9.8 },
		func(t *Totals, f *RunFacts) { t.MessagesDelivered = 998 }, func(t *Totals, f *RunFacts) { t.FilesFailed = 1 },
		func(t *Totals, f *RunFacts) { t.MessageDuplicates = 1 }, func(t *Totals, f *RunFacts) { t.HealthFailures = 1 },
		func(t *Totals, f *RunFacts) { t.FileBudgetHighMiB = 501 }, func(t *Totals, f *RunFacts) { f.DataError = true },
	}
	for i, mutate := range failureTests {
		cfg, totals, facts = successfulFixture()
		mutate(&totals, &facts)
		if got := Evaluate(cfg, totals, facts); got.Status != "failure" {
			t.Errorf("failure case %d: %+v", i, got)
		}
	}
}
func TestReportArtifacts(t *testing.T) {
	cfg, totals, facts := successfulFixture()
	cfg.RunID = "report-test"
	report, err := NewReport(t.TempDir(), cfg.RunID, cfg)
	if err != nil {
		t.Fatal(err)
	}
	progress := Progress{Timestamp: time.Now().UTC(), Phase: "stable", Target: 10}
	if err = report.AddProgress(progress); err != nil {
		t.Fatal(err)
	}
	used, limit := int64(7), int64(96)
	progress.FileChunkSlotsUsed, progress.FileChunkSlotsLimit = &used, &limit
	if err = report.AddProgress(progress); err != nil {
		t.Fatal(err)
	}
	totals.FileQueueAvgMS, totals.FileCompletionAvgMS = 120.5, 3456.75
	result := Evaluate(cfg, totals, facts)
	summary, err := report.Finalize(cfg, Allocation{Idle: 8, Chat: 2}, totals, facts, result)
	if err != nil {
		t.Fatal(err)
	}
	if summary.Status != "success" {
		t.Fatal(summary.Status)
	}
	for _, name := range []string{"conclusion.txt", "summary.json", "report.html", "metrics.csv", "config.json"} {
		if _, err = os.Stat(filepath.Join(report.Dir, name)); err != nil {
			t.Errorf("%s: %v", name, err)
		}
	}
	body, err := os.ReadFile(filepath.Join(report.Dir, "summary.json"))
	if err != nil {
		t.Fatal(err)
	}
	var decoded Summary
	if err = json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.Status != "success" || decoded.Result.Status != "success" || !strings.Contains(decoded.Result.Conclusion, "状态=成功") {
		t.Fatalf("summary must keep machine statuses in English and localize conclusions: %s", body)
	}
	if decoded.Totals.FileQueueAvgMS != 120.5 || decoded.Totals.FileCompletionAvgMS != 3456.75 {
		t.Fatalf("file timing averages missing from summary.json: %s", body)
	}
	conclusion, err := os.ReadFile(filepath.Join(report.Dir, "conclusion.txt"))
	if err != nil || !strings.Contains(string(conclusion), "状态=成功") {
		t.Fatalf("localized conclusion missing: %q, %v", conclusion, err)
	}
	csvBody, _ := os.ReadFile(filepath.Join(report.Dir, "metrics.csv"))
	if !strings.Contains(string(csvBody), "unknown") {
		t.Fatal("unknown backend resources must remain unknown")
	}
	rows, err := csv.NewReader(bytes.NewReader(csvBody)).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 3 || len(rows[0]) < 2 || rows[0][len(rows[0])-2] != "file_chunk_slots_used" || rows[0][len(rows[0])-1] != "file_chunk_slots_limit" {
		t.Fatalf("chunk-slot CSV columns missing: %v", rows)
	}
	if got := rows[1][len(rows[1])-2:]; got[0] != "unknown" || got[1] != "unknown" {
		t.Fatalf("unknown chunk slots = %v", got)
	}
	if got := rows[2][len(rows[2])-2:]; got[0] != "7" || got[1] != "96" {
		t.Fatalf("known chunk slots = %v, want [7 96]", got)
	}
	encoded, err := json.Marshal(progress)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(encoded), `"file_chunk_slots_used":7`) || !strings.Contains(string(encoded), `"file_chunk_slots_limit":96`) {
		t.Fatalf("chunk slots missing from NDJSON progress: %s", encoded)
	}
	html, _ := os.ReadFile(filepath.Join(report.Dir, "report.html"))
	htmlText := string(html)
	for _, text := range []string{
		"IMFT", "运行编号：", "<h2 class=\"status success\">成功</h2>",
		"稳定时长完整且未中断", "<td>通过</td>", "文件工作线程/内存预算",
		"条/秒/用户", "分块通道已用/上限（最后有效采样）", "<td colspan=\"3\">7 / 96</td>",
		"平均文件等待时间（毫秒）", "平均文件完成时间（毫秒，含等待与下载校验）", "<td>120.500</td>", "<td>3456.750</td>",
	} {
		if !strings.Contains(htmlText, text) {
			t.Errorf("localized HTML report missing %q: %s", text, html)
		}
	}
	for _, text := range []string{
		"@page{size:A4;margin:12mm 11mm}", "font-size:14px", "table{width:100%",
		"tbody tr:nth-child(even)", "tr{break-inside:avoid;page-break-inside:avoid}",
		"break-after:avoid;page-break-after:avoid", "overflow-wrap:anywhere", "thead{display:table-header-group}",
	} {
		if !strings.Contains(htmlText, text) {
			t.Errorf("print CSS missing %q", text)
		}
	}
	if strings.Contains(htmlText, "Run ID:") || strings.Contains(htmlText, ">PASS<") || strings.Contains(htmlText, ">FAIL<") {
		t.Fatal("HTML report contains untranslated status labels")
	}
}

func TestEvaluateDistinguishesDownloadTimeoutAndMessageRejected(t *testing.T) {
	cfg, totals, facts := successfulFixture()
	totals.FilesFailed = 1
	facts.FailureReason = "file transfer failed: file file-1: download read: read tcp 127.0.0.1: i/o timeout"
	result := Evaluate(cfg, totals, facts)
	advice := strings.Join(result.Bottlenecks, "；")
	if !strings.Contains(advice, "文件下载发生i/o timeout") || strings.Contains(advice, "检查文件active限制") {
		t.Fatalf("download timeout advice = %q", advice)
	}

	cfg, totals, facts = successfulFixture()
	totals.MessageLost = 1
	facts.DataError = true
	facts.FailureReason = "data integrity: server ERROR for message-123: message_rejected: message rejected"
	result = Evaluate(cfg, totals, facts)
	advice = strings.Join(result.Bottlenecks, "；")
	if !strings.Contains(advice, "message_rejected") || !strings.Contains(advice, "message_id") || !strings.Contains(advice, "会话查询/创建") {
		t.Fatalf("message rejection advice = %q", advice)
	}
}

func TestEvaluateRequiresEveryPlannedFile(t *testing.T) {
	tests := []struct {
		name      string
		completed int64
		failed    int64
		drained   bool
		want      bool
	}{
		{name: "all planned files", completed: 2, drained: true, want: true},
		{name: "partial completion", completed: 1, drained: true, want: false},
		{name: "no completion", completed: 0, drained: true, want: false},
		{name: "unexpected extra completion", completed: 3, drained: true, want: false},
		{name: "failed file", completed: 2, failed: 1, drained: true, want: false},
		{name: "not drained", completed: 2, drained: false, want: false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			cfg, totals, facts := successfulFixture()
			cfg.Target = 100
			totals.StableOnlineAverage = 100
			totals.FilesCompleted = test.completed
			totals.FilesFailed = test.failed
			facts.Drained = test.drained
			if got := Evaluate(cfg, totals, facts).Criteria["files_100_percent"]; got != test.want {
				t.Fatalf("files_100_percent=%v, want %v", got, test.want)
			}
		})
	}

	cfg, totals, facts := successfulFixture()
	totals.FilesCompleted = 0
	if got := Evaluate(cfg, totals, facts).Criteria["files_100_percent"]; !got {
		t.Fatal("zero planned files should satisfy the file criterion")
	}
}
