package chat

import (
	"context"
	"encoding/base64"

	"github.com/google/uuid"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/apperror"
)

// HistoryItem 表示一条有序历史消息，Envelope 是 base64 编码的 Protobuf 二进制帧。
type HistoryItem struct {
	Cursor   uint64 `json:"cursor"`
	Envelope string `json:"envelope"`
}

func (s *Service) History(ctx context.Context, userID, peer string, after uint64, limit int) ([]HistoryItem, error) {
	if _, err := uuid.Parse(peer); err != nil || peer == userID {
		return nil, apperror.New(apperror.KindBadRequest, "invalid_peer", "peer_id must be another user ID")
	}
	items := make([]HistoryItem, 0)
	err := s.repo.History(ctx, userID, peer, after, limit, func(record historyRecord) error {
		e := record.envelope
		e.Timestamp = record.created.UnixMilli()
		if record.readAt.Valid {
			e.ReadAt = record.readAt.Time.UnixMilli()
		}
		binary, err := proto.Marshal(e)
		if err != nil {
			return err
		}
		items = append(items, HistoryItem{Cursor: record.cursor, Envelope: base64.StdEncoding.EncodeToString(binary)})
		return nil
	})
	if err != nil {
		return nil, err
	}
	return items, nil
}
