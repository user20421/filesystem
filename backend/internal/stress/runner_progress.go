package stress

import (
	"encoding/json"
	"time"
)

// progressLoop 周期性输出同一 NDJSON Progress schema，并写入 CSV artifact。
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
