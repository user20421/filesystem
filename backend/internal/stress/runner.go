package stress

import (
	"context"
	"fmt"
	"io"
	"path/filepath"
	"sync"
)

type Final struct {
	Type       string `json:"type"`
	RunID      string `json:"run_id"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	ReportDir  string `json:"report_dir"`
	HTML       string `json:"html"`
	Target     int    `json:"target"`
	Reason     string `json:"reason"`
}

type Runner struct {
	cfg        Config
	out        io.Writer
	report     *Report
	metrics    *Metrics
	allocation Allocation
	facts      RunFacts
	factMu     sync.Mutex
	phaseMu    sync.RWMutex
	phase      string
}

func NewRunner(cfg Config, out io.Writer) *Runner {
	return &Runner{cfg: cfg, out: out, phase: "preflight"}
}
func (r *Runner) setPhase(phase string) { r.phaseMu.Lock(); r.phase = phase; r.phaseMu.Unlock() }
func (r *Runner) getPhase() string      { r.phaseMu.RLock(); defer r.phaseMu.RUnlock(); return r.phase }
func (r *Runner) fail(format string, args ...any) {
	r.factMu.Lock()
	if r.facts.FailureReason == "" {
		r.facts.FailureReason = fmt.Sprintf(format, args...)
	}
	r.factMu.Unlock()
}
func (r *Runner) fileFailure(err error) { r.fail("file transfer failed: %v", err) }
func (r *Runner) badData(err error) {
	r.factMu.Lock()
	r.facts.DataError = true
	if r.facts.FailureReason == "" {
		r.facts.FailureReason = "data integrity: " + err.Error()
	}
	r.factMu.Unlock()
}

// Run 执行完整压测生命周期，任何失败仍尽量生成报告并清理本 run 数据。
func (r *Runner) Run(ctx context.Context) Final {
	report, err := NewReport(r.cfg.ReportRoot, r.cfg.RunID, r.cfg)
	if err != nil {
		return Final{Type: "final", RunID: r.cfg.RunID, Status: "failure", Conclusion: err.Error(), Reason: err.Error(), Target: r.cfg.Target}
	}
	r.report = report
	defer report.Close()
	r.metrics = NewMetrics(r.cfg.RunID, r.cfg.FileBudgetMiB)
	r.allocation, err = Allocate(r.cfg.Target, r.cfg.IdlePercent, r.cfg.ChatPercent, r.cfg.FilePercent)
	if err != nil {
		r.fail("%v", err)
	}
	progressStop := make(chan struct{})
	progressDone := make(chan struct{})
	go func() { defer close(progressDone); r.progressLoop(progressStop) }()
	runCtx, cancelRun := context.WithCancel(ctx)
	defer cancelRun()
	resources, err := r.preflight(ctx, err)
	defer func() {
		if resources.db != nil {
			resources.db.Close()
		}
	}()
	if err == nil && ctx.Err() == nil {
		r.executeLoad(runCtx, cancelRun, resources.api, resources.users)
	} else if ctx.Err() != nil {
		r.facts.Interrupted = true
		r.fail("run interrupted: %v", ctx.Err())
	}
	r.cleanup(resources)
	close(progressStop)
	<-progressDone
	r.factMu.Lock()
	facts := r.facts
	r.factMu.Unlock()
	totals := r.metrics.Totals()
	result := Evaluate(r.cfg, totals, facts)
	if _, err = r.report.Finalize(r.cfg, r.allocation, totals, facts, result); err != nil {
		result.Status = "failure"
		result.Reason = "report: " + err.Error()
		result.Conclusion = result.Reason
	}
	absoluteDir, _ := filepath.Abs(r.report.Dir)
	return Final{Type: "final", RunID: r.cfg.RunID, Status: result.Status, Conclusion: result.Conclusion, ReportDir: absoluteDir, HTML: filepath.Join(absoluteDir, "report.html"), Target: r.cfg.Target, Reason: result.Reason}
}
