package chat

import (
	"sort"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

type client struct {
	conn *websocket.Conn
	send chan *impb.Envelope
	done chan struct{}
	once sync.Once
}

func (c *client) close() {
	c.once.Do(func() {
		close(c.done)
		if c.conn != nil {
			_ = c.conn.Close()
		}
	})
}

// enqueue 将消息放入有界发送队列；慢消费者占满队列时立即断开，避免单个连接拖垮整个 Hub。
func (c *client) enqueue(e *impb.Envelope) bool {
	select {
	case c.send <- e:
		return true
	default:
		c.close()
		return false
	}
}

// Hub 是单进程在线连接表，每用户最多允许 5 条连接；离线消息不在这里补发，而是由数据库历史负责。
type Hub struct {
	mu    sync.RWMutex
	users map[string]map[*client]struct{}
}

func NewHub() *Hub { return &Hub{users: make(map[string]map[*client]struct{})} }
func (h *Hub) add(userID string, c *client) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	set := h.users[userID]
	if set == nil {
		set = make(map[*client]struct{})
		h.users[userID] = set
	}
	if len(set) >= 5 {
		return false
	}
	set[c] = struct{}{}
	return true
}
func (h *Hub) remove(userID string, c *client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.users[userID], c)
	if len(h.users[userID]) == 0 {
		delete(h.users, userID)
	}
}

// OnlineUsers 返回按 ID 排序的在线用户快照，便于界面与压测稳定读取。
func (h *Hub) OnlineUsers() []string {
	h.mu.RLock()
	users := make([]string, 0, len(h.users))
	for userID, connections := range h.users {
		if len(connections) > 0 {
			users = append(users, userID)
		}
	}
	h.mu.RUnlock()
	sort.Strings(users)
	return users
}

func (h *Hub) IsOnline(userID string) bool {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return len(h.users[userID]) > 0
}

// CloseUser 只移除并关闭指定用户的全部连接，不影响其他在线用户。
func (h *Hub) CloseUser(userID string) {
	h.mu.Lock()
	targets := make([]*client, 0, len(h.users[userID]))
	for c := range h.users[userID] {
		targets = append(targets, c)
	}
	delete(h.users, userID)
	h.mu.Unlock()
	for _, c := range targets {
		c.close()
	}
}

func (h *Hub) CloseAll() {
	h.mu.RLock()
	targets := make([]*client, 0)
	for _, set := range h.users {
		for c := range set {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.close()
	}
}

// Publish 仅向接收者当前在线连接推送消息，未在线时依赖数据库历史补拉。
func (h *Hub) Publish(userID string, e *impb.Envelope) {
	h.mu.RLock()
	targets := make([]*client, 0, len(h.users[userID]))
	for c := range h.users[userID] {
		targets = append(targets, c)
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.enqueue(e)
	}
}
func (c *client) writer() {
	ticker := time.NewTicker(25 * time.Second)
	defer ticker.Stop()
	defer c.close()
	for {
		select {
		case <-c.done:
			return
		case e := <-c.send:
			_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			data, err := proto.Marshal(e)
			if err != nil {
				return
			}
			if err := c.conn.WriteMessage(websocket.BinaryMessage, data); err != nil {
				return
			}
		case <-ticker.C:
			_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}
