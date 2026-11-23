package stress

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLoadIsolatedEnvironment(t *testing.T) {
	dir := t.TempDir()
	base := "MYSQL_ROOT_PASSWORD=root-secret\nMYSQL_PORT=14306\nMINIO_ROOT_USER=base-minio\nMINIO_ROOT_PASSWORD=minio-secret\nMINIO_API_PORT=19900\n"
	stress := "STRESS_MYSQL_USER=secure_im_stress\nSTRESS_MYSQL_PASSWORD=stress-db-secret\nSTRESS_JWT_SECRET=12345678901234567890123456789012\n"
	if err := os.WriteFile(filepath.Join(dir, ".env"), []byte(base), 0o600); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, ".env.stress")
	if err := os.WriteFile(path, []byte(stress), 0o600); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"STRESS_MYSQL_ROOT_DSN", "STRESS_MYSQL_DATABASE", "STRESS_MINIO_BUCKET", "MINIO_ENDPOINT", "MINIO_ACCESS_KEY", "MINIO_SECRET_KEY", "MINIO_BUCKET", "MYSQL_ADDR", "MYSQL_DATABASE", "STRESS_JWT_SECRET"} {
		t.Setenv(key, "")
	}
	env, err := LoadEnvironment(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(env.MySQLRootDSN, "root:root-secret@tcp(127.0.0.1:14306)/secure_im_stress") {
		t.Fatalf("unexpected root DSN %q", env.MySQLRootDSN)
	}
	if string(env.JWTSecret) != "12345678901234567890123456789012" || env.MinIOBucket != "secure-im-stress-files" || env.MinIOEndpoint != "127.0.0.1:19900" || env.MinIOSecretKey != "minio-secret" {
		t.Fatalf("unexpected isolated environment %+v", env)
	}
}
func TestDebugStatsBudget(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"data":{"go":{"heap_alloc_bytes":123,"goroutines":7},"file_memory_budget":{"used":1048576,"limit":524288000},"file_chunk_slots":{"used":7,"limit":96}}}`))
	}))
	defer server.Close()
	metrics := NewMetrics("test", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	debug := telemetry.debugStats(context.Background())
	metrics.BackendBudget(debug.budgetUsed, debug.budgetLimit)
	metrics.BackendChunkSlots(debug.slotUsed, debug.slotLimit)
	metrics.BackendRuntime(debug.heapMiB, debug.goroutines)
	progress := metrics.Snapshot(1, "stable", 0)
	if progress.FileBudgetUsedMiB == nil || *progress.FileBudgetUsedMiB != 1 || progress.FileBudgetLimitMiB == nil || *progress.FileBudgetLimitMiB != 500 {
		t.Fatalf("budget was not decoded: %+v", progress)
	}
	if progress.FileChunkSlotsUsed == nil || *progress.FileChunkSlotsUsed != 7 || progress.FileChunkSlotsLimit == nil || *progress.FileChunkSlotsLimit != 96 {
		t.Fatalf("chunk slots were not decoded: %+v", progress)
	}
	if progress.GoHeapMiB == nil || progress.Goroutines == nil || *progress.Goroutines != 7 {
		t.Fatal("backend Go runtime stats were not decoded")
	}
	if progress.BackendRSSMiB != nil || progress.BackendCPUPercent != nil {
		t.Fatal("Go heap must not be fabricated as process RSS/CPU")
	}
}

func TestDebugStatsWithoutChunkSlotsStaysUnknown(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"data":{"file_memory_budget":{"used":0,"limit":524288000}}}`))
	}))
	defer server.Close()
	metrics := NewMetrics("legacy", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	debug := telemetry.debugStats(context.Background())
	metrics.BackendChunkSlots(debug.slotUsed, debug.slotLimit)
	progress := metrics.Snapshot(1, "stable", 0)
	if progress.FileChunkSlotsUsed != nil || progress.FileChunkSlotsLimit != nil {
		t.Fatalf("legacy debug stats fabricated chunk slots: %+v", progress)
	}
}

func TestTelemetryShutdownIsNotHealthFailure(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte(`{"data":{"status":"ready"}}`)) }))
	defer server.Close()
	metrics := NewMetrics("shutdown", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	telemetry.sample(ctx)
	if got := metrics.Totals().HealthFailures; got != 0 {
		t.Fatalf("shutdown recorded %d health failures", got)
	}
}

func TestTelemetryAllowsBriefReadinessDelayUnderLoad(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(3 * time.Second)
		_, _ = w.Write([]byte(`{"data":{"status":"ready"}}`))
	}))
	defer server.Close()
	metrics := NewMetrics("delayed-ready", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	telemetry.sample(context.Background())
	if got := metrics.Totals().HealthFailures; got != 0 {
		t.Fatalf("brief readiness delay recorded %d health failures", got)
	}
}

func TestTelemetrySingleHealthFailureIsNotRecorded(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer server.Close()
	metrics := NewMetrics("single-health-failure", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	telemetry.sampleHealth(context.Background())
	if got := metrics.Totals().HealthFailures; got != 0 {
		t.Fatalf("single readiness failure recorded %d health failures", got)
	}
}

func TestTelemetryHealthFailureRequiresThreeConsecutiveFailures(t *testing.T) {
	statuses := []int{
		http.StatusServiceUnavailable,
		http.StatusServiceUnavailable,
		http.StatusServiceUnavailable,
		http.StatusOK,
		http.StatusServiceUnavailable,
		http.StatusServiceUnavailable,
		http.StatusServiceUnavailable,
	}
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(statuses[calls])
		calls++
	}))
	defer server.Close()
	metrics := NewMetrics("consecutive-health-failures", 500)
	telemetry := NewTelemetry(NewAPIClient(server.URL), metrics, "unused")
	wantTotals := []int64{0, 0, 1, 1, 1, 1, 2}
	for i, want := range wantTotals {
		telemetry.sampleHealth(context.Background())
		if got := metrics.Totals().HealthFailures; got != want {
			t.Fatalf("sample %d health failures = %d, want %d", i+1, got, want)
		}
	}
}
