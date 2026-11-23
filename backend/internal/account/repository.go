package account

import (
	"context"
	"database/sql"
)

// repository 仅覆盖账号注销需要的文件枚举和元数据删除。
type repository interface {
	FileIDs(context.Context, string) ([]string, error)
	DeleteUser(context.Context, string) error
}

// sqlRepository 保持账号注销的 SQL 顺序和单事务提交边界。
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
