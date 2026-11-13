package auth

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
)

type Claims struct {
	UserID string `json:"uid"`
	jwt.RegisteredClaims
}
type TokenManager struct {
	secret                []byte
	accessTTL, refreshTTL time.Duration
	now                   func() time.Time
}

func NewTokenManager(secret []byte, accessTTL, refreshTTL time.Duration) *TokenManager {
	return &TokenManager{secret: secret, accessTTL: accessTTL, refreshTTL: refreshTTL, now: time.Now}
}

func (m *TokenManager) AccessToken(userID string) (string, error) {
	now := m.now()
	claims := Claims{UserID: userID, RegisteredClaims: jwt.RegisteredClaims{ID: uuid.NewString(), Subject: userID, Issuer: "secure-im", Audience: jwt.ClaimStrings{"secure-im-api"}, IssuedAt: jwt.NewNumericDate(now), NotBefore: jwt.NewNumericDate(now), ExpiresAt: jwt.NewNumericDate(now.Add(m.accessTTL))}}
	return jwt.NewWithClaims(jwt.SigningMethodHS256, claims).SignedString(m.secret)
}
func (m *TokenManager) ParseAccessToken(raw string) (string, error) {
	var claims Claims
	token, err := jwt.ParseWithClaims(raw, &claims, func(t *jwt.Token) (any, error) {
		if t.Method != jwt.SigningMethodHS256 {
			return nil, fmt.Errorf("unexpected signing method")
		}
		return m.secret, nil
	}, jwt.WithAudience("secure-im-api"), jwt.WithIssuer("secure-im"), jwt.WithValidMethods([]string{"HS256"}))
	if err != nil || !token.Valid || claims.UserID == "" || claims.Subject != claims.UserID {
		return "", fmt.Errorf("invalid access token")
	}
	return claims.UserID, nil
}
func (m *TokenManager) RefreshToken() (raw string, hash string, expires time.Time, err error) {
	buf := make([]byte, 32)
	if _, err = rand.Read(buf); err != nil {
		return
	}
	raw = base64.RawURLEncoding.EncodeToString(buf)
	hash = HashRefreshToken(raw)
	expires = m.now().Add(m.refreshTTL)
	return
}
func HashRefreshToken(raw string) string {
	sum := sha256.Sum256([]byte(raw))
	return fmt.Sprintf("%x", sum[:])
}
