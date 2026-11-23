package auth

import "context"

func (s *Service) Logout(ctx context.Context, userID, raw string) error {
	if len(raw) < 32 || len(raw) > 256 {
		return nil
	}
	return s.repo.RevokeRefreshToken(ctx, HashRefreshToken(raw), userID)
}
