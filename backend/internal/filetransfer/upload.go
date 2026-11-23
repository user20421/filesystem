package filetransfer

import (
	"bytes"
	"context"
	"database/sql"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

// UploadChunk 校验并原子替换一个分块；新对象先发布，元数据提交后再删除旧对象。
func (s *Service) UploadChunk(ctx context.Context, id, user string, index int, expectedSHA string, body io.Reader) error {
	expectedSHA = strings.ToLower(expectedSHA)
	if !shaPattern.MatchString(expectedSHA) {
		return apperror.New(apperror.KindBadRequest, "invalid_chunk_sha256", "X-Chunk-SHA256 must contain 64 hexadecimal characters")
	}
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, true); err != nil {
		return err
	}
	unlockContent := s.lock("content:" + f.ContentID)
	defer unlockContent()
	f, err = s.getFile(ctx, id)
	if err != nil {
		return err
	}
	expectedSize, err := expectedChunkSize(f, index)
	if err != nil {
		return err
	}
	if f.Status != "uploading" {
		if f.Status == "completed" {
			storedSize, storedSHA, err := s.repo.StoredChunk(ctx, f.ContentID, index)
			if err != nil && err != sql.ErrNoRows {
				return err
			}
			if err == nil && storedSize == expectedSize && hashesEqual(storedSHA, expectedSHA) {
				return nil
			}
		}
		return apperror.New(apperror.KindConflict, "invalid_file_state", "chunks can only be uploaded while the content is active")
	}
	releaseSlot, err := s.scheduler.acquire(ctx, f.ContentID)
	if err != nil {
		return err
	}
	defer releaseSlot()
	releaseMemory, err := s.memory.acquire(ctx, chunkMemoryWeight(expectedSize))
	if err != nil {
		return err
	}
	defer releaseMemory()
	plain, err := io.ReadAll(io.LimitReader(body, expectedSize+1))
	if err != nil {
		return err
	}
	if int64(len(plain)) != expectedSize {
		return apperror.New(apperror.KindBadRequest, "invalid_chunk_size", fmt.Sprintf("chunk %d must contain exactly %d bytes", index, expectedSize))
	}
	if !hashesEqual(SHA256(plain), expectedSHA) {
		return apperror.New(apperror.KindBadRequest, "chunk_checksum_mismatch", "chunk SHA256 does not match X-Chunk-SHA256")
	}
	key := fmt.Sprintf("contents/%s/%08d-%s", f.ContentID, index, uuid.NewString())
	if err := s.objects.Put(ctx, key, bytes.NewReader(plain), int64(len(plain))); err != nil {
		return err
	}
	committed := false
	defer func() {
		if !committed {
			cleanupCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			_ = s.objects.Remove(cleanupCtx, key)
		}
	}()
	oldKey, err := s.repo.StoreChunk(ctx, id, f.ContentID, index, key, plain)
	if err != nil {
		return err
	}
	committed = true
	if oldKey != "" && oldKey != key {
		cleanupCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = s.objects.Remove(cleanupCtx, oldKey)
	}
	return nil
}
