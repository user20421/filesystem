package stress

import (
	"context"
	"time"
)

// cleanup 仅清理当前 run，并保留清理错误供报告评估使用。
func (r *Runner) cleanup(resources preflightResult) {
	r.setPhase("cleanup")
	if !resources.resourcesReady {
		return
	}
	cleanupCtx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	cleanupErr := Cleanup(cleanupCtx, resources.db, resources.objects, resources.env.MinIOBucket, r.cfg.RunID)
	cancel()
	if cleanupErr != nil {
		r.facts.CleanupError = cleanupErr.Error()
	}
}
