package account

import (
	"context"
	"database/sql"

	"secure-im/backend/internal/apperror"
)

type fileDeleter interface {
	DeleteForAccount(context.Context, string, string) error
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
	return apperror.New(apperror.KindServiceUnavailable, "account_deletion_failed", "account deletion could not be completed; please retry")
}
