package auth

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
)

func TestRateLimiterWindowAndIndependentKeys(t *testing.T) {
	limiter := newRateLimiter(RateLimitConfig{Limit: 2, Window: time.Minute, MaxKeys: 10})
	now := time.Unix(1_700_000_000, 0)
	limiter.now = func() time.Time { return now }

	if allowed, _ := limiter.allow("alice"); !allowed {
		t.Fatal("first request was rejected")
	}
	if allowed, _ := limiter.allow("alice"); !allowed {
		t.Fatal("second request was rejected")
	}
	if allowed, retry := limiter.allow("alice"); allowed || retry != time.Minute {
		t.Fatalf("third request allowed=%t retry=%v, want rejected with 1m", allowed, retry)
	}
	if allowed, _ := limiter.allow("bob"); !allowed {
		t.Fatal("independent key was rejected")
	}
	now = now.Add(30 * time.Second)
	if allowed, retry := limiter.allow("alice"); allowed || retry != 30*time.Second {
		t.Fatalf("request within window allowed=%t retry=%v, want rejected with 30s", allowed, retry)
	}
	now = now.Add(30 * time.Second)
	if allowed, _ := limiter.allow("alice"); !allowed {
		t.Fatal("request after window reset was rejected")
	}
}

func TestRateLimiterBoundsAndCleansEntries(t *testing.T) {
	limiter := newRateLimiter(RateLimitConfig{Limit: 1, Window: time.Minute, MaxKeys: 2})
	now := time.Unix(1_700_000_000, 0)
	limiter.now = func() time.Time { return now }
	if allowed, _ := limiter.allow("one"); !allowed {
		t.Fatal("first key was rejected")
	}
	if allowed, _ := limiter.allow("two"); !allowed {
		t.Fatal("second key was rejected")
	}
	if allowed, retry := limiter.allow("three"); allowed || retry != time.Minute {
		t.Fatalf("new key at capacity allowed=%t retry=%v, want rejected with 1m", allowed, retry)
	}
	if got := len(limiter.entries); got != 2 {
		t.Fatalf("entries=%d, want bounded size 2", got)
	}
	now = now.Add(time.Minute)
	if allowed, _ := limiter.allow("three"); !allowed {
		t.Fatal("new key was rejected after stale entries expired")
	}
	if got := len(limiter.entries); got != 1 {
		t.Fatalf("entries=%d after cleanup, want 1", got)
	}
}

func TestAuthHandlersRejectRateLimitedRequestsBeforeService(t *testing.T) {
	tokens := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	cfg := RateLimitConfig{Limit: 1, Window: time.Minute, MaxKeys: 10}
	handler := NewHandler(&Service{tokens: tokens}, cfg, cfg)
	const remoteAddr = "192.0.2.10:4321"

	t.Run("register by client IP", func(t *testing.T) {
		handler.registerLimiter.allow("192.0.2.10")
		req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/register", strings.NewReader(`{"username":"alice","display_name":"Alice","password":"123456"}`))
		req.RemoteAddr = remoteAddr
		req.Header.Set("X-Forwarded-For", "198.51.100.99")
		recorder := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(recorder)
		c.Request = req
		handler.Register(c)
		assertRateLimited(t, recorder)
	})

	t.Run("login by IP and normalized username", func(t *testing.T) {
		handler.loginLimiter.allow("192.0.2.10\x00ALICE")
		req := httptest.NewRequest(http.MethodPost, "/api/v1/auth/login", strings.NewReader(`{"username":"aLiCe","password":"123456"}`))
		req.RemoteAddr = remoteAddr
		req.Header.Set("X-Forwarded-For", "198.51.100.99")
		recorder := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(recorder)
		c.Request = req
		handler.Login(c)
		assertRateLimited(t, recorder)
	})
}

func assertRateLimited(t *testing.T, recorder *httptest.ResponseRecorder) {
	t.Helper()
	if recorder.Code != http.StatusTooManyRequests {
		t.Fatalf("status=%d body=%s, want 429", recorder.Code, recorder.Body.String())
	}
	if got := recorder.Header().Get("Retry-After"); got != "60" {
		t.Fatalf("Retry-After=%q, want 60", got)
	}
	if !strings.Contains(recorder.Body.String(), `"code":"rate_limited"`) {
		t.Fatalf("response does not contain stable rate_limited code: %s", recorder.Body.String())
	}
}
