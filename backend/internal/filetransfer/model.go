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
	"secure-im/backend/internal/httpx"
)

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
type Chunk struct {
	Index         int    `json:"index"`
	ObjectKey     string `json:"-"`
	PlaintextSize int64  `json:"size"`
	SHA256        string `json:"sha256"`
}

var shaPattern = regexp.MustCompile(`^[a-fA-F0-9]{64}$`)

func Authorize(file File, userID string, ownerOnly bool) error {
	if ownerOnly && file.OwnerID == userID {
		return nil
	}
	if !ownerOnly && (file.OwnerID == userID || file.RecipientID == userID) {
		return nil
	}
	return httpx.Error(403, "file_forbidden", "you are not a participant in this file transfer")
}
func expectedChunkSize(file File, index int) (int64, error) {
	if index < 0 || index >= file.ChunkCount {
		return 0, httpx.Error(400, "invalid_chunk_index", "chunk index is out of range")
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
		return File{}, httpx.Error(404, "file_not_found", "file not found")
	}
	f, err := scanFile(s.db.QueryRowContext(ctx, selectFile+`WHERE f.id=?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return File{}, httpx.Error(404, "file_not_found", "file not found")
	}
	return f, err
}
