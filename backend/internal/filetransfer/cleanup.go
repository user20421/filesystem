package filetransfer

import (
	"context"
	"errors"
	"log/slog"
	"time"

	"secure-im/backend/internal/apperror"
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

// cleanupService 明确 Sweeper 所需的候选查询与删除能力，禁止直接访问 Service 数据库。
type cleanupService interface {
	Delete(context.Context, string, string) error
	CleanupCandidates(context.Context, time.Time, time.Time, int) ([]cleanupCandidate, error)
	DeleteCleanupCandidate(context.Context, string, string, string, time.Time) error
}

type Sweeper struct {
	files  cleanupService
	log    *slog.Logger
	config CleanupConfig
	now    func() time.Time
}

func NewSweeper(files cleanupService, log *slog.Logger, config CleanupConfig) *Sweeper {
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
		err := s.files.DeleteCleanupCandidate(roundCtx, candidate.id, candidate.ownerID, candidate.status, cutoff)
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
	return s.files.CleanupCandidates(ctx, uploadCutoff, deletingCutoff, cleanupBatchSize)
}

func isNotFound(err error) bool {
	var business *apperror.Error
	return errors.As(err, &business) && business.Kind == apperror.KindNotFound
}
