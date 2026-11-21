package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"io"

	"secure-im/backend/internal/httpx"
)

func (s *Service) Download(ctx context.Context, id, user string, output io.Writer, onMetadata func(File)) error {
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
	chunks, err := s.chunks(ctx, f.ContentID)
	if err != nil {
		return err
	}
	if len(chunks) != f.ChunkCount {
		return fmt.Errorf("completed file has missing chunks")
	}
	onMetadata(f)
	for _, c := range chunks {
		plain, releaseChunk, err := s.readChunk(ctx, f.ContentID, c)
		if err != nil {
			return err
		}
		_, err = output.Write(plain)
		releaseChunk()
		if err != nil {
			return err
		}
	}
	return nil
}
func (s *Service) Delete(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, true)
}

// DeleteForAccount removes either participant's logical reference. Shared
// contents and their objects survive until the final reference is removed.
func (s *Service) DeleteForAccount(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, false)
}

func (s *Service) delete(ctx context.Context, id, user string, ownerOnly bool) error {
	unlock := s.lock("file:" + id)
	defer unlock()
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, ownerOnly); err != nil {
		return err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var contentID, fileStatus, contentStatus string
	var refs int
	err = tx.QueryRowContext(ctx, `SELECT f.content_id,f.status,c.status,c.ref_count FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`, id).Scan(&contentID, &fileStatus, &contentStatus, &refs)
	if err == sql.ErrNoRows {
		return httpx.Error(404, "file_not_found", "file not found")
	}
	if err != nil {
		return err
	}
	last := fileStatus == "deleting" || (refs == 1 && fileStatus == "active")
	if fileStatus == "active" {
		if refs < 1 {
			return fmt.Errorf("invalid content reference count")
		}
		if last {
			if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`, contentID); err != nil {
				return err
			}
			if _, err = tx.ExecContext(ctx, `UPDATE files SET status='deleting' WHERE id=? AND status='active'`, id); err != nil {
				return err
			}
		} else {
			if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=ref_count-1 WHERE id=? AND ref_count>1`, contentID); err != nil {
				return err
			}
			if _, err = tx.ExecContext(ctx, `DELETE FROM files WHERE id=? AND status='active'`, id); err != nil {
				return err
			}
		}
	} else if fileStatus != "deleting" || contentStatus != "deleting" || refs != 0 {
		return fmt.Errorf("invalid deleting file state")
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	if !last {
		return nil
	}
	chunks, err := s.chunks(ctx, contentID)
	if err != nil {
		return err
	}
	for _, c := range chunks {
		if err := s.objects.Remove(ctx, c.ObjectKey); err != nil {
			return fmt.Errorf("remove plaintext object: %w", err)
		}
	}
	tx, err = s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var stillDeleting int
	if err = tx.QueryRowContext(ctx, `SELECT COUNT(1) FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0 FOR UPDATE`, contentID).Scan(&stillDeleting); err != nil {
		return err
	}
	if stillDeleting != 1 {
		return fmt.Errorf("content state changed during deletion")
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM files WHERE id=? AND status='deleting'`, id); err != nil {
		return err
	}
	result, err := tx.ExecContext(ctx, `DELETE FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0`, contentID)
	if err != nil {
		return err
	}
	if n, _ := result.RowsAffected(); n != 1 {
		return sql.ErrNoRows
	}
	return tx.Commit()
}
