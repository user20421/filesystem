package stress

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func TestUploadChunkRetriesTransientServerError(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Error(err)
		}
		if string(body) != "chunk-content" {
			t.Errorf("body=%q", body)
		}
		if requests.Add(1) == 1 {
			w.WriteHeader(http.StatusInternalServerError)
			_, _ = w.Write([]byte(`{"error":{"code":"internal_error"}}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"data":{"uploaded":true,"index":0}}`))
	}))
	defer server.Close()
	pool := NewFilePool(DefaultConfig(), NewAPIClient(server.URL), NewMetrics("retry", 500), 1, func(error) {}, nil)
	if err := pool.uploadChunk(context.Background(), "token", "file-id", 0, []byte("chunk-content"), "sum"); err != nil {
		t.Fatal(err)
	}
	if got := requests.Load(); got != 2 {
		t.Fatalf("requests=%d, want 2", got)
	}
}
func TestUploadChunkDoesNotRetryClientError(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { requests.Add(1); w.WriteHeader(http.StatusBadRequest) }))
	defer server.Close()
	pool := NewFilePool(DefaultConfig(), NewAPIClient(server.URL), NewMetrics("no-retry", 500), 1, func(error) {}, nil)
	if err := pool.uploadChunk(context.Background(), "token", "file-id", 0, []byte("x"), "sum"); err == nil {
		t.Fatal("expected HTTP 400")
	}
	if got := requests.Load(); got != 1 {
		t.Fatalf("requests=%d, want 1", got)
	}
}

func TestTransferInstantSkipsUploadAndComplete(t *testing.T) {
	cfg := DefaultConfig()
	cfg.FileSize = 64 << 10
	cfg.ChunkSize = 64 << 10
	cfg.FileBudgetMiB = 1
	job := fileJob{id: "instant-seed", sender: User{ID: "sender", Token: "sender-token"}, recipient: User{ID: "recipient", Token: "recipient-token"}}
	plaintext := make([]byte, int(cfg.FileSize))
	fillSynthetic(plaintext, 0, job.id)
	var unexpected atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/api/v1/files/init":
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_, _ = io.WriteString(w, `{"data":{"id":"file-id","size":65536,"chunk_size":65536,"chunk_count":1,"status":"completed","instant":true}}`)
		case r.Method == http.MethodGet && r.URL.Path == "/api/v1/files/file-id/download":
			_, _ = w.Write(plaintext)
		default:
			unexpected.Add(1)
			w.WriteHeader(http.StatusInternalServerError)
		}
	}))
	defer server.Close()
	pool := NewFilePool(cfg, NewAPIClient(server.URL), NewMetrics("instant", 500), 1, func(err error) { t.Error(err) }, nil)
	if err := pool.transfer(context.Background(), job); err != nil {
		t.Fatal(err)
	}
	if got := unexpected.Load(); got != 0 {
		t.Fatalf("instant transfer made %d upload/complete requests", got)
	}
}

func TestDownloadWaitsForBudgetBeforeOpeningRequest(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
	}))
	defer server.Close()
	pool := NewFilePool(DefaultConfig(), NewAPIClient(server.URL), NewMetrics("download-budget", 500), 1, func(error) {}, nil)
	if err := pool.budget.Acquire(context.Background(), pool.cfg.FileBudgetMiB<<20); err != nil {
		t.Fatal(err)
	}
	// 预算已满时下载必须在 Acquire 处等待；若先建立请求，服务端会向无人读取的 socket 写数据直至超时。
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if _, err := pool.downloadHash(ctx, "token", "file-id"); err == nil {
		t.Fatal("expected budget wait to end with context error")
	}
	if got := requests.Load(); got != 0 {
		t.Fatalf("download opened %d requests while the buffer budget was full", got)
	}
}
