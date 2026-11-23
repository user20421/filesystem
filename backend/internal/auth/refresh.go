package auth

import (
	"context"
	"database/sql"
	"errors"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

// Refresh 原子轮换刷新令牌，拒绝已撤销、已轮换或过期的令牌。
// 行锁与条件更新保证同一旧令牌在并发刷新中只能被消费一次。
func (s *Service) Refresh(ctx context.Context, raw string) (Result, error) {
	if len(raw) < 32 || len(raw) > 256 {
		return Result{}, invalidRefresh()
	}
	refresh, hash, expires, err := s.tokens.RefreshToken()
	if err != nil {
		return Result{}, err
	}
	user, err := s.repo.RotateRefreshToken(ctx, HashRefreshToken(raw), uuid.NewString(), hash, expires)
	if errors.Is(err, sql.ErrNoRows) {
		return Result{}, invalidRefresh()
	}
	if err != nil {
		return Result{}, err
	}
	user.Username = normalizeUsername(user.Username)
	access, err := s.tokens.AccessToken(user.ID)
	if err != nil {
		return Result{}, err
	}
	return Result{User: user, Tokens: Tokens{AccessToken: access, RefreshToken: refresh, TokenType: "Bearer", ExpiresIn: int64(s.tokens.accessTTL.Seconds())}}, nil
}

func invalidRefresh() error {
	return apperror.New(apperror.KindUnauthorized, "invalid_refresh_token", "refresh token is invalid or expired")
}

func (s *Service) Me(ctx context.Context, id string) (User, error) {
	user, err := s.repo.UserByID(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, apperror.New(apperror.KindUnauthorized, "invalid_token", "user no longer exists")
	}
	user.Username = normalizeUsername(user.Username)
	return user, err
}

func (s *Service) ListUsers(ctx context.Context) ([]User, error) {
	users, err := s.repo.ListUsers(ctx)
	if err != nil {
		return nil, err
	}
	for i := range users {
		users[i].Username = normalizeUsername(users[i].Username)
	}
	return users, nil
}
