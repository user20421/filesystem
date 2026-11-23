package filetransfer

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"time"

	"secure-im/backend/internal/httpx"
)

const (
	cleanupBatchSize         = 100
	defaultCleanupRoundLimit = 30 * time.Second
)

var errCleanupSkipped = errors.New("file cleanup candidate is no longer eligible")

type CleanupConfig struct {
	UploadIdleTTL      time.Duration
	DeletingRetryAfter time.Duration
	Interval           time.Duration
	RoundTimeout       time.Duration
}

type CleanupStats struct {
	Selected  int
	Succeeded int
	Skipped   int
	Failed    int
}

type cleanupCandidate struct {
	id      string
	ownerID string
	status  string
}

type Sweeper struct {
	files  *Service
	log    *slog.Logger
	config CleanupConfig
	now    func() time.Time
}

func NewSweeper(files *Service, log *slog.Logger, config CleanupConfig) *Sweeper {
	if log == nil {
		log = slog.Default()
	}
	if config.RoundTimeout <= 0 {
		config.RoundTimeout = defaultCleanupRoundLimit
	}
	return &Sweeper{files: files, log: log, config: config, now: time.Now}
}

// Run executes rounds synchronously, so a slow round can never overlap the
// next one. Cancellation is observed by the round context and by the timer.
func (s *Sweeper) Run(ctx context.Context) {
	if ctx.Err() != nil {
		return
	}
	s.RunOnce(ctx)
	timer := time.NewTimer(s.config.Interval)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-timer.C:
			s.RunOnce(ctx)
			timer.Reset(s.config.Interval)
		}
	}
}

// RunOnce 串行执行一轮清理，候选必须在删除时重新校验状态和时间，单个失败不阻断其他候选并保留下轮重试。
func (s *Sweeper) RunOnce(ctx context.Context) CleanupStats {
	var stats CleanupStats
	if ctx.Err() != nil {
		return stats
	}
	roundCtx, cancel := context.WithTimeout(ctx, s.config.RoundTimeout)
	defer cancel()
	now := s.now()
	candidates, err := s.candidates(roundCtx, now)
	if err != nil {
		stats.Failed = 1
		s.log.Error("file cleanup candidate selection failed", "error", err, "selected", stats.Selected, "succeeded", stats.Succeeded, "skipped", stats.Skipped, "failed", stats.Failed)
		return stats
	}
	stats.Selected = len(candidates)
	for _, candidate := range candidates {
		cutoff := now.Add(-s.config.UploadIdleTTL)
		if candidate.status == "deleting" {
			cutoff = now.Add(-s.config.DeletingRetryAfter)
		}
		err := s.files.delete(roundCtx, candidate.id, candidate.ownerID, true, candidate.status, cutoff)
		switch {
		case err == nil:
			stats.Succeeded++
		case errors.Is(err, errCleanupSkipped), isNotFound(err):
			stats.Skipped++
		default:
			stats.Failed++
			s.log.Warn("file cleanup candidate failed", "file_id", candidate.id, "status", candidate.status, "error", err)
		}
		if roundCtx.Err() != nil {
			break
		}
	}
	s.log.Info("file cleanup round", "selected", stats.Selected, "succeeded", stats.Succeeded, "skipped", stats.Skipped, "failed", stats.Failed)
	return stats
}

func (s *Sweeper) candidates(ctx context.Context, now time.Time) ([]cleanupCandidate, error) {
	uploadCutoff := now.Add(-s.config.UploadIdleTTL)
	deletingCutoff := now.Add(-s.config.DeletingRetryAfter)
	rows, err := s.files.db.QueryContext(ctx, `SELECT f.id,f.owner_id,c.status FROM files f JOIN file_contents c ON c.id=f.content_id WHERE (c.status='uploading' AND f.status='active' AND c.updated_at<=?) OR (c.status='deleting' AND f.status='deleting' AND c.ref_count=0 AND c.updated_at<=?) ORDER BY c.updated_at,f.id LIMIT ?`, uploadCutoff, deletingCutoff, cleanupBatchSize)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	candidates := make([]cleanupCandidate, 0)
	for rows.Next() {
		var candidate cleanupCandidate
		if err := rows.Scan(&candidate.id, &candidate.ownerID, &candidate.status); err != nil {
			return nil, err
		}
		candidates = append(candidates, candidate)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return candidates, nil
}

func isNotFound(err error) bool {
	var apiErr *httpx.APIError
	return errors.As(err, &apiErr) && apiErr.Status == http.StatusNotFound
}
