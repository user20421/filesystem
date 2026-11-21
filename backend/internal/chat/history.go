package chat

import (
	"context"
	"database/sql"
	"encoding/base64"
	"time"

	"github.com/google/uuid"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/proto/impb"
)

type HistoryItem struct {
	Cursor   uint64 `json:"cursor"`
	Envelope string `json:"envelope"`
}

func (s *Service) History(ctx context.Context, userID, peer string, after uint64, limit int) ([]HistoryItem, error) {
	if _, err := uuid.Parse(peer); err != nil || peer == userID {
		return nil, httpx.Error(400, "invalid_peer", "peer_id must be another user ID")
	}
	rows, err := s.db.QueryContext(ctx, `SELECT id,message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at,read_at FROM messages WHERE ((sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)) AND id>? ORDER BY id ASC LIMIT ?`, userID, peer, peer, userID, after, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]HistoryItem, 0)
	for rows.Next() {
		var item HistoryItem
		var created time.Time
		var readAt sql.NullTime
		e := &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE}
		if err := rows.Scan(&item.Cursor, &e.MessageId, &e.ConversationId, &e.SenderId, &e.RecipientId, &e.DeviceId, &e.Payload, &created, &readAt); err != nil {
			return nil, err
		}
		e.Timestamp = created.UnixMilli()
		if readAt.Valid {
			e.ReadAt = readAt.Time.UnixMilli()
		}
		binary, err := proto.Marshal(e)
		if err != nil {
			return nil, err
		}
		item.Envelope = base64.StdEncoding.EncodeToString(binary)
		items = append(items, item)
	}
	return items, rows.Err()
}
