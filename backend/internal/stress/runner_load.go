package stress

import (
	"context"
	"sync"
	"time"
)

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
			// File production continues through drain; only run cancellation (drain timeout or active stop) aborts it.
			pool.Start(runCtx, users)
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
