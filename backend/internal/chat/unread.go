package chat

import "context"

// UnreadSummary 按发送者聚合当前用户的未读数量，last_message_at 使用 Unix 毫秒。
type UnreadSummary struct {
	PeerID        string `json:"peer_id"`
	UnreadCount   int64  `json:"unread_count"`
	LastMessageAt int64  `json:"last_message_at"`
}

// Unread 通过一次分组查询返回所有传入消息的未读摘要。
func (s *Service) Unread(ctx context.Context, recipientID string) ([]UnreadSummary, error) {
	return s.repo.Unread(ctx, recipientID)
}
