package filetransfer

import (
	"context"
	"database/sql"
	"errors"
	"io"
	"log/slog"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
)

const cleanupCandidatesQuery = `SELECT f.id,f.owner_id,c.status FROM files f JOIN file_contents c ON c.id=f.content_id WHERE (c.status='uploading' AND f.status='active' AND c.updated_at<=?) OR (c.status='deleting' AND f.status='deleting' AND c.ref_count=0 AND c.updated_at<=?) ORDER BY c.updated_at,f.id LIMIT ?`

func newCleanupTestSweeper(db *sql.DB, objects ObjectStore) *Sweeper {
	return NewSweeper(NewService(db, objects, 3, 1<<20, 1<<20, 1), slog.New(slog.NewTextHandler(io.Discard, nil)), CleanupConfig{
		UploadIdleTTL:      time.Hour,
		DeletingRetryAfter: 5 * time.Minute,
		Interval:           time.Minute,
		RoundTimeout:       time.Second,
	})
}

func expectCleanupCandidates(mock sqlmock.Sqlmock, sweeper *Sweeper, now time.Time, candidates ...cleanupCandidate) *sqlmock.ExpectedQuery {
	rows := sqlmock.NewRows(strings.Fields("id owner_id status"))
	for _, candidate := range candidates {
		rows.AddRow(candidate.id, candidate.ownerID, candidate.status)
	}
	return mock.ExpectQuery(regexp.QuoteMeta(cleanupCandidatesQuery)).
		WithArgs(now.Add(-sweeper.config.UploadIdleTTL), now.Add(-sweeper.config.DeletingRetryAfter), cleanupBatchSize).
		WillReturnRows(rows)
}

func expectCleanupFinalDeleting(mock sqlmock.Sqlmock, candidate cleanupCandidate, contentID string, updatedAt time.Time) {
	expectGetFile(mock, candidate.id, contentID, candidate.ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	expectDeleteState(mock, candidate.id, contentID, "deleting", "deleting", 0, updatedAt)
	mock.ExpectCommit()
	expectFinalMetadataDelete(mock, candidate.id, contentID)
}

func TestSweeperLeavesFreshAndSharedContent(t *testing.T) {
	for _, name := range []string{"fresh uploading", "shared ready"} {
		t.Run(name, func(t *testing.T) {
			db, mock, err := sqlmock.New()
			if err != nil {
				t.Fatal(err)
			}
			defer db.Close()
			objects := &testObjectStore{}
			sweeper := newCleanupTestSweeper(db, objects)
			now := time.Now()
			sweeper.now = func() time.Time { return now }
			expectCleanupCandidates(mock, sweeper, now)
			stats := sweeper.RunOnce(context.Background())
			if stats.Selected != 0 || stats.Succeeded != 0 || stats.Skipped != 0 || stats.Failed != 0 {
				t.Fatalf("unexpected stats: %+v", stats)
			}
			if len(objects.removed) != 0 || len(objects.listPrefixes) != 0 {
				t.Fatalf("fresh/shared content touched object storage: lists=%v removes=%v", objects.listPrefixes, objects.removed)
			}
			if err := mock.ExpectationsWereMet(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestSweeperDeletesExpiredUploadAndReleasesActiveReference(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	candidate := cleanupCandidate{id: id, ownerID: ownerID, status: "uploading"}
	objects := &testObjectStore{listed: []string{"contents/" + contentID + "/orphan"}}
	sweeper := newCleanupTestSweeper(db, objects)
	now := time.Now()
	stale := now.Add(-2 * time.Hour)
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, candidate)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "uploading")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "active", "uploading", 1, stale)
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	expectFinalMetadataDelete(mock, id, contentID)
	stats := sweeper.RunOnce(context.Background())
	if stats.Selected != 1 || stats.Succeeded != 1 || stats.Failed != 0 {
		t.Fatalf("unexpected stats: %+v", stats)
	}
	if len(objects.removed) != 1 {
		t.Fatalf("removed=%v, want expired content object", objects.removed)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSweeperSkipsContentActiveAfterScan(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	candidate := cleanupCandidate{id: id, ownerID: ownerID, status: "uploading"}
	objects := &testObjectStore{}
	sweeper := newCleanupTestSweeper(db, objects)
	now := time.Now()
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, candidate)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "uploading")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "active", "uploading", 1, now)
	mock.ExpectRollback()
	stats := sweeper.RunOnce(context.Background())
	if stats.Selected != 1 || stats.Succeeded != 0 || stats.Skipped != 1 || stats.Failed != 0 {
		t.Fatalf("unexpected stats: %+v", stats)
	}
	if len(objects.listPrefixes) != 0 {
		t.Fatalf("active content was listed for deletion: %v", objects.listPrefixes)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSweeperSkipsMissingFile(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	candidate := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "deleting"}
	sweeper := newCleanupTestSweeper(db, &testObjectStore{})
	now := time.Now()
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, candidate)
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).WithArgs(candidate.id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id content_id owner_id recipient_id original_name size sha256 chunk_size chunk_count status revision created_at completed_at")))
	stats := sweeper.RunOnce(context.Background())
	if stats.Selected != 1 || stats.Skipped != 1 || stats.Failed != 0 {
		t.Fatalf("unexpected stats: %+v", stats)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSweeperContinuesAfterFailureAndRetriesNextRound(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	first := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "deleting"}
	second := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "deleting"}
	firstContent, secondContent := uuid.NewString(), uuid.NewString()
	objects := &testObjectStore{listErrors: []error{errors.New("temporary List failure"), nil}}
	sweeper := newCleanupTestSweeper(db, objects)
	now := time.Now()
	stale := now.Add(-time.Hour)
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, first, second)
	expectGetFile(mock, first.id, firstContent, first.ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	expectDeleteState(mock, first.id, firstContent, "deleting", "deleting", 0, stale)
	mock.ExpectCommit()
	expectCleanupFinalDeleting(mock, second, secondContent, stale)
	stats := sweeper.RunOnce(context.Background())
	if stats.Selected != 2 || stats.Succeeded != 1 || stats.Failed != 1 {
		t.Fatalf("first round stats: %+v", stats)
	}

	// A failed candidate remains deleting and is selected again on a later round.
	expectCleanupCandidates(mock, sweeper, now, first)
	expectCleanupFinalDeleting(mock, first, firstContent, stale)
	stats = sweeper.RunOnce(context.Background())
	if stats.Selected != 1 || stats.Succeeded != 1 || stats.Failed != 0 {
		t.Fatalf("retry round stats: %+v", stats)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSweeperNonFinalReferencePreservesContentAge(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	candidate := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "uploading"}
	contentID := uuid.NewString()
	objects := &testObjectStore{}
	sweeper := newCleanupTestSweeper(db, objects)
	now := time.Now()
	stale := now.Add(-2 * time.Hour)
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, candidate)
	expectGetFile(mock, candidate.id, contentID, candidate.ownerID, uuid.NewString(), "uploading")
	mock.ExpectBegin()
	expectDeleteState(mock, candidate.id, contentID, "active", "uploading", 2, stale)
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=ref_count-1,updated_at=updated_at WHERE id=? AND ref_count>1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM files WHERE id=? AND status='active'`)).
		WithArgs(candidate.id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	stats := sweeper.RunOnce(context.Background())
	if stats.Succeeded != 1 || stats.Failed != 0 {
		t.Fatalf("unexpected stats: %+v", stats)
	}
	if len(objects.listPrefixes) != 0 || len(objects.removed) != 0 {
		t.Fatalf("shared uploading content objects were touched: lists=%v removes=%v", objects.listPrefixes, objects.removed)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSweeperCancellationAndPreCanceledRun(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	sweeper := newCleanupTestSweeper(db, &testObjectStore{})
	now := time.Now()
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now).WillDelayFor(time.Second)
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(20 * time.Millisecond)
		cancel()
	}()
	started := time.Now()
	sweeper.RunOnce(ctx)
	if elapsed := time.Since(started); elapsed > 500*time.Millisecond {
		t.Fatalf("round did not observe cancellation promptly: %v", elapsed)
	}

	canceled, cancelNow := context.WithCancel(context.Background())
	cancelNow()
	done := make(chan struct{})
	go func() {
		sweeper.Run(canceled)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(100 * time.Millisecond):
		t.Fatal("pre-canceled Run did not return")
	}
}

type overlapObjectStore struct {
	testObjectStore
	active atomic.Int64
	max    atomic.Int64
	calls  atomic.Int64
}

func (s *overlapObjectStore) List(ctx context.Context, prefix string) ([]string, error) {
	active := s.active.Add(1)
	for {
		max := s.max.Load()
		if active <= max || s.max.CompareAndSwap(max, active) {
			break
		}
	}
	s.calls.Add(1)
	time.Sleep(15 * time.Millisecond)
	s.active.Add(-1)
	return s.testObjectStore.List(ctx, prefix)
}

func TestSweeperRoundsDoNotOverlap(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	objects := &overlapObjectStore{}
	sweeper := newCleanupTestSweeper(db, objects)
	sweeper.config.Interval = time.Millisecond
	now := time.Now()
	stale := now.Add(-time.Hour)
	sweeper.now = func() time.Time { return now }
	candidate := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "deleting"}
	contentID := uuid.NewString()
	for range 8 {
		expectCleanupCandidates(mock, sweeper, now, candidate)
		expectCleanupFinalDeleting(mock, candidate, contentID, stale)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 55*time.Millisecond)
	defer cancel()
	sweeper.Run(ctx)
	if calls := objects.calls.Load(); calls < 2 {
		t.Fatalf("only %d rounds reached List; test did not exercise adjacent rounds", calls)
	}
	if max := objects.max.Load(); max != 1 {
		t.Fatalf("concurrent List calls=%d, rounds overlapped", max)
	}
}

type blockingListObjectStore struct {
	testObjectStore
	started chan struct{}
	release chan struct{}
	once    sync.Once
}

func (s *blockingListObjectStore) List(ctx context.Context, prefix string) ([]string, error) {
	s.once.Do(func() { close(s.started) })
	select {
	case <-s.release:
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	return s.testObjectStore.List(ctx, prefix)
}

func TestManualDeleteAndSweeperRaceIsIdempotent(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	candidate := cleanupCandidate{id: uuid.NewString(), ownerID: uuid.NewString(), status: "deleting"}
	contentID := uuid.NewString()
	objects := &blockingListObjectStore{started: make(chan struct{}), release: make(chan struct{})}
	sweeper := newCleanupTestSweeper(db, objects)
	now := time.Now()
	stale := now.Add(-time.Hour)
	sweeper.now = func() time.Time { return now }
	expectCleanupCandidates(mock, sweeper, now, candidate)
	expectCleanupFinalDeleting(mock, candidate, contentID, stale)
	// The manual retry waits on the same file lock and observes the finalized
	// reference as missing instead of listing/removing/finalizing it twice.
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).WithArgs(candidate.id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id content_id owner_id recipient_id original_name size sha256 chunk_size chunk_count status revision created_at completed_at")))

	sweepDone := make(chan CleanupStats, 1)
	go func() { sweepDone <- sweeper.RunOnce(context.Background()) }()
	select {
	case <-objects.started:
	case <-time.After(time.Second):
		t.Fatal("sweeper did not reach List")
	}
	manualDone := make(chan error, 1)
	go func() { manualDone <- sweeper.files.Delete(context.Background(), candidate.id, candidate.ownerID) }()
	close(objects.release)
	if stats := <-sweepDone; stats.Succeeded != 1 || stats.Failed != 0 {
		t.Fatalf("sweep stats: %+v", stats)
	}
	if err := <-manualDone; !isNotFound(err) {
		t.Fatalf("manual retry error=%v, want idempotent 404", err)
	}
	if got := len(objects.listPrefixes); got != 1 {
		t.Fatalf("List calls=%d, want 1", got)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
