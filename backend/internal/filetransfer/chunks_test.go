package filetransfer

import (
	"context"
	"regexp"
	"strings"
	"testing"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
)

func TestUploadChunkReadySharedContentAcceptsMatchingRetry(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	service := NewService(db, &testObjectStore{}, 1, 1<<20, 1<<20, 1)
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	sha := strings.Repeat("a", 64)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "uploading")
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT plaintext_size,sha256 FROM file_chunks WHERE content_id=? AND chunk_index=?`)).
		WithArgs(contentID, 0).
		WillReturnRows(sqlmock.NewRows([]string{"plaintext_size", "sha256"}).AddRow(1, sha))

	if err := service.UploadChunk(context.Background(), id, ownerID, 0, sha, strings.NewReader("x")); err != nil {
		t.Fatalf("matching upload after shared content completed: %v", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestReadChunkObjectFailureReleasesSlotAndMemory(t *testing.T) {
	service := NewServiceWithMemoryBudget(nil, &testObjectStore{}, 1, 1<<20, 1<<20, 1, 8<<20)
	defer func() {
		if recovered := recover(); recovered != nil {
			t.Fatalf("readChunk panicked during error cleanup: %v", recovered)
		}
	}()

	_, _, err := service.readChunk(context.Background(), "content", Chunk{Index: 2, ObjectKey: "missing", PlaintextSize: 2 << 20})
	if err == nil {
		t.Fatal("readChunk succeeded with a failing object store")
	}
	if used, _ := service.MemoryStats(); used != 0 {
		t.Fatalf("memory reservation leaked after object failure: used=%d", used)
	}
	active, queued, byKey := schedulerState(service.scheduler)
	if active != 0 || queued != 0 || len(byKey) != 0 {
		t.Fatalf("chunk slot leaked after object failure: active=%d queued=%d byKey=%v", active, queued, byKey)
	}
}
