package account

import (
	"context"
	"database/sql"

	"secure-im/backend/internal/httpx"
)

type fileDeleter interface {
	DeleteForAccount(context.Context, string, string) error
}

type repository interface {
	FileIDs(context.Context, string) ([]string, error)
	DeleteUser(context.Context, string) error
}

type Service struct {
	repo          repository
	files         fileDeleter
	closeUser     func(string)
	revokeTickets func(string)
}

func NewService(db *sql.DB, files fileDeleter, closeUser, revokeTickets func(string)) *Service {
	return &Service{repo: &sqlRepository{db: db}, files: files, closeUser: closeUser, revokeTickets: revokeTickets}
}

// Delete removes external objects before deleting metadata. If any step fails,
// the user row remains and the same request can safely resume the deletion.
// Delete 按“先外部文件、后用户元数据”的顺序注销账号。
// 任何一步失败都保留用户以便重试；成功后才关闭实时连接、撤销票据并删除凭据。
func (s *Service) Delete(ctx context.Context, userID string) error {
	ids, err := s.repo.FileIDs(ctx, userID)
	if err != nil {
		return retryable()
	}
	for _, id := range ids {
		if err := s.files.DeleteForAccount(ctx, id, userID); err != nil {
			return retryable()
		}
	}
	if err := s.repo.DeleteUser(ctx, userID); err != nil {
		return retryable()
	}
	if s.closeUser != nil {
		s.closeUser(userID)
	}
	if s.revokeTickets != nil {
		s.revokeTickets(userID)
	}
	return nil
}

func retryable() error {
	return httpx.Error(503, "account_deletion_failed", "account deletion could not be completed; please retry")
}

type sqlRepository struct{ db *sql.DB }

func (r *sqlRepository) FileIDs(ctx context.Context, userID string) ([]string, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT id FROM files WHERE owner_id=? OR recipient_id=? ORDER BY id`, userID, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	ids := make([]string, 0)
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

func (r *sqlRepository) DeleteUser(ctx context.Context, userID string) error {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `DELETE FROM messages WHERE sender_id=? OR recipient_id=? OR conversation_id IN (SELECT id FROM conversations WHERE user_low_id=? OR user_high_id=?)`, userID, userID, userID, userID); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM conversations WHERE user_low_id=? OR user_high_id=?`, userID, userID); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM refresh_tokens WHERE user_id=?`, userID); err != nil {
		return err
	}
	result, err := tx.ExecContext(ctx, `DELETE FROM users WHERE id=?`, userID)
	if err != nil {
		return err
	}
	if count, _ := result.RowsAffected(); count != 1 {
		return sql.ErrNoRows
	}
	return tx.Commit()
}
