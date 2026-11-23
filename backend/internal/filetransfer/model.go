package filetransfer

import (
	"context"
	"database/sql"
	"errors"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

// File 是独立的逻辑文件引用，包含参与者与显示状态；底层内容通过 ContentID 与其他引用共享。
type File struct {
	ID           string     `json:"id"`
	ContentID    string     `json:"content_id"`
	OwnerID      string     `json:"owner_id"`
	RecipientID  string     `json:"recipient_id"`
	OriginalName string     `json:"original_name"`
	Size         int64      `json:"size"`
	SHA256       string     `json:"sha256"`
	ChunkSize    int64      `json:"chunk_size"`
	ChunkCount   int        `json:"chunk_count"`
	Status       string     `json:"status"`
	Instant      bool       `json:"instant"`
	Revision     int64      `json:"-"`
	CreatedAt    time.Time  `json:"created_at"`
	CompletedAt  *time.Time `json:"completed_at,omitempty"`
}

// Chunk 描述共享内容的一个分块及其对象位置、明文大小和 SHA-256。
type Chunk struct {
	Index         int    `json:"index"`
	ObjectKey     string `json:"-"`
	PlaintextSize int64  `json:"size"`
	SHA256        string `json:"sha256"`
}

var shaPattern = regexp.MustCompile(`^[a-fA-F0-9]{64}$`)

// Authorize 校验文件参与者权限；ownerOnly=true 仅允许上传者，false 允许上传者或接收者。
func Authorize(file File, userID string, ownerOnly bool) error {
	if ownerOnly && file.OwnerID == userID {
		return nil
	}
	if !ownerOnly && (file.OwnerID == userID || file.RecipientID == userID) {
		return nil
	}
	return apperror.New(apperror.KindForbidden, "file_forbidden", "you are not a participant in this file transfer")
}

// expectedChunkSize 计算指定分块的字节数，最后一块按文件剩余大小处理，拒绝越界序号。
func expectedChunkSize(file File, index int) (int64, error) {
	if index < 0 || index >= file.ChunkCount {
		return 0, apperror.New(apperror.KindBadRequest, "invalid_chunk_index", "chunk index is out of range")
	}
	if index < file.ChunkCount-1 {
		return file.ChunkSize, nil
	}
	return file.Size - int64(index)*file.ChunkSize, nil
}
func validateName(name string) bool {
	return utf8.ValidString(name) && len(name) > 0 && len(name) <= 255 && name == strings.TrimSpace(name) && name != "." && name != ".." && !strings.ContainsAny(name, "/\\\x00\r\n")
}

const selectFile = `SELECT f.id,f.content_id,f.owner_id,f.recipient_id,f.original_name,c.size,c.sha256,c.chunk_size,c.chunk_count,c.status,c.revision,f.created_at,c.completed_at FROM files f JOIN file_contents c ON c.id=f.content_id `

// scanFile 将数据库 ready 映射为 API 的 completed，其他内部状态原样保留。
func scanFile(scanner interface{ Scan(...any) error }) (File, error) {
	var f File
	var completed sql.NullTime
	err := scanner.Scan(&f.ID, &f.ContentID, &f.OwnerID, &f.RecipientID, &f.OriginalName, &f.Size, &f.SHA256, &f.ChunkSize, &f.ChunkCount, &f.Status, &f.Revision, &f.CreatedAt, &completed)
	if f.Status == "ready" {
		f.Status = "completed"
	}
	if completed.Valid {
		t := completed.Time
		f.CompletedAt = &t
	}
	return f, err
}
func (s *Service) getFile(ctx context.Context, id string) (File, error) {
	if _, err := uuid.Parse(id); err != nil {
		return File{}, apperror.New(apperror.KindNotFound, "file_not_found", "file not found")
	}
	f, err := s.repo.File(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return File{}, apperror.New(apperror.KindNotFound, "file_not_found", "file not found")
	}
	return f, err
}
