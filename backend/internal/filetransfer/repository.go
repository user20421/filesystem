package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"time"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

// repository 是文件传输的窄持久化接口；对象存储、键锁与调度仍由 Service 协调。
type repository interface {
	File(context.Context, string) (File, error)
	RecipientExists(context.Context, string) (bool, error)
	CreateFile(context.Context, string, InitInput, int) (string, error)
	UploadedChunkIndexes(context.Context, string) ([]int, error)
	StoredChunk(context.Context, string, int) (int64, string, error)
	StoreChunk(context.Context, string, string, int, string, []byte) (string, error)
	Chunks(context.Context, string) ([]Chunk, error)
	MarkReady(context.Context, string, int64) (bool, error)
	BeginDeletion(context.Context, string, string, time.Time) (string, bool, error)
	FinishDeletion(context.Context, string, string) error
	CleanupCandidates(context.Context, time.Time, time.Time, int) ([]cleanupCandidate, error)
}

// sqlRepository 集中保存文件 SQL，方法顺序与原事务和锁读取完全一致。
type sqlRepository struct{ db *sql.DB }

func (r *sqlRepository) File(ctx context.Context, id string) (File, error) {
	return scanFile(r.db.QueryRowContext(ctx, selectFile+`WHERE f.id=?`, id))
}

func (r *sqlRepository) RecipientExists(ctx context.Context, id string) (bool, error) {
	var exists int
	if err := r.db.QueryRowContext(ctx, `SELECT COUNT(1) FROM users WHERE id=?`, id).Scan(&exists); err != nil {
		return false, err
	}
	return exists == 1, nil
}

// CreateFile 在同一事务中创建/锁定内容、增加引用并写入逻辑文件。
func (r *sqlRepository) CreateFile(ctx context.Context, owner string, in InitInput, maxActive int) (string, error) {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	contentID := uuid.NewString()
	_, err = tx.ExecContext(ctx, `INSERT INTO file_contents(id,size,sha256,chunk_size,chunk_count) VALUES(?,?,?,?,?) ON DUPLICATE KEY UPDATE id=id`, contentID, in.Size, in.SHA256, in.ChunkSize, int((in.Size+in.ChunkSize-1)/in.ChunkSize))
	if err != nil {
		return "", err
	}
	var status string
	err = tx.QueryRowContext(ctx, `SELECT id,status FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`, in.Size, in.SHA256).Scan(&contentID, &status)
	if err != nil {
		return "", err
	}
	if status == "deleting" {
		return "", apperror.New(apperror.KindConflict, "content_cleanup_pending", "the previous content reference is still being deleted; retry shortly")
	}
	if status == "uploading" {
		var count int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(1) FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.owner_id=? AND f.status='active' AND c.status='uploading'`, owner).Scan(&count); err != nil {
			return "", err
		}
		if count >= maxActive {
			return "", apperror.New(apperror.KindTooManyRequests, "active_file_limit", fmt.Sprintf("at most %d active files are allowed", maxActive))
		}
	}
	fileID := uuid.NewString()
	if _, err = tx.ExecContext(ctx, `INSERT INTO files(id,content_id,owner_id,recipient_id,original_name) VALUES(?,?,?,?,?)`, fileID, contentID, owner, in.RecipientID, in.Name); err != nil {
		return "", err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=ref_count+1 WHERE id=?`, contentID); err != nil {
		return "", err
	}
	if err = tx.Commit(); err != nil {
		return "", err
	}
	return fileID, nil
}

func (r *sqlRepository) UploadedChunkIndexes(ctx context.Context, contentID string) ([]int, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT chunk_index FROM file_chunks WHERE content_id=? ORDER BY chunk_index`, contentID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	indexes := make([]int, 0)
	for rows.Next() {
		var index int
		if err := rows.Scan(&index); err != nil {
			return nil, err
		}
		indexes = append(indexes, index)
	}
	return indexes, rows.Err()
}

func (r *sqlRepository) StoredChunk(ctx context.Context, contentID string, index int) (int64, string, error) {
	var size int64
	var sha string
	err := r.db.QueryRowContext(ctx, `SELECT plaintext_size,sha256 FROM file_chunks WHERE content_id=? AND chunk_index=?`, contentID, index).Scan(&size, &sha)
	return size, sha, err
}

// StoreChunk 在事务内再次检查状态并替换分块元数据，返回提交前旧对象键。
func (r *sqlRepository) StoreChunk(ctx context.Context, id, contentID string, index int, key string, plain []byte) (string, error) {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	var contentStatus, fileStatus string
	err = tx.QueryRowContext(ctx, `SELECT c.status,f.status FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? AND f.content_id=? FOR UPDATE`, id, contentID).Scan(&contentStatus, &fileStatus)
	if err == sql.ErrNoRows {
		return "", apperror.New(apperror.KindNotFound, "file_not_found", "file not found")
	}
	if err != nil {
		return "", err
	}
	if contentStatus != "uploading" || fileStatus != "active" {
		return "", apperror.New(apperror.KindConflict, "invalid_file_state", "file state changed during chunk upload")
	}
	var oldKey string
	err = tx.QueryRowContext(ctx, `SELECT object_key FROM file_chunks WHERE content_id=? AND chunk_index=? FOR UPDATE`, contentID, index).Scan(&oldKey)
	if err != nil && err != sql.ErrNoRows {
		return "", err
	}
	if err == sql.ErrNoRows {
		_, err = tx.ExecContext(ctx, `INSERT INTO file_chunks(content_id,chunk_index,object_key,plaintext_size,sha256) VALUES(?,?,?,?,?)`, contentID, index, key, len(plain), SHA256(plain))
	} else {
		_, err = tx.ExecContext(ctx, `UPDATE file_chunks SET object_key=?,plaintext_size=?,sha256=?,created_at=NOW(6) WHERE content_id=? AND chunk_index=?`, key, len(plain), SHA256(plain), contentID, index)
	}
	if err != nil {
		return "", err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET revision=revision+1 WHERE id=? AND status='uploading'`, contentID); err != nil {
		return "", err
	}
	if err := tx.Commit(); err != nil {
		return "", err
	}
	return oldKey, nil
}

func (r *sqlRepository) Chunks(ctx context.Context, contentID string) ([]Chunk, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT chunk_index,object_key,plaintext_size,sha256 FROM file_chunks WHERE content_id=? ORDER BY chunk_index`, contentID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	chunks := make([]Chunk, 0)
	for rows.Next() {
		var chunk Chunk
		if err := rows.Scan(&chunk.Index, &chunk.ObjectKey, &chunk.PlaintextSize, &chunk.SHA256); err != nil {
			return nil, err
		}
		chunks = append(chunks, chunk)
	}
	return chunks, rows.Err()
}

func (r *sqlRepository) MarkReady(ctx context.Context, contentID string, revision int64) (bool, error) {
	result, err := r.db.ExecContext(ctx, `UPDATE file_contents SET status='ready',completed_at=NOW(6) WHERE id=? AND status='uploading' AND revision=?`, contentID, revision)
	if err != nil {
		return false, err
	}
	count, _ := result.RowsAffected()
	return count == 1, nil
}

// BeginDeletion 先提交 deleting 屏障；对象清理由调用方在事务外执行。
func (r *sqlRepository) BeginDeletion(ctx context.Context, id, expectedStatus string, cutoff time.Time) (string, bool, error) {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return "", false, err
	}
	defer tx.Rollback()
	var contentID, fileStatus, contentStatus string
	var refs int
	var updatedAt time.Time
	err = tx.QueryRowContext(ctx, `SELECT f.content_id,f.status,c.status,c.ref_count,c.updated_at FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`, id).Scan(&contentID, &fileStatus, &contentStatus, &refs, &updatedAt)
	if err == sql.ErrNoRows {
		return "", false, apperror.New(apperror.KindNotFound, "file_not_found", "file not found")
	}
	if err != nil {
		return "", false, err
	}
	if expectedStatus != "" {
		eligible := contentStatus == expectedStatus && !updatedAt.After(cutoff)
		if expectedStatus == "uploading" {
			eligible = eligible && fileStatus == "active"
		} else if expectedStatus == "deleting" {
			eligible = eligible && fileStatus == "deleting" && refs == 0
		} else {
			eligible = false
		}
		if !eligible {
			return "", false, errCleanupSkipped
		}
	}
	last := fileStatus == "deleting" || (refs == 1 && fileStatus == "active")
	if fileStatus == "active" {
		if refs < 1 {
			return "", false, fmt.Errorf("invalid content reference count")
		}
		if last {
			if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`, contentID); err != nil {
				return "", false, err
			}
			if _, err = tx.ExecContext(ctx, `UPDATE files SET status='deleting' WHERE id=? AND status='active'`, id); err != nil {
				return "", false, err
			}
		} else {
			if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=ref_count-1,updated_at=updated_at WHERE id=? AND ref_count>1`, contentID); err != nil {
				return "", false, err
			}
			if _, err = tx.ExecContext(ctx, `DELETE FROM files WHERE id=? AND status='active'`, id); err != nil {
				return "", false, err
			}
		}
	} else if fileStatus != "deleting" || contentStatus != "deleting" || refs != 0 {
		return "", false, fmt.Errorf("invalid deleting file state")
	}
	if err = tx.Commit(); err != nil {
		return "", false, err
	}
	return contentID, last, nil
}

// FinishDeletion 仅在对象全部删除后完成第二个元数据事务。
func (r *sqlRepository) FinishDeletion(ctx context.Context, id, contentID string) error {
	tx, err := r.db.BeginTx(ctx, nil)
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
	if count, _ := result.RowsAffected(); count != 1 {
		return sql.ErrNoRows
	}
	return tx.Commit()
}

func (r *sqlRepository) CleanupCandidates(ctx context.Context, uploadCutoff, deletingCutoff time.Time, limit int) ([]cleanupCandidate, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT f.id,f.owner_id,c.status FROM files f JOIN file_contents c ON c.id=f.content_id WHERE (c.status='uploading' AND f.status='active' AND c.updated_at<=?) OR (c.status='deleting' AND f.status='deleting' AND c.ref_count=0 AND c.updated_at<=?) ORDER BY c.updated_at,f.id LIMIT ?`, uploadCutoff, deletingCutoff, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	candidates := make([]cleanupCandidate, 0)
	for rows.Next() {
		var candidate cleanupCandidate
		if err := rows.Scan(&candidate.id, &candidate.ownerID, &candidate.status); err != nil {
			return nil, err
		}
		candidates = append(candidates, candidate)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return candidates, nil
}
