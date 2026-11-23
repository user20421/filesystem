package stress

import (
	"context"
	"sync"
	"testing"
	"time"
)

func TestByteBudgetBackpressure(t *testing.T) {
	budget := NewByteBudget(100)
	ctx := context.Background()
	if err := budget.Acquire(ctx, 80); err != nil {
		t.Fatal(err)
	}
	acquired := make(chan error, 1)
	go func() { acquired <- budget.Acquire(ctx, 30) }()
	select {
	case err := <-acquired:
		t.Fatalf("second acquire did not block: %v", err)
	case <-time.After(30 * time.Millisecond):
	}
	budget.Release(80)
	select {
	case err := <-acquired:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("acquire did not wake")
	}
	used, high, limit := budget.Snapshot()
	if used != 30 || high != 80 || limit != 100 {
		t.Fatalf("used/high/limit=%d/%d/%d", used, high, limit)
	}
	budget.Release(30)
}
func TestByteBudgetCancellationAndLimit(t *testing.T) {
	budget := NewByteBudget(100)
	if err := budget.Acquire(context.Background(), 101); err == nil {
		t.Fatal("expected oversized request error")
	}
	if err := budget.Acquire(context.Background(), 100); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	if err := budget.Acquire(ctx, 1); err != context.DeadlineExceeded {
		t.Fatalf("expected deadline, got %v", err)
	}
	budget.Release(100)
}
func TestSyntheticContentMatchesAcrossChunks(t *testing.T) {
	const size = int64(200000)
	seed := "file-test"
	expected, err := syntheticHash(seed, size)
	if err != nil {
		t.Fatal(err)
	}
	reader := &patternReader{remaining: size, seed: seedByte(seed)}
	buffer := make([]byte, 32768)
	offset := int64(0)
	for offset < size {
		n := 32768
		if size-offset < int64(n) {
			n = int(size - offset)
		}
		chunk := make([]byte, n)
		fillSynthetic(chunk, offset, seed)
		if _, err = reader.Read(buffer[:n]); err != nil {
			t.Fatal(err)
		}
		for i := 0; i < n; i++ {
			if buffer[i] != chunk[i] {
				t.Fatal("chunk and whole-file generators differ")
			}
		}
		offset += int64(n)
	}
	actual, err := syntheticHash(seed, size)
	if err != nil || actual != expected {
		t.Fatalf("hash mismatch %s %s %v", actual, expected, err)
	}
}

func TestFileQueueBackpressureAndCancellation(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileQueueSize = 1
	failures := make(chan error, 1)
	pool := NewFilePool(cfg, nil, NewMetrics("queue", 500), 2, func(error) {}, func(err error) { failures <- err })
	ctx, cancel := context.WithCancel(context.Background())
	users := []User{{ID: "a", Role: "file"}, {ID: "b", Role: "file"}}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); pool.produce(ctx, users[0], users[1]) }()
	go func() { defer wg.Done(); pool.produce(ctx, users[1], users[0]) }()
	time.Sleep(30 * time.Millisecond)
	if len(pool.queue) != 1 {
		t.Fatalf("bounded queue length=%d, want 1", len(pool.queue))
	}
	progress := pool.metrics.Snapshot(2, "stable", 0)
	if progress.FilesQueued != 2 || progress.FilesActive != 0 {
		t.Fatalf("unexpected queue progress %+v", progress)
	}
	cancel()
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("blocked producers did not honor cancellation")
	}
	progress = pool.metrics.Snapshot(2, "drain", 0)
	if progress.FilesQueued != 1 || progress.FilesFailed != 1 || len(failures) != 1 {
		t.Fatalf("canceled producer was not accounted as failed: %+v, callbacks=%d", progress, len(failures))
	}
}

func TestFilePoolFiniteProductionAndDrain(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileWorkers = 2
	cfg.FileQueueSize = 1
	cfg.FileSize = 64 << 10
	metrics := NewMetrics("finite", 500)
	pool := NewFilePool(cfg, nil, metrics, 2, func(error) {}, nil)
	pool.transferFunc = func(context.Context, fileJob) error { return nil }
	users := []User{{ID: "a", Username: "A", Role: "file", Partner: 1}, {ID: "b", Username: "B", Role: "file", Partner: 0}}
	pool.Start(context.Background(), users)
	done := make(chan struct{})
	go func() { pool.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("finite file queue did not drain")
	}
	progress := metrics.Snapshot(2, "drain", 0)
	if progress.FilesCompleted != 2 || progress.FilesFailed != 0 || progress.FilesQueued != 0 || progress.FilesActive != 0 {
		t.Fatalf("unexpected finite drain metrics %+v", progress)
	}
	select {
	case _, ok := <-pool.queue:
		if ok {
			t.Fatal("more than one task per file user was produced")
		}
	default:
	}
}

func TestFilePoolProductionSurvivesStableCancellation(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileWorkers = 1
	cfg.FileQueueSize = 1
	cfg.FileSize = 64 << 10
	metrics := NewMetrics("stable-cancel", 500)
	pool := NewFilePool(cfg, nil, metrics, 4, func(error) {}, nil)
	runCtx, cancelRun := context.WithCancel(context.Background())
	defer cancelRun()
	_, cancelStable := context.WithCancel(runCtx)
	defer cancelStable()
	release := make(chan struct{})
	var releaseOnce sync.Once
	unblock := func() { releaseOnce.Do(func() { close(release) }) }
	defer unblock()
	transferStarted := make(chan struct{})
	pool.transferFunc = func(ctx context.Context, _ fileJob) error {
		select {
		case <-transferStarted:
		default:
			close(transferStarted)
		}
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	users := []User{
		{ID: "a", Username: "A", Role: "file", Partner: 1},
		{ID: "b", Username: "B", Role: "file", Partner: 0},
		{ID: "c", Username: "C", Role: "file", Partner: 3},
		{ID: "d", Username: "D", Role: "file", Partner: 2},
	}
	pool.Start(runCtx, users)
	select {
	case <-transferStarted:
	case <-time.After(time.Second):
		t.Fatal("file worker did not start")
	}
	deadline := time.Now().Add(time.Second)
	for metrics.Snapshot(4, "stable", 0).FilesQueued != 3 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if got := metrics.Snapshot(4, "stable", 0).FilesQueued; got != 3 {
		t.Fatalf("queued producers=%d, want 3 (one queued and two blocked)", got)
	}
	cancelStable()
	unblock()
	done := make(chan struct{})
	go func() { pool.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("file queue did not drain after stable cancellation")
	}
	progress := metrics.Snapshot(4, "drain", 0)
	if progress.FilesCompleted != 4 || progress.FilesFailed != 0 || progress.FilesQueued != 0 || progress.FilesActive != 0 {
		t.Fatalf("stable cancellation lost planned files: %+v", progress)
	}
}
