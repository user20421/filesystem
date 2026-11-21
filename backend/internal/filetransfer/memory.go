package filetransfer

import (
	"context"
	"fmt"
	"sync"
)

// Account for the single plaintext buffer plus hashing and object-store
// bookkeeping. Plain chunks are never duplicated for encryption.
const chunkOperationOverhead = int64(64 << 10)

func chunkMemoryWeight(plaintextSize int64) int64 {
	return plaintextSize + chunkOperationOverhead
}

type memoryBudget struct {
	mu      sync.Mutex
	used    int64
	limit   int64
	changed chan struct{}
}

func newMemoryBudget(limit int64) *memoryBudget {
	return &memoryBudget{limit: limit, changed: make(chan struct{})}
}

// acquire reserves weight bytes without ever allowing used to exceed limit.
// The returned release function is idempotent and must be called when all
// buffers covered by the reservation are no longer in use.
func (b *memoryBudget) acquire(ctx context.Context, weight int64) (func(), error) {
	if weight < 0 {
		return nil, fmt.Errorf("negative file memory reservation")
	}
	if weight == 0 {
		return func() {}, nil
	}
	for {
		b.mu.Lock()
		if weight <= b.limit-b.used {
			b.used += weight
			b.mu.Unlock()
			var once sync.Once
			return func() {
				once.Do(func() { b.release(weight) })
			}, nil
		}
		if weight > b.limit {
			b.mu.Unlock()
			return nil, fmt.Errorf("file memory reservation of %d bytes exceeds budget of %d bytes", weight, b.limit)
		}
		changed := b.changed
		b.mu.Unlock()

		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-changed:
		}
	}
}

func (b *memoryBudget) release(weight int64) {
	b.mu.Lock()
	b.used -= weight
	if b.used < 0 {
		b.mu.Unlock()
		panic("file memory budget released more than reserved")
	}
	close(b.changed)
	b.changed = make(chan struct{})
	b.mu.Unlock()
}

func (b *memoryBudget) stats() (used, limit int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.used, b.limit
}
