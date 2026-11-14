package stress

import (
	"context"
	"fmt"
	"sync"
)

// ByteBudget is a context-aware weighted semaphore for synthetic file buffers.
type ByteBudget struct {
	mu                sync.Mutex
	limit, used, high int64
	changed           chan struct{}
}

func NewByteBudget(limit int64) *ByteBudget {
	return &ByteBudget{limit: limit, changed: make(chan struct{})}
}
func (b *ByteBudget) Acquire(ctx context.Context, size int64) error {
	if size < 0 || size > b.limit {
		return fmt.Errorf("buffer size %d exceeds budget %d", size, b.limit)
	}
	for {
		b.mu.Lock()
		if b.used+size <= b.limit {
			b.used += size
			if b.used > b.high {
				b.high = b.used
			}
			b.mu.Unlock()
			return nil
		}
		changed := b.changed
		b.mu.Unlock()
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-changed:
		}
	}
}
func (b *ByteBudget) Release(size int64) {
	b.mu.Lock()
	b.used -= size
	if b.used < 0 {
		panic("file budget released too much")
	}
	close(b.changed)
	b.changed = make(chan struct{})
	b.mu.Unlock()
}
func (b *ByteBudget) Snapshot() (used, high, limit int64) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.used, b.high, b.limit
}
