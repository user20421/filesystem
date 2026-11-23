package filetransfer

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"

	"secure-im/backend/internal/apperror"
)

// Complete 按序读取并校验全部分块，只有 revision 未变化时才将内容置为 ready。
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
		return f, apperror.New(apperror.KindConflict, "invalid_file_state", "file is not awaiting completion")
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
		return f, apperror.New(apperror.KindConflict, "invalid_file_state", "file is not awaiting completion")
	}
	chunks, err := s.chunks(ctx, f.ContentID)
	if err != nil {
		return f, err
	}
	if len(chunks) != f.ChunkCount {
		return f, apperror.New(apperror.KindConflict, "missing_chunks", "all chunks must be uploaded before completion")
	}
	hash := sha256.New()
	for i, chunk := range chunks {
		if chunk.Index != i {
			return f, apperror.New(apperror.KindConflict, "missing_chunks", "chunk sequence is incomplete")
		}
		expected, _ := expectedChunkSize(f, i)
		if chunk.PlaintextSize != expected {
			return f, fmt.Errorf("stored chunk size mismatch")
		}
		plain, releaseChunk, err := s.readChunk(ctx, f.ContentID, chunk)
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
		return f, apperror.New(apperror.KindConflict, "file_checksum_mismatch", "assembled plaintext SHA256 does not match the initialized file")
	}
	ready, err := s.repo.MarkReady(ctx, f.ContentID, f.Revision)
	if err != nil {
		return f, err
	}
	if !ready {
		current, getErr := s.getFile(ctx, id)
		if getErr != nil {
			return f, getErr
		}
		if current.Status == "completed" {
			return current, nil
		}
		return f, apperror.New(apperror.KindConflict, "file_changed", "chunks changed during completion; retry completion")
	}
	return s.getFile(ctx, id)
}
