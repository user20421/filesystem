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
type Service struct {
	db                *sql.DB
	objects           ObjectStore
	kek               []byte
	maxActive         int
	maxChunk, maxFile int64
	sem               chan struct{}
	memory            *memoryBudget
	lockMu            sync.Mutex
	locks             map[string]*lockEntry
}

func NewService(db *sql.DB, objects ObjectStore, kek []byte, maxActive int, maxChunk, maxFile int64, concurrency int) *Service {
	return NewServiceWithMemoryBudget(db, objects, kek, maxActive, maxChunk, maxFile, concurrency, 500<<20)
}

func NewServiceWithMemoryBudget(db *sql.DB, objects ObjectStore, kek []byte, maxActive int, maxChunk, maxFile int64, concurrency int, fileMemoryBudget int64) *Service {
	return &Service{db: db, objects: objects, kek: kek, maxActive: maxActive, maxChunk: maxChunk, maxFile: maxFile, sem: make(chan struct{}, concurrency), memory: newMemoryBudget(fileMemoryBudget), locks: make(map[string]*lockEntry)}
}

func (s *Service) MemoryStats() (used, limit int64) {
	return s.memory.stats()
}
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
func (s *Service) acquire(ctx context.Context) error {
	select {
	case s.sem <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
func (s *Service) release() { <-s.sem }

type InitInput struct {
	Name        string
	Size        int64
	SHA256      string
	RecipientID string
	ChunkSize   int64
}

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
		in.ChunkSize = min(int64(1<<20), s.maxChunk)
	}
	if in.ChunkSize < 64<<10 || in.ChunkSize > s.maxChunk {
		return File{}, httpx.Error(400, "invalid_chunk_size", fmt.Sprintf("chunk_size must be between 64KiB and %d bytes", s.maxChunk))
	}
	unlock := s.lock("init:" + owner)
	defer unlock()
	var count int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(1) FROM files WHERE owner_id=? AND status='uploading'`, owner).Scan(&count); err != nil {
		return File{}, err
	}
	if count >= s.maxActive {
		return File{}, httpx.Error(429, "active_file_limit", fmt.Sprintf("at most %d active files are allowed", s.maxActive))
	}
	var exists int
	if err := s.db.QueryRowContext(ctx, `SELECT COUNT(1) FROM users WHERE id=?`, in.RecipientID).Scan(&exists); err != nil {
		return File{}, err
	}
	if exists != 1 {
		return File{}, httpx.Error(404, "recipient_not_found", "recipient not found")
	}
	dek, wrapped, nonce, err := GenerateWrappedDEK(s.kek)
	if err != nil {
		return File{}, err
	}
	for i := range dek {
		dek[i] = 0
	}
	f := File{ID: uuid.NewString(), OwnerID: owner, RecipientID: in.RecipientID, OriginalName: in.Name, Size: in.Size, SHA256: strings.ToLower(in.SHA256), ChunkSize: in.ChunkSize, ChunkCount: int((in.Size + in.ChunkSize - 1) / in.ChunkSize), WrappedDEK: wrapped, DEKNonce: nonce, Status: "uploading"}
	_, err = s.db.ExecContext(ctx, `INSERT INTO files(id,owner_id,recipient_id,original_name,size,sha256,chunk_size,chunk_count,wrapped_dek,dek_nonce,status) VALUES(?,?,?,?,?,?,?,?,?,?,?)`, f.ID, f.OwnerID, f.RecipientID, f.OriginalName, f.Size, f.SHA256, f.ChunkSize, f.ChunkCount, f.WrappedDEK, f.DEKNonce, f.Status)
	if err != nil {
		return File{}, err
	}
	return s.getFile(ctx, f.ID)
}
func (s *Service) Status(ctx context.Context, id, user string) (File, []int, error) {
	f, err := s.getFile(ctx, id)
	if err != nil {
		return f, nil, err
	}
	if err := Authorize(f, user, false); err != nil {
		return f, nil, err
	}
	rows, err := s.db.QueryContext(ctx, `SELECT chunk_index FROM file_chunks WHERE file_id=? ORDER BY chunk_index`, id)
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
