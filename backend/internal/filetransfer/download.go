package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"io"

	"secure-im/backend/internal/httpx"
)

func (s *Service) Download(ctx context.Context, id, user string, output io.Writer, onMetadata func(File)) error {
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
	if err := Authorize(f, user, false); err != nil {
		return err
	}
	if f.Status != "completed" {
		return httpx.Error(409, "file_not_ready", "only completed files can be downloaded")
	}
	chunks, err := s.chunks(ctx, id)
	if err != nil {
		return err
	}
	if len(chunks) != f.ChunkCount {
		return fmt.Errorf("completed file has missing chunks")
	}
	dek, err := UnwrapDEK(s.kek, f.WrappedDEK, f.DEKNonce)
	if err != nil {
		return err
	}
	onMetadata(f)
	for _, c := range chunks {
		plain, releaseMemory, err := s.readChunk(ctx, dek, c)
		if err != nil {
			return err
		}
		_, err = output.Write(plain)
		releaseMemory()
		if err != nil {
			return err
		}
	}
	return nil
}
func (s *Service) Delete(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, true)
}

// DeleteForAccount removes a transfer in which the deleted user is either the
// owner or recipient. It deliberately shares the normal per-file lock and
// object-first deletion path so interrupted account deletions can be retried.
func (s *Service) DeleteForAccount(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, false)
}

func (s *Service) delete(ctx context.Context, id, user string, ownerOnly bool) error {
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
	if err := Authorize(f, user, ownerOnly); err != nil {
		return err
	}
	if _, err := s.db.ExecContext(ctx, `UPDATE files SET status='deleting' WHERE id=?`, id); err != nil {
		return err
	}
	chunks, err := s.chunks(ctx, id)
	if err != nil {
		return err
	}
	for _, c := range chunks {
		if err := s.objects.Remove(ctx, c.ObjectKey); err != nil {
			return fmt.Errorf("remove encrypted object: %w", err)
		}
	}
	result, err := s.db.ExecContext(ctx, `DELETE FROM files WHERE id=? AND status='deleting'`, id)
	if err != nil {
		return err
	}
	if n, _ := result.RowsAffected(); n != 1 {
		return sql.ErrNoRows
	}
	return nil
}
