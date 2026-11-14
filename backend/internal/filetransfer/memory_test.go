package filetransfer

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"
)

func TestMemoryBudgetConcurrentReservationsNeverExceedLimit(t *testing.T) {
	budget := newMemoryBudget(128)
	var wg sync.WaitGroup
	var mu sync.Mutex
	var inFlight, peak int64
	errs := make(chan error, 64)
	for i := 0; i < 64; i++ {
		weight := int64(i%16 + 1)
		wg.Add(1)
		go func() {
			defer wg.Done()
			release, err := budget.acquire(context.Background(), weight)
			if err != nil {
				errs <- err
				return
			}
			mu.Lock()
			inFlight += weight
			peak = max(peak, inFlight)
			used, limit := budget.stats()
			if used > limit {
				errs <- fmt.Errorf("used %d exceeds limit %d", used, limit)
			}
			mu.Unlock()
			time.Sleep(time.Millisecond)
			mu.Lock()
			inFlight -= weight
			mu.Unlock()
			release()
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
	if peak > 128 {
		t.Fatalf("peak reservation %d exceeded 128", peak)
	}
	if used, _ := budget.stats(); used != 0 {
		t.Fatalf("used bytes after concurrent releases = %d, want 0", used)
	}
}

func TestMemoryBudgetReleaseWakesWaiter(t *testing.T) {
	budget := newMemoryBudget(100)
	releaseFull, err := budget.acquire(context.Background(), 100)
	if err != nil {
		t.Fatal(err)
	}
	acquired := make(chan func(), 1)
	go func() {
		release, err := budget.acquire(context.Background(), 25)
		if err != nil {
			t.Error(err)
			return
		}
		acquired <- release
	}()
	select {
	case <-acquired:
		t.Fatal("waiter acquired memory while budget was full")
	case <-time.After(30 * time.Millisecond):
	}
	releaseFull()
	releaseFull() // A duplicate release must not corrupt the budget.
	select {
	case release := <-acquired:
		if used, _ := budget.stats(); used != 25 {
			t.Fatalf("used bytes = %d, want 25", used)
		}
		release()
	case <-time.After(time.Second):
		t.Fatal("waiter was not awakened by release")
	}
}

func TestMemoryBudgetWaitHonorsContextTimeout(t *testing.T) {
	budget := newMemoryBudget(64)
	release, err := budget.acquire(context.Background(), 64)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	if _, err := budget.acquire(ctx, 1); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("acquire error = %v, want context deadline exceeded", err)
	}
	if used, _ := budget.stats(); used != 64 {
		t.Fatalf("timed out waiter changed used bytes to %d", used)
	}
	release()
	release, err = budget.acquire(context.Background(), 64)
	if err != nil {
		t.Fatalf("acquire after release failed: %v", err)
	}
	release()
}
