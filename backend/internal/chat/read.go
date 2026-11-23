package chat

import (
	"context"
	"errors"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

// ReadReceipt 描述读取方新确认的一段传入消息。
type ReadReceipt struct {
	ConversationID string `json:"conversation_id"`
	PeerID         string `json:"peer_id"`
	Cursor         uint64 `json:"cursor"`
	ReadAt         int64  `json:"read_at"`
	Updated        int64  `json:"updated"`
}

// MarkRead 将游标之前所有传入消息标记为已读；缺失和越权游标使用同一错误。
func (s *Service) MarkRead(ctx context.Context, readerID, peerID string, cursor uint64) (ReadReceipt, error) {
	if _, err := uuid.Parse(peerID); err != nil || peerID == readerID {
		return ReadReceipt{}, apperror.New(apperror.KindBadRequest, "invalid_peer", "peer_id must be another user ID")
	}
	if cursor == 0 {
		return ReadReceipt{}, apperror.New(apperror.KindBadRequest, "invalid_cursor", "cursor must be greater than zero")
	}
	receipt, err := s.repo.MarkRead(ctx, readerID, peerID, cursor)
	var notFound *readCursorNotFoundError
	if errors.As(err, &notFound) {
		return ReadReceipt{}, apperror.New(apperror.KindForbidden, "read_forbidden", "message cannot be marked as read")
	}
	return receipt, err
}
