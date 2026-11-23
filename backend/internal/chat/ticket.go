package chat

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"sync"
	"time"
)

type ticket struct {
	userID  string
	expires time.Time
}

// TicketStore 在内存中保存一次性 WebSocket 票据的哈希，原始票据只通过 Issue 返回给客户端。
type TicketStore struct {
	mu      sync.Mutex
	ttl     time.Duration
	now     func() time.Time
	entries map[[32]byte]ticket
}

func NewTicketStore(ttl time.Duration) *TicketStore {
	return &TicketStore{ttl: ttl, now: time.Now, entries: make(map[[32]byte]ticket)}
}

// Issue 为指定用户签发短期票据，同时清理过期项并限制全局和单用户票据数量。
func (s *TicketStore) Issue(userID string) (string, error) {
	buf := make([]byte, 32)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	raw := base64.RawURLEncoding.EncodeToString(buf)
	key := sha256.Sum256([]byte(raw))
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	for k, v := range s.entries {
		if !v.expires.After(now) {
			delete(s.entries, k)
		}
	}
	count := 0
	for _, v := range s.entries {
		if v.userID == userID {
			count++
		}
	}
	if len(s.entries) >= 10000 || count >= 20 {
		return "", fmt.Errorf("WebSocket ticket limit reached")
	}
	s.entries[key] = ticket{userID: userID, expires: now.Add(s.ttl)}
	return raw, nil
}

// RevokeUser 使用户全部未消费票据失效，常用于退出、注销和账号安全处理。
func (s *TicketStore) RevokeUser(userID string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for key, entry := range s.entries {
		if entry.userID == userID {
			delete(s.entries, key)
		}
	}
}

// Consume 校验并立即删除票据，即使过期票据也只能尝试消费一次，防止重放建立连接。
func (s *TicketStore) Consume(raw string) (string, bool) {
	key := sha256.Sum256([]byte(raw))
	now := s.now()
	s.mu.Lock()
	defer s.mu.Unlock()
	v, ok := s.entries[key]
	if !ok {
		return "", false
	}
	delete(s.entries, key)
	if !v.expires.After(now) {
		return "", false
	}
	return v.userID, true
}
