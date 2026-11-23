package auth

import (
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/httpx"
)

type RateLimitConfig struct {
	Limit   int
	Window  time.Duration
	MaxKeys int
}

type rateLimitEntry struct {
	windowStart time.Time
	count       int
}

type rateLimiter struct {
	limit   int
	window  time.Duration
	maxKeys int
	now     func() time.Time
	entries map[string]rateLimitEntry
	mu      sync.Mutex
}

func newRateLimiter(cfg RateLimitConfig) *rateLimiter {
	if cfg.Limit == 0 {
		return nil
	}
	return &rateLimiter{
		limit:   cfg.Limit,
		window:  cfg.Window,
		maxKeys: cfg.MaxKeys,
		now:     time.Now,
		entries: make(map[string]rateLimitEntry),
	}
}

func (l *rateLimiter) allow(key string) (bool, time.Duration) {
	now := l.now()
	l.mu.Lock()
	defer l.mu.Unlock()

	entry, exists := l.entries[key]
	if exists && now.Sub(entry.windowStart) >= l.window {
		entry = rateLimitEntry{windowStart: now}
	}
	if exists {
		if entry.count >= l.limit {
			return false, retryAfter(entry.windowStart.Add(l.window), now)
		}
		entry.count++
		l.entries[key] = entry
		return true, 0
	}

	if len(l.entries) >= l.maxKeys {
		l.cleanupLocked(now)
		if len(l.entries) >= l.maxKeys {
			earliest := now.Add(l.window)
			for _, item := range l.entries {
				expires := item.windowStart.Add(l.window)
				if expires.Before(earliest) {
					earliest = expires
				}
			}
			return false, retryAfter(earliest, now)
		}
	}
	l.entries[key] = rateLimitEntry{windowStart: now, count: 1}
	return true, 0
}

func (l *rateLimiter) cleanupLocked(now time.Time) {
	for key, entry := range l.entries {
		if now.Sub(entry.windowStart) >= l.window {
			delete(l.entries, key)
		}
	}
}

func retryAfter(expires, now time.Time) time.Duration {
	remaining := expires.Sub(now)
	if remaining <= 0 {
		return time.Second
	}
	return ((remaining + time.Second - 1) / time.Second) * time.Second
}

func allowRequest(c *gin.Context, limiter *rateLimiter, key string) bool {
	if limiter == nil {
		return true
	}
	allowed, retry := limiter.allow(key)
	if allowed {
		return true
	}
	seconds := int64(retry / time.Second)
	if seconds < 1 {
		seconds = 1
	}
	c.Writer.Header().Set("Retry-After", strconv.FormatInt(seconds, 10))
	httpx.Fail(c, httpx.Error(http.StatusTooManyRequests, "rate_limited", "too many requests, try again later"))
	return false
}
