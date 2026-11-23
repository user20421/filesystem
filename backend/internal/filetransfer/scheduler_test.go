package filetransfer

import (
	"context"
	"errors"
	"runtime"
	"testing"
	"time"
)

type schedulerAcquire struct {
	acquired chan func()
	err      chan error
}

func startSchedulerAcquire(ctx context.Context, scheduler *chunkScheduler, fileKey string) *schedulerAcquire {
	call := &schedulerAcquire{acquired: make(chan func(), 1), err: make(chan error, 1)}
	go func() {
		release, err := scheduler.acquire(ctx, fileKey)
		if err != nil {
			call.err <- err
			return
		}
		call.acquired <- release
	}()
	return call
}

func waitSchedulerAcquired(t *testing.T, call *schedulerAcquire) func() {
	t.Helper()
	select {
	case release := <-call.acquired:
		return release
	case err := <-call.err:
		t.Fatalf("scheduler acquire failed: %v", err)
	case <-time.After(time.Second):
		t.Fatal("scheduler acquire did not complete")
	}
	return nil
}

func waitSchedulerQueue(t *testing.T, scheduler *chunkScheduler, length int) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for {
		scheduler.mu.Lock()
		got := len(scheduler.queue)
		scheduler.mu.Unlock()
		if got == length {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("scheduler queue length = %d, want %d", got, length)
		}
		runtime.Gosched()
	}
}

func schedulerState(scheduler *chunkScheduler) (active, queued int, byKey map[string]int) {
	scheduler.mu.Lock()
	defer scheduler.mu.Unlock()
	byKey = make(map[string]int, len(scheduler.activeByKey))
	for key, count := range scheduler.activeByKey {
		byKey[key] = count
	}
	return scheduler.active, len(scheduler.queue), byKey
}

func TestChunkSchedulerStats(t *testing.T) {
	scheduler := newChunkScheduler(96)
	if active, limit := scheduler.Stats(); active != 0 || limit != 96 {
		t.Fatalf("initial stats = %d/%d, want 0/96", active, limit)
	}
	release := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	if active, limit := scheduler.Stats(); active != 1 || limit != 96 {
		t.Fatalf("active stats = %d/%d, want 1/96", active, limit)
	}
	release()
	if active, limit := scheduler.Stats(); active != 0 || limit != 96 {
		t.Fatalf("released stats = %d/%d, want 0/96", active, limit)
	}
}

func TestChunkSchedulerRoundRobinAcrossFiles(t *testing.T) {
	scheduler := newChunkScheduler(1)
	releaseA := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	b := startSchedulerAcquire(context.Background(), scheduler, "b")
	waitSchedulerQueue(t, scheduler, 1)
	c := startSchedulerAcquire(context.Background(), scheduler, "c")
	waitSchedulerQueue(t, scheduler, 2)
	aNext := startSchedulerAcquire(context.Background(), scheduler, "a")
	waitSchedulerQueue(t, scheduler, 3)

	releaseA()
	releaseB := waitSchedulerAcquired(t, b)
	releaseB()
	releaseC := waitSchedulerAcquired(t, c)
	releaseC()
	releaseANext := waitSchedulerAcquired(t, aNext)
	releaseANext()

	active, queued, _ := schedulerState(scheduler)
	if active != 0 || queued != 0 {
		t.Fatalf("scheduler leaked work: active=%d queued=%d", active, queued)
	}
}

func TestChunkSchedulerDoesNotGiveCompetingFileMultipleSlots(t *testing.T) {
	scheduler := newChunkScheduler(3)
	releaseA := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	releaseB := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "b"))
	aSecond := startSchedulerAcquire(context.Background(), scheduler, "a")
	aThird := startSchedulerAcquire(context.Background(), scheduler, "a")
	waitSchedulerQueue(t, scheduler, 2)

	active, _, byKey := schedulerState(scheduler)
	if active != 2 || byKey["a"] != 1 || byKey["b"] != 1 {
		t.Fatalf("competing files received wrong slots: active=%d byKey=%v", active, byKey)
	}

	releaseB()
	releaseASecond := waitSchedulerAcquired(t, aSecond)
	releaseA()
	releaseAThird := waitSchedulerAcquired(t, aThird)
	releaseASecond()
	releaseAThird()
}

func TestChunkSchedulerNewFilePrecedesSameFileBacklog(t *testing.T) {
	scheduler := newChunkScheduler(1)
	releaseA := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	aNext := startSchedulerAcquire(context.Background(), scheduler, "a")
	waitSchedulerQueue(t, scheduler, 1)
	b := startSchedulerAcquire(context.Background(), scheduler, "b")
	waitSchedulerQueue(t, scheduler, 2)

	releaseA()
	releaseB := waitSchedulerAcquired(t, b)
	if _, queued, _ := schedulerState(scheduler); queued != 1 {
		t.Fatalf("same-file backlog was not kept waiting: queued=%d", queued)
	}
	releaseB()
	releaseANext := waitSchedulerAcquired(t, aNext)
	releaseANext()
}

func TestChunkSchedulerSingleFileUsesIdleSlots(t *testing.T) {
	scheduler := newChunkScheduler(3)
	releases := make([]func(), 0, 4)
	for i := 0; i < 3; i++ {
		releases = append(releases, waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a")))
	}
	fourth := startSchedulerAcquire(context.Background(), scheduler, "a")
	waitSchedulerQueue(t, scheduler, 1)
	releases[0]()
	releases = append(releases, waitSchedulerAcquired(t, fourth))
	for _, release := range releases[1:] {
		release()
	}
	active, queued, _ := schedulerState(scheduler)
	if active != 0 || queued != 0 {
		t.Fatalf("scheduler leaked work: active=%d queued=%d", active, queued)
	}
}

func TestChunkSchedulerCancellationRemovesWaiter(t *testing.T) {
	scheduler := newChunkScheduler(1)
	releaseA := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	ctx, cancel := context.WithCancel(context.Background())
	b := startSchedulerAcquire(ctx, scheduler, "b")
	c := startSchedulerAcquire(context.Background(), scheduler, "c")
	waitSchedulerQueue(t, scheduler, 2)
	cancel()
	select {
	case err := <-b.err:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("canceled acquire error = %v", err)
		}
	case release := <-b.acquired:
		release()
		t.Fatal("canceled waiter acquired a slot")
	case <-time.After(time.Second):
		t.Fatal("canceled waiter did not return")
	}
	waitSchedulerQueue(t, scheduler, 1)
	releaseA()
	releaseC := waitSchedulerAcquired(t, c)
	releaseC()
	active, queued, byKey := schedulerState(scheduler)
	if active != 0 || queued != 0 || len(byKey) != 0 {
		t.Fatalf("cancellation leaked scheduler state: active=%d queued=%d byKey=%v", active, queued, byKey)
	}
}

func TestChunkSchedulerReleaseWakesWaiter(t *testing.T) {
	scheduler := newChunkScheduler(1)
	releaseA := waitSchedulerAcquired(t, startSchedulerAcquire(context.Background(), scheduler, "a"))
	b := startSchedulerAcquire(context.Background(), scheduler, "b")
	waitSchedulerQueue(t, scheduler, 1)
	releaseA()
	releaseB := waitSchedulerAcquired(t, b)
	releaseB()
	releaseB() // A duplicate release must not wake unrelated work or corrupt counts.
	active, queued, _ := schedulerState(scheduler)
	if active != 0 || queued != 0 {
		t.Fatalf("release leaked scheduler state: active=%d queued=%d", active, queued)
	}
}

func TestChunkSchedulerHonorsConfiguredLimit(t *testing.T) {
	scheduler := newChunkScheduler(2)
	calls := make([]*schedulerAcquire, 5)
	releases := make([]func(), 0, len(calls))
	for i := 0; i < 2; i++ {
		calls[i] = startSchedulerAcquire(context.Background(), scheduler, "a")
		releases = append(releases, waitSchedulerAcquired(t, calls[i]))
	}
	for i := 2; i < len(calls); i++ {
		calls[i] = startSchedulerAcquire(context.Background(), scheduler, "a")
		waitSchedulerQueue(t, scheduler, i-1)
	}
	for i := 2; i < len(calls); i++ {
		active, _, _ := schedulerState(scheduler)
		if active != 2 {
			t.Fatalf("active slots = %d, want configured limit 2", active)
		}
		releases[i-2]()
		releases = append(releases, waitSchedulerAcquired(t, calls[i]))
	}
	for _, release := range releases[len(releases)-2:] {
		release()
	}
	active, queued, _ := schedulerState(scheduler)
	if active != 0 || queued != 0 {
		t.Fatalf("limit test leaked work: active=%d queued=%d", active, queued)
	}
}
