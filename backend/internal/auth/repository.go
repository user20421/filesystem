package auth

import (
	"context"
	"database/sql"
	"time"
)

// repository 是认证与用户资料所需的窄持久化接口，不泄露给 HTTP 层。
type repository interface {
	CreateUser(context.Context, User, string) error
	LoginUser(context.Context, string) (User, string, error)
	CreateRefreshToken(context.Context, string, string, string, time.Time) error
	RotateRefreshToken(context.Context, string, string, string, time.Time) (User, error)
	RevokeRefreshToken(context.Context, string, string) error
	UserByID(context.Context, string) (User, error)
	ListUsers(context.Context) ([]User, error)
}

// sqlRepository 原样封装认证 SQL、行锁和刷新令牌事务。
type sqlRepository struct{ db *sql.DB }

func (r *sqlRepository) CreateUser(ctx context.Context, user User, hash string) error {
	_, err := r.db.ExecContext(ctx, `INSERT INTO users(id,username,display_name,password_hash,created_at) VALUES(?,?,?,?,?)`, user.ID, user.Username, user.DisplayName, hash, user.CreatedAt)
	return err
}

func (r *sqlRepository) LoginUser(ctx context.Context, username string) (User, string, error) {
	var user User
	var hash string
	err := r.db.QueryRowContext(ctx, `SELECT id,username,display_name,password_hash,created_at FROM users WHERE username=?`, username).Scan(&user.ID, &user.Username, &user.DisplayName, &hash, &user.CreatedAt)
	return user, hash, err
}

func (r *sqlRepository) CreateRefreshToken(ctx context.Context, id, userID, hash string, expires time.Time) error {
	_, err := r.db.ExecContext(ctx, `INSERT INTO refresh_tokens(id,user_id,token_hash,expires_at) VALUES(?,?,?,?)`, id, userID, hash, expires)
	return err
}

// RotateRefreshToken 在单个事务中锁定并消费旧令牌，再写入新令牌。
func (r *sqlRepository) RotateRefreshToken(ctx context.Context, oldHash, newID, newHash string, newExpires time.Time) (User, error) {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return User{}, err
	}
	defer tx.Rollback()
	var tokenID string
	var user User
	var expiresAt time.Time
	var rotated, revoked sql.NullTime
	err = tx.QueryRowContext(ctx, `SELECT r.id,r.expires_at,r.rotated_at,r.revoked_at,u.id,u.username,u.display_name,u.created_at FROM refresh_tokens r JOIN users u ON u.id=r.user_id WHERE r.token_hash=? FOR UPDATE`, oldHash).Scan(&tokenID, &expiresAt, &rotated, &revoked, &user.ID, &user.Username, &user.DisplayName, &user.CreatedAt)
	if err != nil {
		return User{}, err
	}
	if rotated.Valid || revoked.Valid || !expiresAt.After(time.Now()) {
		return User{}, sql.ErrNoRows
	}
	result, err := tx.ExecContext(ctx, `UPDATE refresh_tokens SET rotated_at=NOW(6) WHERE id=? AND rotated_at IS NULL AND revoked_at IS NULL`, tokenID)
	if err != nil {
		return User{}, err
	}
	if count, _ := result.RowsAffected(); count != 1 {
		return User{}, sql.ErrNoRows
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO refresh_tokens(id,user_id,token_hash,expires_at) VALUES(?,?,?,?)`, newID, user.ID, newHash, newExpires); err != nil {
		return User{}, err
	}
	if err := tx.Commit(); err != nil {
		return User{}, err
	}
	return user, nil
}

func (r *sqlRepository) RevokeRefreshToken(ctx context.Context, hash, userID string) error {
	_, err := r.db.ExecContext(ctx, `UPDATE refresh_tokens SET revoked_at=NOW(6) WHERE token_hash=? AND user_id=? AND revoked_at IS NULL`, hash, userID)
	return err
}

func (r *sqlRepository) UserByID(ctx context.Context, id string) (User, error) {
	var user User
	err := r.db.QueryRowContext(ctx, `SELECT id,username,display_name,created_at FROM users WHERE id=?`, id).Scan(&user.ID, &user.Username, &user.DisplayName, &user.CreatedAt)
	return user, err
}

func (r *sqlRepository) ListUsers(ctx context.Context) ([]User, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT id,username,display_name,created_at FROM users ORDER BY username LIMIT 500`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	users := make([]User, 0)
	for rows.Next() {
		var user User
		if err := rows.Scan(&user.ID, &user.Username, &user.DisplayName, &user.CreatedAt); err != nil {
			return nil, err
		}
		users = append(users, user)
	}
	return users, rows.Err()
}
