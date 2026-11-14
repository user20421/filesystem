package stress

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
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
	pool := NewFilePool(DefaultConfig(), NewAPIClient(server.URL), NewMetrics("retry", 500), 1, func(error) {})
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
	pool := NewFilePool(DefaultConfig(), NewAPIClient(server.URL), NewMetrics("no-retry", 500), 1, func(error) {})
	if err := pool.uploadChunk(context.Background(), "token", "file-id", 0, []byte("x"), "sum"); err == nil {
		t.Fatal("expected HTTP 400")
	}
	if got := requests.Load(); got != 1 {
		t.Fatalf("requests=%d, want 1", got)
	}
}
