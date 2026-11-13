package auth

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"time"

	"github.com/google/uuid"
	"secure-im/backend/internal/httpx"
)

func (s *Service) Refresh(ctx context.Context, raw string) (Result, error) {
	if len(raw) < 32 || len(raw) > 256 {
		return Result{}, invalidRefresh()
	}
	refresh, hash, expires, err := s.tokens.RefreshToken()
	if err != nil {
		return Result{}, err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Result{}, err
	}
	defer tx.Rollback()
	var tokenID string
	var user User
	var expiresAt time.Time
	var rotated, revoked sql.NullTime
	err = tx.QueryRowContext(ctx, `SELECT r.id,r.expires_at,r.rotated_at,r.revoked_at,u.id,u.username,u.display_name,u.created_at FROM refresh_tokens r JOIN users u ON u.id=r.user_id WHERE r.token_hash=? FOR UPDATE`, HashRefreshToken(raw)).Scan(&tokenID, &expiresAt, &rotated, &revoked, &user.ID, &user.Username, &user.DisplayName, &user.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Result{}, invalidRefresh()
	}
	if err != nil {
		return Result{}, err
	}
	user.Username = strings.ToUpper(user.Username)
	if rotated.Valid || revoked.Valid || !expiresAt.After(time.Now()) {
		return Result{}, invalidRefresh()
	}
	result, err := tx.ExecContext(ctx, `UPDATE refresh_tokens SET rotated_at=NOW(6) WHERE id=? AND rotated_at IS NULL AND revoked_at IS NULL`, tokenID)
	if err != nil {
		return Result{}, err
	}
	if count, _ := result.RowsAffected(); count != 1 {
		return Result{}, invalidRefresh()
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO refresh_tokens(id,user_id,token_hash,expires_at) VALUES(?,?,?,?)`, uuid.NewString(), user.ID, hash, expires); err != nil {
		return Result{}, err
	}
	if err := tx.Commit(); err != nil {
		return Result{}, err
	}
	access, err := s.tokens.AccessToken(user.ID)
	if err != nil {
		return Result{}, err
	}
	return Result{User: user, Tokens: Tokens{AccessToken: access, RefreshToken: refresh, TokenType: "Bearer", ExpiresIn: int64(s.tokens.accessTTL.Seconds())}}, nil
}
func invalidRefresh() error {
	return httpx.Error(401, "invalid_refresh_token", "refresh token is invalid or expired")
}
func (s *Service) Me(ctx context.Context, id string) (User, error) {
	var user User
	err := s.db.QueryRowContext(ctx, `SELECT id,username,display_name,created_at FROM users WHERE id=?`, id).Scan(&user.ID, &user.Username, &user.DisplayName, &user.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, httpx.Error(401, "invalid_token", "user no longer exists")
	}
	user.Username = strings.ToUpper(user.Username)
	return user, err
}
func (s *Service) ListUsers(ctx context.Context) ([]User, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id,username,display_name,created_at FROM users ORDER BY username LIMIT 500`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	users := make([]User, 0)
	for rows.Next() {
		var u User
		if err := rows.Scan(&u.ID, &u.Username, &u.DisplayName, &u.CreatedAt); err != nil {
			return nil, err
		}
		u.Username = strings.ToUpper(u.Username)
		users = append(users, u)
	}
	return users, rows.Err()
}
