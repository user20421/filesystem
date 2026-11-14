package stress

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"sync"
	"time"

	"github.com/minio/minio-go/v7"
	"secure-im/backend/internal/auth"
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
	var env Environment
	var db *sql.DB
	var objects *minio.Client
	defer func() {
		if db != nil {
			db.Close()
		}
	}()
	if err == nil {
		env, err = LoadEnvironment(r.cfg.EnvFile)
		if err != nil {
			r.fail("preflight: %v", err)
		}
	}
	api := NewAPIClient(r.cfg.Server)
	if err == nil {
		preflightCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		err = api.Health(preflightCtx, "/health")
		if err == nil {
			err = api.Health(preflightCtx, "/ready")
		}
		cancel()
		if err != nil {
			r.fail("preflight HTTP: %v", err)
		}
	}
	if err == nil {
		resourceCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
		db, objects, err = OpenResources(resourceCtx, env)
		cancel()
		if err != nil {
			r.fail("preflight DB/Bucket: %v", err)
		}
	}
	var users []User
	resourcesReady := db != nil && objects != nil
	if err == nil {
		tokens := auth.NewTokenManager(env.JWTSecret, 24*time.Hour, 24*time.Hour)
		prepareCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
		users, err = CreateUsers(prepareCtx, db, tokens, r.cfg, r.allocation)
		cancel()
		if err != nil {
			r.fail("prepare users: %v", err)
		} else if err = preflightWebSocket(ctx, api, users[0]); err != nil {
			r.fail("preflight ws-ticket/WebSocket: %v", err)
		}
	}
	if err == nil && ctx.Err() == nil {
		r.executeLoad(runCtx, cancelRun, api, users)
	} else if ctx.Err() != nil {
		r.facts.Interrupted = true
		r.fail("run interrupted: %v", ctx.Err())
	}
	r.setPhase("cleanup")
	if resourcesReady {
		cleanupCtx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
		cleanupErr := Cleanup(cleanupCtx, db, objects, env.MinIOBucket, r.cfg.RunID)
		cancel()
		if cleanupErr != nil {
			r.facts.CleanupError = cleanupErr.Error()
		}
	}
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

func preflightWebSocket(ctx context.Context, api *APIClient, user User) error {
	preflightCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	ticket, err := api.Ticket(preflightCtx, user.Token)
	if err != nil {
		return err
	}
	conn, err := api.WebSocket(preflightCtx, ticket)
	if err != nil {
		return err
	}
	return conn.Close()
}

func (r *Runner) executeLoad(runCtx context.Context, cancelRun context.CancelFunc, api *APIClient, users []User) {
	clients := make([]*WSClient, len(users))
	var clientWG sync.WaitGroup
	for i := range users {
		clients[i] = NewWSClient(users[i], api, r.metrics, r.badData)
	}
	telemetry := NewTelemetry(api, r.metrics, r.cfg.DockerContainer)
	var telemetryWG sync.WaitGroup
	telemetryWG.Add(1)
	go func() { defer telemetryWG.Done(); telemetry.Run(runCtx) }()
	r.setPhase("rampup")
	launched := 0
	interval := time.Second / time.Duration(r.cfg.ConnectionsPerS)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
launchLoop:
	for i := range clients {
		if i > 0 {
			select {
			case <-runCtx.Done():
				break launchLoop
			case <-ticker.C:
			}
		}
		client := clients[i]
		clientWG.Add(2)
		go func() { defer clientWG.Done(); client.Run(runCtx) }()
		go func() { defer clientWG.Done(); client.Heartbeat(runCtx) }()
		launched++
	}
	required := (r.cfg.Target*99 + 99) / 100
	deadline := time.NewTimer(r.cfg.RampTimeout)
	ready := time.NewTicker(50 * time.Millisecond)
	for r.metrics.Online() < int64(required) && runCtx.Err() == nil {
		select {
		case <-runCtx.Done():
		case <-deadline.C:
		case <-ready.C:
		}
	}
	stopTimer(deadline)
	ready.Stop()
	r.factMu.Lock()
	r.facts.TargetReached = r.metrics.Online() >= int64(required)
	r.factMu.Unlock()
	if !r.facts.TargetReached {
		r.fail("only %d/%d connections online after ramp (required %d; launched %d)", r.metrics.Online(), r.cfg.Target, required, launched)
	}
	loadCtx, cancelLoad := context.WithCancel(runCtx)
	var pool *FilePool
	if r.facts.TargetReached && runCtx.Err() == nil {
		r.setPhase("stable")
		r.metrics.StableSample()
		for i, client := range clients {
			if users[i].Role == "chat" && users[i].Partner >= 0 {
				partner := users[users[i].Partner]
				clientWG.Add(1)
				go func() { defer clientWG.Done(); client.Chat(loadCtx, partner, r.cfg.ChatRate) }()
			}
		}
		if r.allocation.File > 0 {
			pool = NewFilePool(r.cfg, api, r.metrics, len(users), r.badData, r.fileFailure)
			pool.Start(runCtx, loadCtx, users)
		}
		stableTimer := time.NewTimer(r.cfg.StableDuration)
		select {
		case <-stableTimer.C:
			r.factMu.Lock()
			r.facts.StableFull = true
			r.factMu.Unlock()
		case <-runCtx.Done():
			stopTimer(stableTimer)
			r.facts.Interrupted = true
			r.fail("stable phase interrupted: %v", runCtx.Err())
		}
	}
	cancelLoad()
	r.setPhase("drain")
	if pool != nil {
		done := make(chan struct{})
		go func() { pool.Wait(); close(done) }()
		timer := time.NewTimer(r.cfg.DrainTimeout)
		select {
		case <-done:
			r.facts.Drained = true
			stopTimer(timer)
		case <-timer.C:
			r.fail("file queue did not drain within %s", r.cfg.DrainTimeout)
		case <-runCtx.Done():
			r.fail("file drain interrupted")
			stopTimer(timer)
		}
		if !r.facts.Drained {
			cancelRun()
			<-done
		}
	} else {
		r.facts.Drained = true
		select {
		case <-time.After(3 * time.Second):
		case <-runCtx.Done():
		}
	}
	// Stop producing first, then allow the final ACK/delivery frames to arrive
	// before closing sockets. Otherwise short runs report false message loss.
	drainDeadline := time.Now().Add(3 * time.Second)
	for r.metrics.PendingMessages() > 0 && time.Now().Before(drainDeadline) && runCtx.Err() == nil {
		time.Sleep(10 * time.Millisecond)
	}
	cancelRun()
	for _, client := range clients {
		client.Close()
	}
	clientWG.Wait()
	telemetryWG.Wait()
	rssMax, cpuMax := telemetry.Max()
	r.factMu.Lock()
	r.facts.BackendRSSMaxMiB, r.facts.BackendCPUMax = rssMax, cpuMax
	r.factMu.Unlock()
}

func stopTimer(timer *time.Timer) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
}

func (r *Runner) progressLoop(stop <-chan struct{}) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	var previousBytes int64
	previousTime := time.Now()
	emit := func() {
		now := time.Now()
		totals := r.metrics.Totals()
		elapsed := now.Sub(previousTime).Seconds()
		throughput := float64(0)
		if elapsed > 0 {
			throughput = float64(totals.BytesCompleted-previousBytes) / (1 << 20) / elapsed
		}
		previousBytes, previousTime = totals.BytesCompleted, now
		if r.getPhase() == "stable" {
			r.metrics.StableSample()
		}
		progress := r.metrics.Snapshot(r.cfg.Target, r.getPhase(), throughput)
		_ = json.NewEncoder(r.out).Encode(progress)
		if r.report != nil {
			_ = r.report.AddProgress(progress)
		}
	}
	emit()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			emit()
		}
	}
}
