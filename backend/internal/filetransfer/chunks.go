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
	if err := s.acquire(ctx); err != nil {
		return err
	}
	defer s.release()
	unlock := s.lock("file:" + id)
	defer unlock()
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, true); err != nil {
		return err
	}
	if f.Status != "uploading" {
		return httpx.Error(409, "invalid_file_state", "chunks can only be uploaded while the file is active")
	}
	expectedSize, err := expectedChunkSize(f, index)
	if err != nil {
		return err
	}
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
	dek, err := UnwrapDEK(s.kek, f.WrappedDEK, f.DEKNonce)
	if err != nil {
		return err
	}
	ciphertext, nonce, err := EncryptChunk(dek, plain)
	if err != nil {
		return err
	}
	key := fmt.Sprintf("files/%s/%08d-%s", id, index, uuid.NewString())
	if err := s.objects.Put(ctx, key, bytes.NewReader(ciphertext), int64(len(ciphertext))); err != nil {
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
	var oldKey string
	err = tx.QueryRowContext(ctx, `SELECT object_key FROM file_chunks WHERE file_id=? AND chunk_index=? FOR UPDATE`, id, index).Scan(&oldKey)
	if err != nil && err != sql.ErrNoRows {
		return err
	}
	if err == sql.ErrNoRows {
		_, err = tx.ExecContext(ctx, `INSERT INTO file_chunks(file_id,chunk_index,object_key,plaintext_size,sha256,nonce) VALUES(?,?,?,?,?,?)`, id, index, key, len(plain), SHA256(plain), nonce)
	} else {
		_, err = tx.ExecContext(ctx, `UPDATE file_chunks SET object_key=?,plaintext_size=?,sha256=?,nonce=?,created_at=NOW(6) WHERE file_id=? AND chunk_index=?`, key, len(plain), SHA256(plain), nonce, id, index)
	}
	if err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	committed = true
	if oldKey != "" && oldKey != key {
		_ = s.objects.Remove(ctx, oldKey)
	}
	return nil
}
func (s *Service) chunks(ctx context.Context, id string) ([]Chunk, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT chunk_index,object_key,plaintext_size,sha256,nonce FROM file_chunks WHERE file_id=? ORDER BY chunk_index`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	chunks := make([]Chunk, 0)
	for rows.Next() {
		var c Chunk
		if err := rows.Scan(&c.Index, &c.ObjectKey, &c.PlaintextSize, &c.SHA256, &c.Nonce); err != nil {
			return nil, err
		}
		chunks = append(chunks, c)
	}
	return chunks, rows.Err()
}
func (s *Service) readChunk(ctx context.Context, dek []byte, c Chunk) (plain []byte, releaseMemory func(), err error) {
	releaseMemory, err = s.memory.acquire(ctx, chunkMemoryWeight(c.PlaintextSize))
	if err != nil {
		return nil, nil, err
	}
	success := false
	defer func() {
		if !success {
			releaseMemory()
		}
	}()
	object, err := s.objects.Get(ctx, c.ObjectKey)
	if err != nil {
		return nil, nil, err
	}
	defer object.Close()
	ciphertext, err := io.ReadAll(io.LimitReader(object, c.PlaintextSize+17))
	if err != nil {
		return nil, nil, err
	}
	if int64(len(ciphertext)) != c.PlaintextSize+chunkCiphertextOverhead {
		return nil, nil, fmt.Errorf("ciphertext size mismatch for chunk %d", c.Index)
	}
	plain, err = DecryptChunk(dek, ciphertext, c.Nonce)
	if err != nil {
		return nil, nil, err
	}
	if int64(len(plain)) != c.PlaintextSize || !hashesEqual(SHA256(plain), c.SHA256) {
		return nil, nil, fmt.Errorf("plaintext checksum mismatch for chunk %d", c.Index)
	}
	success = true
	return plain, releaseMemory, nil
}
func (s *Service) Complete(ctx context.Context, id, user string) (File, error) {
	if err := s.acquire(ctx); err != nil {
		return File{}, err
	}
	defer s.release()
	unlock := s.lock("file:" + id)
	defer unlock()
	f, err := s.getFile(ctx, id)
	if err != nil {
		return f, err
	}
	if err := Authorize(f, user, true); err != nil {
		return f, err
	}
	if f.Status != "uploading" {
		return f, httpx.Error(409, "invalid_file_state", "file is not awaiting completion")
	}
	chunks, err := s.chunks(ctx, id)
	if err != nil {
		return f, err
	}
	if len(chunks) != f.ChunkCount {
		return f, httpx.Error(409, "missing_chunks", "all chunks must be uploaded before completion")
	}
	dek, err := UnwrapDEK(s.kek, f.WrappedDEK, f.DEKNonce)
	if err != nil {
		return f, err
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
		plain, releaseMemory, err := s.readChunk(ctx, dek, c)
		if err != nil {
			return f, err
		}
		_, err = hash.Write(plain)
		releaseMemory()
		if err != nil {
			return f, err
		}
	}
	if !hashesEqual(hex.EncodeToString(hash.Sum(nil)), f.SHA256) {
		return f, httpx.Error(409, "file_checksum_mismatch", "assembled plaintext SHA256 does not match the initialized file")
	}
	result, err := s.db.ExecContext(ctx, `UPDATE files SET status='completed',completed_at=NOW(6) WHERE id=? AND status='uploading'`, id)
	if err != nil {
		return f, err
	}
	if n, _ := result.RowsAffected(); n != 1 {
		return f, httpx.Error(409, "invalid_file_state", "file state changed during completion")
	}
	return s.getFile(ctx, id)
}
