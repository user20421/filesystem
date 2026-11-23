package filetransfer

import (
	"context"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
	"secure-im/backend/internal/auth"
)

type deadlineRecorder struct {
	*httptest.ResponseRecorder
	virtualNow   time.Time
	writeAdvance time.Duration
	deadlines    []time.Time
	idleLimits   []time.Duration
}

func newDeadlineRecorder(writeAdvance time.Duration) *deadlineRecorder {
	return &deadlineRecorder{ResponseRecorder: httptest.NewRecorder(), virtualNow: time.Unix(0, 0), writeAdvance: writeAdvance}
}

func (w *deadlineRecorder) SetWriteDeadline(deadline time.Time) error {
	w.deadlines = append(w.deadlines, deadline)
	if !deadline.IsZero() {
		w.idleLimits = append(w.idleLimits, time.Until(deadline))
	}
	return nil
}

func (w *deadlineRecorder) Write(p []byte) (int, error) {
	w.virtualNow = w.virtualNow.Add(w.writeAdvance)
	return w.ResponseRecorder.Write(p)
}

type downloadObjectStore struct{ testObjectStore }

func (s *downloadObjectStore) Get(_ context.Context, key string) (io.ReadCloser, error) {
	return io.NopCloser(strings.NewReader(key)), nil
}

func expectFileDownloadAuth(mock sqlmock.Sqlmock, userID string) string {
	tokens := auth.NewTokenManager([]byte("01234567890123456789012345678901"), time.Hour, time.Hour)
	token, err := tokens.AccessToken(userID)
	if err != nil {
		panic(err)
	}
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id,username,display_name,created_at FROM users WHERE id=?`)).
		WithArgs(userID).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id username display_name created_at")).AddRow(userID, "OWNER", "Owner", time.Now()))
	return token
}

func downloadFileRow(id, contentID, ownerID, recipientID string, size int64, chunkCount int) *sqlmock.Rows {
	return sqlmock.NewRows(strings.Fields("id content_id owner_id recipient_id original_name size sha256 chunk_size chunk_count status revision created_at completed_at")).
		AddRow(id, contentID, ownerID, recipientID, "file.bin", size, strings.Repeat("a", 64), 1, chunkCount, "ready", 0, time.Now(), time.Now())
}

func TestDownloadBeyondSixtySecondsUsesRollingWriteDeadline(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	recipientID := uuid.NewString()
	token := expectFileDownloadAuth(mock, ownerID)
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).WithArgs(id).
		WillReturnRows(downloadFileRow(id, contentID, ownerID, recipientID, 2, 2))
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT chunk_index,object_key,plaintext_size,sha256 FROM file_chunks WHERE content_id=? ORDER BY chunk_index`)).
		WithArgs(contentID).
		WillReturnRows(sqlmock.NewRows(strings.Fields("chunk_index object_key plaintext_size sha256")).
			AddRow(0, "a", 1, SHA256([]byte("a"))).
			AddRow(1, "b", 1, SHA256([]byte("b"))))

	service := NewService(db, &downloadObjectStore{}, 1, 1<<20, 1<<20, 2)
	handler := NewHandler(service, slog.New(slog.NewTextHandler(io.Discard, nil)), 1<<20, nil)
	tokens := auth.NewTokenManager([]byte("01234567890123456789012345678901"), time.Hour, time.Hour)
	authHandler := auth.NewHandler(auth.NewService(db, tokens), auth.RateLimitConfig{}, auth.RateLimitConfig{})
	req := httptest.NewRequest(http.MethodGet, "/api/v1/files/"+id+"/download", nil)
	req.SetPathValue("file_id", id)
	req.Header.Set("Authorization", "Bearer "+token)
	// Each payload write takes 31 virtual seconds. A fixed 60-second response
	// deadline would abort the second write; a rolling idle deadline must not.
	response := newDeadlineRecorder(31 * time.Second)
	authHandler.Middleware(http.HandlerFunc(handler.Download)).ServeHTTP(response, req)

	if response.Code != http.StatusOK || response.Body.String() != "ab" {
		t.Fatalf("download failed: status=%d body=%q", response.Code, response.Body.String())
	}
	if elapsed := response.virtualNow.Sub(time.Unix(0, 0)); elapsed <= 60*time.Second {
		t.Fatalf("test did not exceed 60 seconds: %s", elapsed)
	}
	if len(response.deadlines) != 3 || !response.deadlines[0].IsZero() {
		t.Fatalf("deadlines = %v, want one clear followed by two write deadlines", response.deadlines)
	}
	if len(response.idleLimits) != 2 {
		t.Fatalf("idle deadline count = %d, want 2", len(response.idleLimits))
	}
	for _, idle := range response.idleLimits {
		if idle < 29*time.Second || idle > 30*time.Second {
			t.Fatalf("write idle deadline = %s, want approximately 30s", idle)
		}
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestCompleteClearsGlobalWriteDeadline(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	id, contentID, ownerID := uuid.NewString(), uuid.NewString(), uuid.NewString()
	token := expectFileDownloadAuth(mock, ownerID)
	mock.ExpectQuery(regexp.QuoteMeta(selectFile + `WHERE f.id=?`)).WithArgs(id).
		WillReturnRows(downloadFileRow(id, contentID, ownerID, uuid.NewString(), 2, 2))

	service := NewService(db, &downloadObjectStore{}, 1, 1<<20, 1<<20, 1)
	handler := NewHandler(service, slog.New(slog.NewTextHandler(io.Discard, nil)), 1<<20, nil)
	tokens := auth.NewTokenManager([]byte("01234567890123456789012345678901"), time.Hour, time.Hour)
	authHandler := auth.NewHandler(auth.NewService(db, tokens), auth.RateLimitConfig{}, auth.RateLimitConfig{})
	req := httptest.NewRequest(http.MethodPost, "/api/v1/files/"+id+"/complete", nil)
	req.SetPathValue("file_id", id)
	req.Header.Set("Authorization", "Bearer "+token)
	response := newDeadlineRecorder(0)
	authHandler.Middleware(http.HandlerFunc(handler.Complete)).ServeHTTP(response, req)

	if response.Code != http.StatusOK {
		t.Fatalf("complete status = %d, body=%s", response.Code, response.Body.String())
	}
	if len(response.deadlines) != 1 || !response.deadlines[0].IsZero() {
		t.Fatalf("complete deadlines = %v, want global deadline cleared", response.deadlines)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
