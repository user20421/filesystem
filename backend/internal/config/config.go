package config

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type Config struct {
	Address              string
	JWTSecret            []byte
	AccessTTL            time.Duration
	RefreshTTL           time.Duration
	WSTicketTTL          time.Duration
	MySQLDSN             string
	MinIOEndpoint        string
	MinIOAccessKey       string
	MinIOSecretKey       string
	MinIOBucket          string
	MinIOUseSSL          bool
	MaxActiveFiles       int
	MaxChunkSize         int64
	MaxFileSize          int64
	UploadConcurrency    int
	FileMemoryBudget     int64
	EnableDebugStats     bool
	WSMaxConnections     int
	RegisterRateLimit    int
	LoginRateLimit       int
	AuthRateLimitWindow  time.Duration
	AuthRateLimitMaxKeys int
}

func Load() (Config, error) {
	c := Config{
		Address:              env("LISTEN_ADDR", "127.0.0.1:8080"),
		JWTSecret:            []byte(os.Getenv("JWT_SECRET")),
		AccessTTL:            duration("ACCESS_TOKEN_TTL", 15*time.Minute),
		RefreshTTL:           duration("REFRESH_TOKEN_TTL", 30*24*time.Hour),
		WSTicketTTL:          duration("WS_TICKET_TTL", time.Minute),
		MySQLDSN:             os.Getenv("MYSQL_DSN"),
		MinIOEndpoint:        env("MINIO_ENDPOINT", "127.0.0.1:9000"),
		MinIOAccessKey:       os.Getenv("MINIO_ACCESS_KEY"),
		MinIOSecretKey:       os.Getenv("MINIO_SECRET_KEY"),
		MinIOBucket:          env("MINIO_BUCKET", "secure-im-files"),
		MinIOUseSSL:          boolean("MINIO_USE_SSL", false),
		MaxActiveFiles:       integer("MAX_ACTIVE_FILES", 3),
		MaxChunkSize:         int64(integer("MAX_CHUNK_SIZE", 5<<20)),
		MaxFileSize:          int64(integer("MAX_FILE_SIZE", 5<<30)),
		UploadConcurrency:    integer("UPLOAD_CONCURRENCY", 96),
		FileMemoryBudget:     int64(integer("FILE_MEMORY_BUDGET", 500<<20)),
		EnableDebugStats:     boolean("ENABLE_DEBUG_STATS", false),
		WSMaxConnections:     integer("WS_MAX_CONNECTIONS", 1000),
		RegisterRateLimit:    integer("AUTH_REGISTER_RATE_LIMIT", 30),
		LoginRateLimit:       integer("AUTH_LOGIN_RATE_LIMIT", 10),
		AuthRateLimitWindow:  duration("AUTH_RATE_LIMIT_WINDOW", time.Minute),
		AuthRateLimitMaxKeys: integer("AUTH_RATE_LIMIT_MAX_KEYS", 10000),
	}
	if c.MySQLDSN == "" {
		c.MySQLDSN = fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true&charset=utf8mb4&collation=utf8mb4_unicode_ci&timeout=5s",
			env("MYSQL_USER", "root"), os.Getenv("MYSQL_PASSWORD"), env("MYSQL_ADDR", "127.0.0.1:3306"), env("MYSQL_DATABASE", "secure_im"))
	}
	if len(c.JWTSecret) < 32 {
		return c, fmt.Errorf("JWT_SECRET must contain at least 32 bytes")
	}
	if c.MaxActiveFiles < 3 || c.MaxActiveFiles > 5 {
		return c, fmt.Errorf("MAX_ACTIVE_FILES must be between 3 and 5")
	}
	if c.MaxChunkSize < 64<<10 || c.MaxChunkSize > 16<<20 {
		return c, fmt.Errorf("MAX_CHUNK_SIZE must be between 64KiB and 16MiB")
	}
	if c.MaxFileSize < c.MaxChunkSize || c.UploadConcurrency < 1 || c.UploadConcurrency > 256 {
		return c, fmt.Errorf("invalid file size or upload concurrency")
	}
	if c.FileMemoryBudget <= 0 {
		return c, fmt.Errorf("FILE_MEMORY_BUDGET must be positive")
	}
	if c.WSMaxConnections < 1 || c.WSMaxConnections > 100000 {
		return c, fmt.Errorf("WS_MAX_CONNECTIONS must be between 1 and 100000")
	}
	if c.RegisterRateLimit < 0 || c.LoginRateLimit < 0 {
		return c, fmt.Errorf("auth rate limits must not be negative")
	}
	if c.AuthRateLimitWindow <= 0 || c.AuthRateLimitMaxKeys < 1 {
		return c, fmt.Errorf("auth rate limit window must be positive and max keys must be at least 1")
	}
	if c.AccessTTL <= 0 || c.RefreshTTL <= 0 || c.WSTicketTTL <= 0 {
		return c, fmt.Errorf("token TTLs must be positive")
	}
	return c, nil
}

func env(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
func integer(key string, fallback int) int {
	v, err := strconv.Atoi(os.Getenv(key))
	if err != nil {
		return fallback
	}
	return v
}
func boolean(key string, fallback bool) bool {
	v, err := strconv.ParseBool(os.Getenv(key))
	if err != nil {
		return fallback
	}
	return v
}
func duration(key string, fallback time.Duration) time.Duration {
	v, err := time.ParseDuration(os.Getenv(key))
	if err != nil {
		return fallback
	}
	return v
}
