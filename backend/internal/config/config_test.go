package config

import (
	"strings"
	"testing"
	"time"
)

func TestFileDefaultsUseFiveMiBChunksWithoutKEK(t *testing.T) {
	t.Setenv("JWT_SECRET", strings.Repeat("x", 32))
	t.Setenv("FILE_KEK", "")
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
