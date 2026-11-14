package stress

import (
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
	if got := Evaluate(cfg, totals, facts); got.Status != "success" {
		t.Fatalf("success fixture: %+v", got)
	}
	totals.AckP95MS = 2001
	if got := Evaluate(cfg, totals, facts); got.Status != "fluctuation" {
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
	csvBody, _ := os.ReadFile(filepath.Join(report.Dir, "metrics.csv"))
	if !strings.Contains(string(csvBody), "unknown") {
		t.Fatal("unknown backend resources must remain unknown")
	}
	html, _ := os.ReadFile(filepath.Join(report.Dir, "report.html"))
	if !strings.Contains(string(html), "Secure IM") {
		t.Fatal("invalid HTML")
	}
}
