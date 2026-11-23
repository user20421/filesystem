package auth

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/httpx"
)

func TestMiddlewareRejectsDeletedUserAccessToken(t *testing.T) {
	tokens := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	raw, err := tokens.AccessToken("deleted-user")
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(NewService(nil, tokens), RateLimitConfig{}, RateLimitConfig{})
	handler.lookupUser = func(context.Context, string) (User, error) {
		return User{}, httpx.Error(http.StatusUnauthorized, "invalid_token", "user no longer exists")
	}
	called := false
	aborted := false
	router := gin.New()
	router.Use(func(c *gin.Context) {
		defer func() { aborted = c.IsAborted() }()
		c.Next()
	}, handler.Middleware())
	router.GET("/api/v1/users/me", func(*gin.Context) { called = true })
	request := httptest.NewRequest(http.MethodGet, "/api/v1/users/me", nil)
	request.Header.Set("Authorization", "Bearer "+raw)
	recorder := httptest.NewRecorder()
	router.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusUnauthorized || called || !aborted {
		t.Fatalf("status=%d called=%t aborted=%t, want 401, no handler call and abort", recorder.Code, called, aborted)
	}
}

func TestMiddlewareAbortsAuthenticationFailures(t *testing.T) {
	tokens := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	for _, test := range []struct {
		name   string
		header string
	}{
		{name: "missing"},
		{name: "wrong scheme", header: "Basic token"},
		{name: "invalid token", header: "Bearer invalid"},
	} {
		t.Run(test.name, func(t *testing.T) {
			handler := NewHandler(NewService(nil, tokens), RateLimitConfig{}, RateLimitConfig{})
			handler.lookupUser = func(context.Context, string) (User, error) {
				t.Fatal("user lookup must not run for invalid authentication")
				return User{}, nil
			}
			called := false
			aborted := false
			router := gin.New()
			router.Use(func(c *gin.Context) {
				defer func() { aborted = c.IsAborted() }()
				c.Next()
			}, handler.Middleware())
			router.GET("/api/v1/users/me", func(*gin.Context) { called = true })
			request := httptest.NewRequest(http.MethodGet, "/api/v1/users/me", nil)
			request.Header.Set("Authorization", test.header)
			recorder := httptest.NewRecorder()
			router.ServeHTTP(recorder, request)
			if recorder.Code != http.StatusUnauthorized || called || !aborted {
				t.Fatalf("status=%d called=%t aborted=%t, want 401, no handler call and abort", recorder.Code, called, aborted)
			}
		})
	}
}

func TestMiddlewareStoresUserIDInRequestContext(t *testing.T) {
	tokens := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	raw, err := tokens.AccessToken("user-123")
	if err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(NewService(nil, tokens), RateLimitConfig{}, RateLimitConfig{})
	handler.lookupUser = func(_ context.Context, id string) (User, error) {
		if id != "user-123" {
			t.Fatalf("lookup user ID=%q", id)
		}
		return User{ID: id}, nil
	}
	var gotID string
	aborted := true
	router := gin.New()
	router.Use(func(c *gin.Context) {
		defer func() { aborted = c.IsAborted() }()
		c.Next()
	}, handler.Middleware())
	router.GET("/api/v1/users/me", func(c *gin.Context) {
		gotID = UserID(c.Request.Context())
		c.Writer.WriteHeader(http.StatusNoContent)
	})
	request := httptest.NewRequest(http.MethodGet, "/api/v1/users/me", nil)
	request.Header.Set("Authorization", "Bearer "+raw)
	recorder := httptest.NewRecorder()
	router.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusNoContent || gotID != "user-123" || aborted {
		t.Fatalf("status=%d user_id=%q aborted=%t", recorder.Code, gotID, aborted)
	}
}
