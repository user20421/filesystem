package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"time"
)

func (s *Service) Delete(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, true, "", time.Time{})
}

// DeleteForAccount 删除任一方参与者的逻辑引用，共享内容直到最后引用才清理对象。
func (s *Service) DeleteForAccount(ctx context.Context, id, user string) error {
	return s.delete(ctx, id, user, false, "", time.Time{})
}

// delete 保持“屏障事务→事务外对象清理→元数据完成事务”的两阶段删除协议。
func (s *Service) delete(ctx context.Context, id, user string, ownerOnly bool, expectedStatus string, cutoff time.Time) error {
	unlockFile := s.lock("file:" + id)
	defer unlockFile()
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, ownerOnly); err != nil {
		return err
	}
	unlockContent := s.lock("content:" + f.ContentID)
	defer unlockContent()

	contentID, last, err := s.repo.BeginDeletion(ctx, id, expectedStatus, cutoff)
	if err != nil {
		return err
	}
	if !last {
		return nil
	}
	prefix := "contents/" + contentID + "/"
	keys, err := s.objects.List(ctx, prefix)
	if err != nil {
		return fmt.Errorf("list plaintext objects: %w", err)
	}
	for _, key := range keys {
		if err := s.objects.Remove(ctx, key); err != nil {
			return fmt.Errorf("remove plaintext object: %w", err)
		}
	}
	return s.repo.FinishDeletion(ctx, id, contentID)
}

// CleanupCandidates 将过期候选查询显式暴露给 Sweeper，避免清理器穿透数据库。
func (s *Service) CleanupCandidates(ctx context.Context, uploadCutoff, deletingCutoff time.Time, limit int) ([]cleanupCandidate, error) {
	return s.repo.CleanupCandidates(ctx, uploadCutoff, deletingCutoff, limit)
}

// DeleteCleanupCandidate 使用候选状态和时间截点重新校验后执行删除。
func (s *Service) DeleteCleanupCandidate(ctx context.Context, id, ownerID, status string, cutoff time.Time) error {
	return s.delete(ctx, id, ownerID, true, status, cutoff)
}

// DeletionService 是压力测试清理使用的显式仅删除入口，不初始化上传调度或内存预算。
type DeletionService struct{ files *Service }

func NewDeletionService(db *sql.DB, objects ObjectStore) *DeletionService {
	return &DeletionService{files: &Service{repo: &sqlRepository{db: db}, objects: objects, locks: make(map[string]*lockEntry)}}
}

func (s *DeletionService) DeleteForAccount(ctx context.Context, id, user string) error {
	return s.files.DeleteForAccount(ctx, id, user)
}
