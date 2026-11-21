package filetransfer

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/google/uuid"
	"secure-im/backend/internal/httpx"
)

func (s *Service) UploadChunk(ctx context.Context, id, user string, index int, expectedSHA string, body io.Reader) error {
	expectedSHA = strings.ToLower(expectedSHA)
	if !shaPattern.MatchString(expectedSHA) {
		return httpx.Error(400, "invalid_chunk_sha256", "X-Chunk-SHA256 must contain 64 hexadecimal characters")
	}
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, true); err != nil {
		return err
	}
	// Completion takes a content snapshot and reads its object keys. Serialize
	// replacements for the same shared content so those keys cannot be deleted
	// while completion is reading them. Different contents remain independent.
	unlockContent := s.lock("content:" + f.ContentID)
	defer unlockContent()
	unlock := s.lock(fmt.Sprintf("chunk:%s:%d", f.ContentID, index))
	defer unlock()
	f, err = s.getFile(ctx, id)
	if err != nil {
		return err
	}
	expectedSize, err := expectedChunkSize(f, index)
	if err != nil {
		return err
	}
	if f.Status != "uploading" {
		// Another logical reference may complete this shared content while an
		// uploader is between chunks. A matching retry is already satisfied.
		if f.Status == "completed" {
			var storedSize int64
			var storedSHA string
			err := s.db.QueryRowContext(ctx, `SELECT plaintext_size,sha256 FROM file_chunks WHERE content_id=? AND chunk_index=?`, f.ContentID, index).Scan(&storedSize, &storedSHA)
			if err != nil && err != sql.ErrNoRows {
				return err
			}
			if err == nil && storedSize == expectedSize && hashesEqual(storedSHA, expectedSHA) {
				return nil
			}
		}
		return httpx.Error(409, "invalid_file_state", "chunks can only be uploaded while the content is active")
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
		return httpx.Error(400, "invalid_chunk_size", fmt.Sprintf("chunk %d must contain exactly %d bytes", index, expectedSize))
	}
	if !hashesEqual(SHA256(plain), expectedSHA) {
		return httpx.Error(400, "chunk_checksum_mismatch", "chunk SHA256 does not match X-Chunk-SHA256")
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
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var contentStatus, fileStatus string
	err = tx.QueryRowContext(ctx, `SELECT c.status,f.status FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? AND f.content_id=? FOR UPDATE`, id, f.ContentID).Scan(&contentStatus, &fileStatus)
	if errorsIsNoRows(err) {
		return httpx.Error(404, "file_not_found", "file not found")
	}
	if err != nil {
		return err
	}
	if contentStatus != "uploading" || fileStatus != "active" {
		return httpx.Error(409, "invalid_file_state", "file state changed during chunk upload")
	}
	var oldKey string
	err = tx.QueryRowContext(ctx, `SELECT object_key FROM file_chunks WHERE content_id=? AND chunk_index=? FOR UPDATE`, f.ContentID, index).Scan(&oldKey)
	if err != nil && err != sql.ErrNoRows {
		return err
	}
	if err == sql.ErrNoRows {
		_, err = tx.ExecContext(ctx, `INSERT INTO file_chunks(content_id,chunk_index,object_key,plaintext_size,sha256) VALUES(?,?,?,?,?)`, f.ContentID, index, key, len(plain), SHA256(plain))
	} else {
		_, err = tx.ExecContext(ctx, `UPDATE file_chunks SET object_key=?,plaintext_size=?,sha256=?,created_at=NOW(6) WHERE content_id=? AND chunk_index=?`, key, len(plain), SHA256(plain), f.ContentID, index)
	}
	if err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET revision=revision+1 WHERE id=? AND status='uploading'`, f.ContentID); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
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

func errorsIsNoRows(err error) bool { return err == sql.ErrNoRows }

func (s *Service) chunks(ctx context.Context, contentID string) ([]Chunk, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT chunk_index,object_key,plaintext_size,sha256 FROM file_chunks WHERE content_id=? ORDER BY chunk_index`, contentID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	chunks := make([]Chunk, 0)
	for rows.Next() {
		var c Chunk
		if err := rows.Scan(&c.Index, &c.ObjectKey, &c.PlaintextSize, &c.SHA256); err != nil {
			return nil, err
		}
		chunks = append(chunks, c)
	}
	return chunks, rows.Err()
}
func (s *Service) readChunk(ctx context.Context, fileKey string, c Chunk) (plain []byte, release func(), err error) {
	releaseSlot, err := s.scheduler.acquire(ctx, fileKey)
	if err != nil {
		return nil, nil, err
	}
	releaseMemory, err := s.memory.acquire(ctx, chunkMemoryWeight(c.PlaintextSize))
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
			// Error returns clear the named release result. Use the acquired
			// resources directly so cleanup cannot call a nil function.
			releaseMemory()
			releaseSlot()
		}
	}()
	object, err := s.objects.Get(ctx, c.ObjectKey)
	if err != nil {
		return nil, nil, err
	}
	defer object.Close()
	plain, err = io.ReadAll(io.LimitReader(object, c.PlaintextSize+1))
	if err != nil {
		return nil, nil, err
	}
	if int64(len(plain)) != c.PlaintextSize || !hashesEqual(SHA256(plain), c.SHA256) {
		return nil, nil, fmt.Errorf("plaintext checksum mismatch for chunk %d", c.Index)
	}
	success = true
	return plain, release, nil
}

func (s *Service) Complete(ctx context.Context, id, user string) (File, error) {
	f, err := s.getFile(ctx, id)
	if err != nil {
		return f, err
	}
	if err := Authorize(f, user, true); err != nil {
		return f, err
	}
	if f.Status == "completed" {
		return f, nil
	}
	if f.Status != "uploading" {
		return f, httpx.Error(409, "invalid_file_state", "file is not awaiting completion")
	}
	unlockContent := s.lock("content:" + f.ContentID)
	defer unlockContent()
	f, err = s.getFile(ctx, id)
	if err != nil {
		return f, err
	}
	if f.Status == "completed" {
		return f, nil
	}
	if f.Status != "uploading" {
		return f, httpx.Error(409, "invalid_file_state", "file is not awaiting completion")
	}
	chunks, err := s.chunks(ctx, f.ContentID)
	if err != nil {
		return f, err
	}
	if len(chunks) != f.ChunkCount {
		return f, httpx.Error(409, "missing_chunks", "all chunks must be uploaded before completion")
	}
	hash := sha256.New()
	for i, c := range chunks {
		if c.Index != i {
			return f, httpx.Error(409, "missing_chunks", "chunk sequence is incomplete")
		}
		expected, _ := expectedChunkSize(f, i)
		if c.PlaintextSize != expected {
			return f, fmt.Errorf("stored chunk size mismatch")
		}
		plain, releaseChunk, err := s.readChunk(ctx, f.ContentID, c)
		if err != nil {
			return f, err
		}
		_, err = hash.Write(plain)
		releaseChunk()
		if err != nil {
			return f, err
		}
	}
	if !hashesEqual(hex.EncodeToString(hash.Sum(nil)), f.SHA256) {
		return f, httpx.Error(409, "file_checksum_mismatch", "assembled plaintext SHA256 does not match the initialized file")
	}
	result, err := s.db.ExecContext(ctx, `UPDATE file_contents SET status='ready',completed_at=NOW(6) WHERE id=? AND status='uploading' AND revision=?`, f.ContentID, f.Revision)
	if err != nil {
		return f, err
	}
	if n, _ := result.RowsAffected(); n != 1 {
		current, getErr := s.getFile(ctx, id)
		if getErr != nil {
			return f, getErr
		}
		if current.Status == "completed" {
			return current, nil
		}
		return f, httpx.Error(409, "file_changed", "chunks changed during completion; retry completion")
	}
	return s.getFile(ctx, id)
}
