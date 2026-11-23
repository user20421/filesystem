package filetransfer

import (
	"context"
	"fmt"
	"io"
)

func (s *Service) chunks(ctx context.Context, contentID string) ([]Chunk, error) {
	return s.repo.Chunks(ctx, contentID)
}

// readChunk 获取并校验一个明文分块，调度槽与内存预算由返回值统一释放。
func (s *Service) readChunk(ctx context.Context, fileKey string, chunk Chunk) (plain []byte, release func(), err error) {
	releaseSlot, err := s.scheduler.acquire(ctx, fileKey)
	if err != nil {
		return nil, nil, err
	}
	releaseMemory, err := s.memory.acquire(ctx, chunkMemoryWeight(chunk.PlaintextSize))
	if err != nil {
		releaseSlot()
		return nil, nil, err
	}
	release = func() {
		releaseMemory()
		releaseSlot()
	}
	success := false
	defer func() {
		if !success {
			releaseMemory()
			releaseSlot()
		}
	}()
	object, err := s.objects.Get(ctx, chunk.ObjectKey)
	if err != nil {
		return nil, nil, err
	}
	defer object.Close()
	plain, err = io.ReadAll(io.LimitReader(object, chunk.PlaintextSize+1))
	if err != nil {
		return nil, nil, err
	}
	if int64(len(plain)) != chunk.PlaintextSize || !hashesEqual(SHA256(plain), chunk.SHA256) {
		return nil, nil, fmt.Errorf("plaintext checksum mismatch for chunk %d", chunk.Index)
	}
	success = true
	return plain, release, nil
}
