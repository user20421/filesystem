package filetransfer

import (
	"context"
	"errors"
	"io"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
)

type testObjectStore struct {
	mu        sync.Mutex
	removed   []string
	removeErr error
}

func (s *testObjectStore) Ensure(context.Context) error                        { return nil }
func (s *testObjectStore) Put(context.Context, string, io.Reader, int64) error { return nil }
func (s *testObjectStore) Get(context.Context, string) (io.ReadCloser, error) {
	return nil, errors.New("not implemented")
}
func (s *testObjectStore) Remove(_ context.Context, key string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.removed = append(s.removed, key)
	return s.removeErr
}

func fileRow(id, contentID, ownerID, recipientID, status string) *sqlmock.Rows {
	return sqlmock.NewRows(strings.Fields("id content_id owner_id recipient_id original_name size sha256 chunk_size chunk_count status revision created_at completed_at")).
		AddRow(id, contentID, ownerID, recipientID, "file.bin", 1, strings.Repeat("a", 64), 65536, 1, status, 0, time.Now(), nil)
}

func expectGetFile(mock sqlmock.Sqlmock, id, contentID, ownerID, recipientID, status string) {
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).
		WithArgs(id).
		WillReturnRows(fileRow(id, contentID, ownerID, recipientID, status))
}

func TestDeleteSharedContentKeepsObjects(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	objects := &testObjectStore{}
	service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count")).AddRow(contentID, "active", "ready", 2))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=ref_count-1 WHERE id=? AND ref_count>1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM files WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	if err := service.DeleteForAccount(context.Background(), id, ownerID); err != nil {
		t.Fatal(err)
	}
	if len(objects.removed) != 0 {
		t.Fatalf("shared objects removed: %v", objects.removed)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestDeleteObjectFailureIsRetryable(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	objects := &testObjectStore{removeErr: errors.New("temporary object failure")}
	service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	key := "contents/" + contentID + "/00000000-chunk"
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count")).AddRow(contentID, "active", "ready", 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	expectChunks(mock, contentID, key)
	if err := service.DeleteForAccount(context.Background(), id, ownerID); err == nil {
		t.Fatal("expected object removal failure")
	}

	objects.removeErr = nil
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count")).AddRow(contentID, "deleting", "deleting", 0))
	mock.ExpectCommit()
	expectChunks(mock, contentID, key)
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0 FOR UPDATE`)).
		WithArgs(contentID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM files WHERE id=? AND status='deleting'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	if err := service.DeleteForAccount(context.Background(), id, ownerID); err != nil {
		t.Fatal(err)
	}
	if got := len(objects.removed); got != 2 {
		t.Fatalf("object removal attempts=%d, want 2", got)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func expectChunks(mock sqlmock.Sqlmock, contentID, key string) {
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT chunk_index,object_key,plaintext_size,sha256 FROM file_chunks WHERE content_id=? ORDER BY chunk_index`)).
		WithArgs(contentID).
		WillReturnRows(sqlmock.NewRows(strings.Fields("chunk_index object_key plaintext_size sha256")).AddRow(0, key, 1, strings.Repeat("a", 64)))
}

func TestInitDuringFinalDeleteDoesNotAttachNewReference(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	service := NewService(db, &testObjectStore{}, 1, 1<<20, 1<<20, 1)
	ownerID, recipientID := uuid.NewString(), uuid.NewString()
	sha := strings.Repeat("a", 64)
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM users WHERE id=?`)).
		WithArgs(recipientID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectBegin()
	mock.ExpectExec(regexp.QuoteMeta(`INSERT INTO file_contents(id,size,sha256,chunk_size,chunk_count) VALUES(?,?,?,?,?) ON DUPLICATE KEY UPDATE id=id`)).
		WithArgs(sqlmock.AnyArg(), 1, sha, 65536, 1).WillReturnResult(sqlmock.NewResult(0, 0))
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id,status,chunk_size,chunk_count FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`)).
		WithArgs(1, sha).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id status chunk_size chunk_count")).AddRow(uuid.NewString(), "deleting", 65536, 1))
	mock.ExpectRollback()
	_, err = service.Init(context.Background(), ownerID, InitInput{Name: "file.bin", Size: 1, SHA256: sha, RecipientID: recipientID, ChunkSize: 65536})
	if err == nil || !strings.Contains(err.Error(), "previous content reference is still being deleted") {
		t.Fatalf("Init error=%v, want content cleanup pending", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestInitInstantMatchesFinalFileState(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	service := NewService(db, &testObjectStore{}, 1, 1<<20, 1<<20, 1)
	ownerID, recipientID, contentID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	sha := strings.Repeat("a", 64)
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM users WHERE id=?`)).
		WithArgs(recipientID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectBegin()
	mock.ExpectExec(regexp.QuoteMeta(`INSERT INTO file_contents(id,size,sha256,chunk_size,chunk_count) VALUES(?,?,?,?,?) ON DUPLICATE KEY UPDATE id=id`)).
		WithArgs(sqlmock.AnyArg(), 1, sha, 65536, 1).WillReturnResult(sqlmock.NewResult(0, 0))
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id,status,chunk_size,chunk_count FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`)).
		WithArgs(1, sha).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id status chunk_size chunk_count")).AddRow(contentID, "uploading", 65536, 1))
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.owner_id=? AND f.status='active' AND c.status='uploading'`)).
		WithArgs(ownerID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(0))
	mock.ExpectExec(regexp.QuoteMeta(`INSERT INTO files(id,content_id,owner_id,recipient_id,original_name) VALUES(?,?,?,?,?)`)).
		WithArgs(sqlmock.AnyArg(), contentID, ownerID, recipientID, "file.bin").WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=ref_count+1 WHERE id=?`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	// Another reference can complete the content between commit and getFile.
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).
		WithArgs(sqlmock.AnyArg()).
		WillReturnRows(fileRow(uuid.NewString(), contentID, ownerID, recipientID, "ready"))
	file, err := service.Init(context.Background(), ownerID, InitInput{Name: "file.bin", Size: 1, SHA256: sha, RecipientID: recipientID, ChunkSize: 65536})
	if err != nil {
		t.Fatal(err)
	}
	if file.Status != "completed" || !file.Instant {
		t.Fatalf("final file status=%q instant=%v, want completed/true", file.Status, file.Instant)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
