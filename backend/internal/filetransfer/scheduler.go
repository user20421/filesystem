package filetransfer

import (
	"context"
	"sync"
)

type chunkRequestState uint8

const (
	chunkRequestWaiting chunkRequestState = iota
	chunkRequestGranted
	chunkRequestDone
)

type chunkRequest struct {
	fileKey string
	ready   chan struct{}
	state   chunkRequestState
}

// chunkScheduler fairly allocates a bounded number of chunk-operation slots.
// While multiple files compete, each file can hold at most one slot. A single
// file may use all otherwise idle slots.
type chunkScheduler struct {
	mu          sync.Mutex
	limit       int
	active      int
	activeByKey map[string]int
	lastKey     string
	queue       []*chunkRequest
}

func newChunkScheduler(limit int) *chunkScheduler {
	return &chunkScheduler{limit: limit, activeByKey: make(map[string]int)}
}

// Stats returns a consistent snapshot of the currently granted slots and cap.
func (s *chunkScheduler) Stats() (active, limit int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.active, s.limit
}

// acquire 为分块操作申请槽位。等待、已授权和已结束状态均受 mu 保护；取消与授权同时发生时必须立即归还未使用槽位。
func (s *chunkScheduler) acquire(ctx context.Context, fileKey string) (func(), error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	req := &chunkRequest{fileKey: fileKey, ready: make(chan struct{}), state: chunkRequestWaiting}

	s.mu.Lock()
	if err := ctx.Err(); err != nil {
		s.mu.Unlock()
		return nil, err
	}
	s.queue = append(s.queue, req)
	s.dispatchLocked()
	if req.state == chunkRequestGranted {
		s.mu.Unlock()
		return s.releaseFunc(req), nil
	}
	s.mu.Unlock()

	select {
	case <-req.ready:
		return s.releaseFunc(req), nil
	case <-ctx.Done():
		s.mu.Lock()
		switch req.state {
		case chunkRequestWaiting:
			s.removeWaitingLocked(req)
			req.state = chunkRequestDone
			s.dispatchLocked()
		case chunkRequestGranted:
			// 取消与授权可能同时发生；已授权但未使用的槽位必须立即归还并唤醒后续请求。
			// Cancellation can race with a grant. Return the unused slot rather
			// than leaking it or running the canceled operation.
			s.releaseLocked(req)
		}
		s.mu.Unlock()
		return nil, ctx.Err()
	}
}

func (s *chunkScheduler) releaseFunc(req *chunkRequest) func() {
	var once sync.Once
	return func() {
		once.Do(func() {
			s.mu.Lock()
			s.releaseLocked(req)
			s.mu.Unlock()
		})
	}
}

func (s *chunkScheduler) releaseLocked(req *chunkRequest) {
	if req.state != chunkRequestGranted {
		return
	}
	req.state = chunkRequestDone
	s.lastKey = req.fileKey
	s.active--
	s.activeByKey[req.fileKey]--
	if s.activeByKey[req.fileKey] == 0 {
		delete(s.activeByKey, req.fileKey)
	}
	s.dispatchLocked()
}

func (s *chunkScheduler) dispatchLocked() {
	for s.active < s.limit && len(s.queue) > 0 {
		keys := make(map[string]struct{}, len(s.activeByKey)+len(s.queue))
		for key := range s.activeByKey {
			keys[key] = struct{}{}
		}
		for _, req := range s.queue {
			keys[req.fileKey] = struct{}{}
		}
		competing := len(keys) > 1

		selected := -1
		// 多文件竞争时避开刚完成的文件，优先让其他空闲文件获得下一轮；FIFO 仅用于同优先级之间决胜。
		// After a chunk finishes, prefer another inactive file over the
		// finished file's backlog. This is the per-file round-robin turn;
		// FIFO order breaks ties between different eligible files.
		for pass := 0; pass < 2 && selected < 0; pass++ {
			for i, req := range s.queue {
				if competing && s.activeByKey[req.fileKey] > 0 {
					continue
				}
				if competing && pass == 0 && req.fileKey == s.lastKey {
					continue
				}
				selected = i
				break
			}
		}
		if selected < 0 {
			return
		}

		req := s.queue[selected]
		copy(s.queue[selected:], s.queue[selected+1:])
		s.queue[len(s.queue)-1] = nil
		s.queue = s.queue[:len(s.queue)-1]
		req.state = chunkRequestGranted
		s.active++
		s.activeByKey[req.fileKey]++
		close(req.ready)
	}
}

func (s *chunkScheduler) removeWaitingLocked(target *chunkRequest) {
	for i, req := range s.queue {
		if req != target {
			continue
		}
		copy(s.queue[i:], s.queue[i+1:])
		s.queue[len(s.queue)-1] = nil
		s.queue = s.queue[:len(s.queue)-1]
		return
	}
}
