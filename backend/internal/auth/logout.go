package auth

import (
	"context"
)

func (s *Service) Logout(ctx context.Context, userID, raw string) error {
	if len(raw) < 32 || len(raw) > 256 {
		return nil
	}
	_, err := s.db.ExecContext(ctx, `UPDATE refresh_tokens SET revoked_at=NOW(6) WHERE token_hash=? AND user_id=? AND revoked_at IS NULL`, HashRefreshToken(raw), userID)
	return err
}
