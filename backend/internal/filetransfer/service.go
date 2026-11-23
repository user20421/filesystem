package filetransfer

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"sync"

	"github.com/google/uuid"
	"secure-im/backend/internal/httpx"
)

type lockEntry struct {
	mu   sync.Mutex
	refs int
}

// Service 协调文件元数据、MinIO 对象、分块调度与内存预算，所有状态仅在单进程内统一管理。
type Service struct {
	db                *sql.DB
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
	return &Service{db: db, objects: objects, maxActive: maxActive, maxChunk: maxChunk, maxFile: maxFile, scheduler: newChunkScheduler(concurrency), memory: newMemoryBudget(fileMemoryBudget), locks: make(map[string]*lockEntry)}
}

func (s *Service) MemoryStats() (used, limit int64) {
	return s.memory.stats()
}
func (s *Service) ChunkSlotStats() (used, limit int) {
	return s.scheduler.Stats()
}

// lock 提供带引用计数的键锁，最后一个使用者释放后才删除键，返回的函数必须调用以解除锁。
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

// Init 创建逻辑文件引用并复用已有内容：(size,sha256) 是内容身份，引用增加和文件创建必须在同一事务完成。
// 只有 ready 内容才直接秒传；提交后重新读取最终状态，因为其他引用可能恰好完成内容。
func (s *Service) Init(ctx context.Context, owner string, in InitInput) (File, error) {
	if !validateName(in.Name) {
		return File{}, httpx.Error(400, "invalid_filename", "filename must be a clean 1-255 byte base name")
	}
	if in.Size < 1 || in.Size > s.maxFile {
		return File{}, httpx.Error(400, "invalid_file_size", fmt.Sprintf("file size must be between 1 and %d bytes", s.maxFile))
	}
	if !shaPattern.MatchString(in.SHA256) {
		return File{}, httpx.Error(400, "invalid_sha256", "SHA256 must contain 64 hexadecimal characters")
	}
	if _, err := uuid.Parse(in.RecipientID); err != nil || in.RecipientID == owner {
		return File{}, httpx.Error(400, "invalid_recipient", "recipient must be another user ID")
	}
	if in.ChunkSize == 0 {
		in.ChunkSize = min(int64(5<<20), s.maxChunk)
	}
	if in.ChunkSize < 64<<10 || in.ChunkSize > s.maxChunk {
		return File{}, httpx.Error(400, "invalid_chunk_size", fmt.Sprintf("chunk_size must be between 64KiB and %d bytes", s.maxChunk))
	}
	var exists int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(1) FROM users WHERE id=?`, in.RecipientID).Scan(&exists); err != nil {
		return File{}, err
	}
	if exists != 1 {
		return File{}, httpx.Error(404, "recipient_not_found", "recipient not found")
	}
	unlock := s.lock("init:" + owner)
	defer unlock()

	in.SHA256 = strings.ToLower(in.SHA256)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return File{}, err
	}
	defer tx.Rollback()
	// The insert and row lock serialize creators of the same content identity.
	// Keep that lock through reference insertion so instant reuse and ref_count
	// are committed from one consistent content state.
	contentID := uuid.NewString()
	_, err = tx.ExecContext(ctx, `INSERT INTO file_contents(id,size,sha256,chunk_size,chunk_count) VALUES(?,?,?,?,?) ON DUPLICATE KEY UPDATE id=id`, contentID, in.Size, in.SHA256, in.ChunkSize, int((in.Size+in.ChunkSize-1)/in.ChunkSize))
	if err != nil {
		return File{}, err
	}
	var status string
	err = tx.QueryRowContext(ctx, `SELECT id,status FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`, in.Size, in.SHA256).Scan(&contentID, &status)
	if err != nil {
		return File{}, err
	}
	if status == "deleting" {
		return File{}, httpx.Error(409, "content_cleanup_pending", "the previous content reference is still being deleted; retry shortly")
	}
	if status == "uploading" {
		var count int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(1) FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.owner_id=? AND f.status='active' AND c.status='uploading'`, owner).Scan(&count); err != nil {
			return File{}, err
		}
		if count >= s.maxActive {
			return File{}, httpx.Error(429, "active_file_limit", fmt.Sprintf("at most %d active files are allowed", s.maxActive))
		}
	}
	fileID := uuid.NewString()
	if _, err = tx.ExecContext(ctx, `INSERT INTO files(id,content_id,owner_id,recipient_id,original_name) VALUES(?,?,?,?,?)`, fileID, contentID, owner, in.RecipientID, in.Name); err != nil {
		return File{}, err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE file_contents SET ref_count=ref_count+1 WHERE id=?`, contentID); err != nil {
		return File{}, err
	}
	if err = tx.Commit(); err != nil {
		return File{}, err
	}
	f, err := s.getFile(ctx, fileID)
	if err != nil {
		return f, err
	}
	// 秒传标记必须取提交后的最终状态：同一事务只新增逻辑引用，只有 ready 内容才能跳过分块上传。
	// Derive instant from the final committed file state, not the earlier lock probe.
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
	rows, err := s.db.QueryContext(ctx, `SELECT chunk_index FROM file_chunks WHERE content_id=? ORDER BY chunk_index`, f.ContentID)
	if err != nil {
		return f, nil, err
	}
	defer rows.Close()
	indexes := make([]int, 0)
	for rows.Next() {
		var index int
		if err := rows.Scan(&index); err != nil {
			return f, nil, err
		}
		indexes = append(indexes, index)
	}
	return f, indexes, rows.Err()
}
