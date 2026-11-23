package filetransfer

import (
	"context"
	"errors"
	"fmt"
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
	mu           sync.Mutex
	listed       []string
	listPrefixes []string
	listErr      error
	listErrors   []error
	removed      []string
	removeErr    error
}

func (s *testObjectStore) Ensure(context.Context) error                        { return nil }
func (s *testObjectStore) Put(context.Context, string, io.Reader, int64) error { return nil }
func (s *testObjectStore) Get(context.Context, string) (io.ReadCloser, error) {
	return nil, errors.New("not implemented")
}
func (s *testObjectStore) List(_ context.Context, prefix string) ([]string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.listPrefixes = append(s.listPrefixes, prefix)
	err := s.listErr
	if len(s.listErrors) > 0 {
		err = s.listErrors[0]
		s.listErrors = s.listErrors[1:]
	}
	return append([]string(nil), s.listed...), err
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

func expectDeleteState(mock sqlmock.Sqlmock, id, contentID, fileStatus, contentStatus string, refs int, updatedAt time.Time) {
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count,c.updated_at FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count updated_at")).AddRow(contentID, fileStatus, contentStatus, refs, updatedAt))
}

func expectFinalMetadataDelete(mock sqlmock.Sqlmock, id, contentID string) {
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0 FOR UPDATE`)).
		WithArgs(contentID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM files WHERE id=? AND status='deleting'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
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
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count,c.updated_at FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count updated_at")).AddRow(contentID, "active", "ready", 2, time.Now()))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=ref_count-1,updated_at=updated_at WHERE id=? AND ref_count>1`)).
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
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	key := "contents/" + contentID + "/00000000-chunk"
	objects := &testObjectStore{listed: []string{key}, removeErr: errors.New("temporary object failure")}
	service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count,c.updated_at FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count updated_at")).AddRow(contentID, "active", "ready", 1, time.Now()))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	if err := service.DeleteForAccount(context.Background(), id, ownerID); err == nil {
		t.Fatal("expected object removal failure")
	}

	objects.removeErr = nil
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.content_id,f.status,c.status,c.ref_count,c.updated_at FROM files f JOIN file_contents c ON c.id=f.content_id WHERE f.id=? FOR UPDATE`)).
		WithArgs(id).
		WillReturnRows(sqlmock.NewRows(strings.Fields("content_id file_status content_status ref_count updated_at")).AddRow(contentID, "deleting", "deleting", 0, time.Now()))
	mock.ExpectCommit()
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

func TestDeleteRemovesListedOrphanAndFinalizesEmptyList(t *testing.T) {
	for _, withObjects := range []bool{true, false} {
		t.Run(fmt.Sprintf("objects=%v", withObjects), func(t *testing.T) {
			db, mock, err := sqlmock.New()
			if err != nil {
				t.Fatal(err)
			}
			defer db.Close()
			id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
			prefix := "contents/" + contentID + "/"
			keys := []string{}
			if withObjects {
				keys = []string{prefix + "00000000-registered", prefix + "00000000-replaced", prefix + "unregistered"}
			}
			objects := &testObjectStore{listed: keys}
			service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
			expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
			mock.ExpectBegin()
			expectDeleteState(mock, id, contentID, "active", "ready", 1, time.Now())
			mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
				WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
			mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
				WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
			mock.ExpectCommit()
			expectFinalMetadataDelete(mock, id, contentID)
			if err := service.Delete(context.Background(), id, ownerID); err != nil {
				t.Fatal(err)
			}
			if len(objects.listPrefixes) != 1 || objects.listPrefixes[0] != prefix {
				t.Fatalf("List prefixes=%v, want [%s]", objects.listPrefixes, prefix)
			}
			if len(objects.removed) != len(keys) {
				t.Fatalf("removed=%v, want every listed key %v", objects.removed, keys)
			}
			if err := mock.ExpectationsWereMet(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestDeleteListFailureRetriesWithoutFinalizingMetadata(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	objects := &testObjectStore{listErr: errors.New("temporary list failure")}
	service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "active", "ready", 1, time.Now())
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	if err := service.Delete(context.Background(), id, ownerID); err == nil {
		t.Fatal("expected List failure")
	}

	objects.listErr = nil
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "deleting", "deleting", 0, time.Now())
	mock.ExpectCommit()
	expectFinalMetadataDelete(mock, id, contentID)
	if err := service.Delete(context.Background(), id, ownerID); err != nil {
		t.Fatal(err)
	}
	if len(objects.removed) != 0 {
		t.Fatalf("empty List removed objects: %v", objects.removed)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestDeleteMetadataFailureIsRetryable(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	objects := &testObjectStore{}
	service := NewService(db, objects, 1, 1<<20, 1<<20, 1)
	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "ready")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "active", "ready", 1, time.Now())
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE file_contents SET ref_count=0,status='deleting' WHERE id=? AND ref_count=1`)).
		WithArgs(contentID).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`UPDATE files SET status='deleting' WHERE id=? AND status='active'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectCommit()
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0 FOR UPDATE`)).
		WithArgs(contentID).WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM files WHERE id=? AND status='deleting'`)).
		WithArgs(id).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectExec(regexp.QuoteMeta(`DELETE FROM file_contents WHERE id=? AND status='deleting' AND ref_count=0`)).
		WithArgs(contentID).WillReturnError(errors.New("temporary database failure"))
	mock.ExpectRollback()
	if err := service.Delete(context.Background(), id, ownerID); err == nil {
		t.Fatal("expected metadata failure")
	}

	expectGetFile(mock, id, contentID, ownerID, uuid.NewString(), "deleting")
	mock.ExpectBegin()
	expectDeleteState(mock, id, contentID, "deleting", "deleting", 0, time.Now())
	mock.ExpectCommit()
	expectFinalMetadataDelete(mock, id, contentID)
	if err := service.Delete(context.Background(), id, ownerID); err != nil {
		t.Fatal(err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
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
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id,status FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`)).
		WithArgs(1, sha).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id status")).AddRow(uuid.NewString(), "deleting"))
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
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id,status FROM file_contents WHERE size=? AND sha256=? FOR UPDATE`)).
		WithArgs(1, sha).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id status")).AddRow(contentID, "uploading"))
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
