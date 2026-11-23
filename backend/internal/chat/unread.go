package chat

import (
	"context"
	"time"
)

// UnreadSummary aggregates one sender's unread messages for the current user.
// UnreadSummary 按发送者聚合当前用户的未读数量，last_message_at 使用 Unix 毫秒，便于登录后一次恢复红点。
type UnreadSummary struct {
	PeerID        string `json:"peer_id"`
	UnreadCount   int64  `json:"unread_count"`
	LastMessageAt int64  `json:"last_message_at"`
}

// Unread returns all unread incoming messages in one grouped query.
func (s *Service) Unread(ctx context.Context, recipientID string) ([]UnreadSummary, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT sender_id,COUNT(*),MAX(created_at) FROM messages WHERE recipient_id=? AND read_at IS NULL GROUP BY sender_id ORDER BY sender_id`, recipientID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]UnreadSummary, 0)
	for rows.Next() {
		var item UnreadSummary
		var lastMessage time.Time
		if err := rows.Scan(&item.PeerID, &item.UnreadCount, &lastMessage); err != nil {
			return nil, err
		}
		item.LastMessageAt = lastMessage.UnixMilli()
		items = append(items, item)
	}
	return items, rows.Err()
}
