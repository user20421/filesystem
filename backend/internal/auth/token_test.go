package auth

import (
	"testing"
	"time"
)

func TestAccessToken(t *testing.T) {
	m := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	raw, err := m.AccessToken("user-1")
	if err != nil {
		t.Fatal(err)
	}
	id, err := m.ParseAccessToken(raw)
	if err != nil || id != "user-1" {
		t.Fatalf("got %q, %v", id, err)
	}
	other := NewTokenManager([]byte("another-secret-that-is-long-enough-123"), time.Minute, time.Hour)
	if _, err := other.ParseAccessToken(raw); err == nil {
		t.Fatal("token accepted with another secret")
	}
	m.now = func() time.Time { return time.Now().Add(-2 * time.Minute) }
	expired, _ := m.AccessToken("user-1")
	m.now = time.Now
	if _, err := m.ParseAccessToken(expired); err == nil {
		t.Fatal("expired token accepted")
	}
}
func TestRefreshTokensAreRandomAndHashed(t *testing.T) {
	m := NewTokenManager([]byte("01234567890123456789012345678901"), time.Minute, time.Hour)
	a, ah, _, err := m.RefreshToken()
	if err != nil {
		t.Fatal(err)
	}
	b, bh, _, _ := m.RefreshToken()
	if a == b || ah == bh || len(ah) != 64 || HashRefreshToken(a) != ah {
		t.Fatal("invalid refresh token generation")
	}
}
