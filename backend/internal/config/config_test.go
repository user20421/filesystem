package config

import (
	"strings"
	"testing"
	"time"
)

func TestFileDefaultsUseFiveMiBChunks(t *testing.T) {
	t.Setenv("JWT_SECRET", strings.Repeat("x", 32))
	t.Setenv("MAX_CHUNK_SIZE", "")
	cfg, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MaxChunkSize != 5<<20 {
		t.Fatalf("MaxChunkSize = %d, want 5 MiB", cfg.MaxChunkSize)
	}
	if cfg.FileMemoryBudget != 500<<20 {
		t.Fatalf("FileMemoryBudget = %d, want 500 MiB", cfg.FileMemoryBudget)
	}
	if !strings.Contains(cfg.MySQLDSN, "loc=UTC") || !strings.Contains(cfg.MySQLDSN, "time_zone=%27%2B00:00%27") {
		t.Fatalf("MySQL DSN does not enforce UTC: %q", cfg.MySQLDSN)
	}
}

func TestAuthRateLimitDefaultsAndOverrides(t *testing.T) {
	t.Setenv("JWT_SECRET", strings.Repeat("x", 32))
	for _, key := range []string{"AUTH_REGISTER_RATE_LIMIT", "AUTH_LOGIN_RATE_LIMIT", "AUTH_RATE_LIMIT_WINDOW", "AUTH_RATE_LIMIT_MAX_KEYS"} {
		t.Setenv(key, "")
	}
	cfg, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.RegisterRateLimit != 30 || cfg.LoginRateLimit != 10 || cfg.AuthRateLimitWindow != time.Minute || cfg.AuthRateLimitMaxKeys != 10000 {
		t.Fatalf("unexpected auth rate defaults: register=%d login=%d window=%v maxKeys=%d", cfg.RegisterRateLimit, cfg.LoginRateLimit, cfg.AuthRateLimitWindow, cfg.AuthRateLimitMaxKeys)
	}

	t.Setenv("AUTH_REGISTER_RATE_LIMIT", "0")
	t.Setenv("AUTH_LOGIN_RATE_LIMIT", "100")
	t.Setenv("AUTH_RATE_LIMIT_WINDOW", "2m")
	t.Setenv("AUTH_RATE_LIMIT_MAX_KEYS", "25")
	cfg, err = Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.RegisterRateLimit != 0 || cfg.LoginRateLimit != 100 || cfg.AuthRateLimitWindow != 2*time.Minute || cfg.AuthRateLimitMaxKeys != 25 {
		t.Fatalf("unexpected auth rate overrides: register=%d login=%d window=%v maxKeys=%d", cfg.RegisterRateLimit, cfg.LoginRateLimit, cfg.AuthRateLimitWindow, cfg.AuthRateLimitMaxKeys)
	}
}

func TestFileCleanupDefaultsOverridesAndValidation(t *testing.T) {
	t.Setenv("JWT_SECRET", strings.Repeat("x", 32))
	for _, key := range []string{"FILE_UPLOAD_IDLE_TTL", "FILE_DELETING_RETRY_AFTER", "FILE_CLEANUP_INTERVAL"} {
		t.Setenv(key, "")
	}
	cfg, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.FileUploadIdleTTL != 24*time.Hour || cfg.FileDeletingRetryAfter != 5*time.Minute || cfg.FileCleanupInterval != time.Minute {
		t.Fatalf("unexpected cleanup defaults: idle=%v retry=%v interval=%v", cfg.FileUploadIdleTTL, cfg.FileDeletingRetryAfter, cfg.FileCleanupInterval)
	}

	t.Setenv("FILE_UPLOAD_IDLE_TTL", "2h")
	t.Setenv("FILE_DELETING_RETRY_AFTER", "10s")
	t.Setenv("FILE_CLEANUP_INTERVAL", "3s")
	cfg, err = Load()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.FileUploadIdleTTL != 2*time.Hour || cfg.FileDeletingRetryAfter != 10*time.Second || cfg.FileCleanupInterval != 3*time.Second {
		t.Fatalf("unexpected cleanup overrides: idle=%v retry=%v interval=%v", cfg.FileUploadIdleTTL, cfg.FileDeletingRetryAfter, cfg.FileCleanupInterval)
	}

	for _, key := range []string{"FILE_UPLOAD_IDLE_TTL", "FILE_DELETING_RETRY_AFTER", "FILE_CLEANUP_INTERVAL"} {
		t.Setenv(key, "0s")
		if _, err := Load(); err == nil {
			t.Fatalf("%s accepted a zero duration", key)
		}
		t.Setenv(key, "1m")
	}
}

func TestAuthRateLimitRejectsInvalidValues(t *testing.T) {
	t.Setenv("JWT_SECRET", strings.Repeat("x", 32))
	t.Setenv("AUTH_REGISTER_RATE_LIMIT", "-1")
	if _, err := Load(); err == nil {
		t.Fatal("negative register rate limit was accepted")
	}
	t.Setenv("AUTH_REGISTER_RATE_LIMIT", "30")
	t.Setenv("AUTH_RATE_LIMIT_MAX_KEYS", "0")
	if _, err := Load(); err == nil {
		t.Fatal("zero auth rate limit max keys was accepted")
	}
}
