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

// Claims 是访问令牌的身份声明，uid 必须与标准 Subject 一致。
type Claims struct {
	UserID string `json:"uid"`
	jwt.RegisteredClaims
}

// TokenManager 负责访问令牌签发校验和刷新令牌生成，使用 HS256 及固定签发方与受众。
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

// ParseAccessToken 校验签名、算法、签发方、受众、有效期及 uid/Subject 一致性，防止旧算法或跨受众令牌被误用。
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

// RefreshToken 生成返回给客户端的随机原始令牌，同时返回可安全落库的 SHA-256 哈希及过期时间。
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

// HashRefreshToken 对原始刷新令牌做 SHA-256，数据库只保存哈希而不是可直接使用的令牌。
func HashRefreshToken(raw string) string {
	sum := sha256.Sum256([]byte(raw))
	return fmt.Sprintf("%x", sum[:])
}
