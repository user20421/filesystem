package auth

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

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
	protected := handler.Middleware(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { called = true }))
	request := httptest.NewRequest(http.MethodGet, "/api/v1/users/me", nil)
	request.Header.Set("Authorization", "Bearer "+raw)
	recorder := httptest.NewRecorder()
	protected.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusUnauthorized || called {
		t.Fatalf("status=%d called=%t, want 401 and no handler call", recorder.Code, called)
	}
}
