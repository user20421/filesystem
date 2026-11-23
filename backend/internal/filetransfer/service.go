package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"sync"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

type lockEntry struct {
	mu   sync.Mutex
	refs int
}

// Service 协调文件元数据、对象、分块调度与内存预算，不负责具体 SQL。
type Service struct {
	repo              repository
	objects           ObjectStore
	maxActive         int
	maxChunk, maxFile int64
	scheduler         *chunkScheduler
	memory            *memoryBudget
	lockMu            sync.Mutex
	locks             map[string]*lockEntry
}

func NewService(db *sql.DB, objects ObjectStore, maxActive int, maxChunk, maxFile int64, concurrency int) *Service {
	return NewServiceWithMemoryBudget(db, objects, maxActive, maxChunk, maxFile, concurrency, 500<<20)
}

func NewServiceWithMemoryBudget(db *sql.DB, objects ObjectStore, maxActive int, maxChunk, maxFile int64, concurrency int, fileMemoryBudget int64) *Service {
	return &Service{repo: &sqlRepository{db: db}, objects: objects, maxActive: maxActive, maxChunk: maxChunk, maxFile: maxFile, scheduler: newChunkScheduler(concurrency), memory: newMemoryBudget(fileMemoryBudget), locks: make(map[string]*lockEntry)}
}

func (s *Service) MemoryStats() (used, limit int64)  { return s.memory.stats() }
func (s *Service) ChunkSlotStats() (used, limit int) { return s.scheduler.Stats() }

// lock 提供带引用计数的键锁，最后一个使用者释放后才删除键。
func (s *Service) lock(key string) func() {
	s.lockMu.Lock()
	entry := s.locks[key]
	if entry == nil {
		entry = &lockEntry{}
		s.locks[key] = entry
	}
	entry.refs++
	s.lockMu.Unlock()
	entry.mu.Lock()
	return func() {
		entry.mu.Unlock()
		s.lockMu.Lock()
		entry.refs--
		if entry.refs == 0 {
			delete(s.locks, key)
		}
		s.lockMu.Unlock()
	}
}

type InitInput struct {
	Name        string
	Size        int64
	SHA256      string
	RecipientID string
	ChunkSize   int64
}

// Init 创建逻辑文件引用并复用已有 ready 内容，最终秒传状态在提交后重新读取。
func (s *Service) Init(ctx context.Context, owner string, in InitInput) (File, error) {
	if !validateName(in.Name) {
		return File{}, apperror.New(apperror.KindBadRequest, "invalid_filename", "filename must be a clean 1-255 byte base name")
	}
	if in.Size < 1 || in.Size > s.maxFile {
		return File{}, apperror.New(apperror.KindBadRequest, "invalid_file_size", fmt.Sprintf("file size must be between 1 and %d bytes", s.maxFile))
	}
	if !shaPattern.MatchString(in.SHA256) {
		return File{}, apperror.New(apperror.KindBadRequest, "invalid_sha256", "SHA256 must contain 64 hexadecimal characters")
	}
	if _, err := uuid.Parse(in.RecipientID); err != nil || in.RecipientID == owner {
		return File{}, apperror.New(apperror.KindBadRequest, "invalid_recipient", "recipient must be another user ID")
	}
	if in.ChunkSize == 0 {
		in.ChunkSize = min(int64(5<<20), s.maxChunk)
	}
	if in.ChunkSize < 64<<10 || in.ChunkSize > s.maxChunk {
		return File{}, apperror.New(apperror.KindBadRequest, "invalid_chunk_size", fmt.Sprintf("chunk_size must be between 64KiB and %d bytes", s.maxChunk))
	}
	exists, err := s.repo.RecipientExists(ctx, in.RecipientID)
	if err != nil {
		return File{}, err
	}
	if !exists {
		return File{}, apperror.New(apperror.KindNotFound, "recipient_not_found", "recipient not found")
	}
	unlock := s.lock("init:" + owner)
	defer unlock()
	in.SHA256 = strings.ToLower(in.SHA256)
	fileID, err := s.repo.CreateFile(ctx, owner, in, s.maxActive)
	if err != nil {
		return File{}, err
	}
	f, err := s.getFile(ctx, fileID)
	if err != nil {
		return f, err
	}
	f.Instant = f.Status == "completed"
	return f, nil
}

func (s *Service) Status(ctx context.Context, id, user string) (File, []int, error) {
	f, err := s.getFile(ctx, id)
	if err != nil {
		return f, nil, err
	}
	if err := Authorize(f, user, false); err != nil {
		return f, nil, err
	}
	indexes, err := s.repo.UploadedChunkIndexes(ctx, f.ContentID)
	return f, indexes, err
}
