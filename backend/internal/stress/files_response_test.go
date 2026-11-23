package stress

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
)

func TestTransferUsesInitChunkLayout(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileSize = 192 << 10
	cfg.ChunkSize = 64 << 10
	cfg.FileBudgetMiB = 1
	job := fileJob{id: "layout-seed", sender: User{ID: "sender", Token: "sender-token"}, recipient: User{ID: "recipient", Token: "recipient-token"}}
	plaintext := make([]byte, int(cfg.FileSize))
	fillSynthetic(plaintext, 0, job.id)
	const responseChunkSize = int64(128 << 10)
	var uploads atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/files/init":
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_, _ = fmt.Fprintf(w, `{"data":{"id":"file-id","size":%d,"chunk_size":%d,"chunk_count":2,"status":"uploading","instant":false}}`, cfg.FileSize, responseChunkSize)
		case r.Method == http.MethodPut && strings.HasPrefix(r.URL.Path, "/api/v1/files/file-id/chunks/"):
			index, err := strconv.Atoi(strings.TrimPrefix(r.URL.Path, "/api/v1/files/file-id/chunks/"))
			if err != nil {
				t.Error(err)
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			body, err := io.ReadAll(r.Body)
			if err != nil {
				t.Error(err)
			}
			offset := int64(index) * responseChunkSize
			want := plaintext[offset : offset+int64(len(body))]
			if int64(len(body)) != responseChunkSize && index != 1 {
				t.Errorf("chunk %d length=%d", index, len(body))
			}
			if index == 1 && len(body) != 64<<10 {
				t.Errorf("last chunk length=%d, want %d", len(body), 64<<10)
			}
			if !bytes.Equal(body, want) {
				t.Errorf("chunk %d does not start at response-layout offset %d", index, offset)
			}
			uploads.Add(1)
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"data":{"uploaded":true}}`)
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/files/file-id/complete":
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"data":{"id":"file-id","size":196608,"chunk_size":131072,"chunk_count":2,"status":"completed","instant":false}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/files/file-id/download":
			_, _ = w.Write(plaintext)
		default:
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusInternalServerError)
		}
	}))
	defer server.Close()
	pool := NewFilePool(cfg, NewAPIClient(server.URL), NewMetrics("layout", 500), 1, func(err error) { t.Error(err) }, nil)
	if err := pool.transfer(context.Background(), job); err != nil {
		t.Fatal(err)
	}
	if got := uploads.Load(); got != 2 {
		t.Fatalf("uploads=%d, want 2 from Init response", got)
	}
}

func TestTransferCompletedStatusSkipsUploadWithoutInstant(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileSize = 64 << 10
	cfg.ChunkSize = 64 << 10
	cfg.FileBudgetMiB = 1
	job := fileJob{id: "completed-seed", sender: User{ID: "sender", Token: "sender-token"}, recipient: User{ID: "recipient", Token: "recipient-token"}}
	plaintext := make([]byte, int(cfg.FileSize))
	fillSynthetic(plaintext, 0, job.id)
	var unexpected atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/files/init":
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_, _ = io.WriteString(w, `{"data":{"id":"file-id","size":65536,"chunk_size":65536,"chunk_count":1,"status":"completed","instant":false}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/files/file-id/download":
			_, _ = w.Write(plaintext)
		default:
			unexpected.Add(1)
			w.WriteHeader(http.StatusInternalServerError)
		}
	}))
	defer server.Close()
	pool := NewFilePool(cfg, NewAPIClient(server.URL), NewMetrics("completed", 500), 1, func(err error) { t.Error(err) }, nil)
	if err := pool.transfer(context.Background(), job); err != nil {
		t.Fatal(err)
	}
	if got := unexpected.Load(); got != 0 {
		t.Fatalf("completed transfer made %d upload/complete requests", got)
	}
}
