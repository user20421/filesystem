package chat

import (
	"context"
	"database/sql"
	"errors"

	"github.com/google/uuid"
	"secure-im/backend/internal/httpx"
)

// ReadReceipt describes the incoming messages newly acknowledged by a reader.
type ReadReceipt struct {
	ConversationID string `json:"conversation_id"`
	PeerID         string `json:"peer_id"`
	Cursor         uint64 `json:"cursor"`
	ReadAt         int64  `json:"read_at"`
	Updated        int64  `json:"updated"`
}

// MarkRead marks all incoming messages through cursor as read exactly once.
// A cursor is usable only for a message sent by peer to the authenticated reader.
func (s *Service) MarkRead(ctx context.Context, readerID, peerID string, cursor uint64) (ReadReceipt, error) {
	if _, err := uuid.Parse(peerID); err != nil || peerID == readerID {
		return ReadReceipt{}, httpx.Error(400, "invalid_peer", "peer_id must be another user ID")
	}
	if cursor == 0 {
		return ReadReceipt{}, httpx.Error(400, "invalid_cursor", "cursor must be greater than zero")
	}

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ReadReceipt{}, err
	}
	defer tx.Rollback()

	var conversationID string
	var readAt sql.NullTime
	err = tx.QueryRowContext(ctx, `SELECT conversation_id,read_at FROM messages WHERE id=? AND sender_id=? AND recipient_id=? FOR UPDATE`, cursor, peerID, readerID).Scan(&conversationID, &readAt)
	if errors.Is(err, sql.ErrNoRows) {
		// Use the same response for missing, unrelated and foreign cursors.
		return ReadReceipt{}, httpx.Error(403, "read_forbidden", "message cannot be marked as read")
	}
	if err != nil {
		return ReadReceipt{}, err
	}

	result, err := tx.ExecContext(ctx, `UPDATE messages SET read_at=NOW(6) WHERE conversation_id=? AND sender_id=? AND recipient_id=? AND id<=? AND read_at IS NULL`, conversationID, peerID, readerID, cursor)
	if err != nil {
		return ReadReceipt{}, err
	}
	updated, err := result.RowsAffected()
	if err != nil {
		return ReadReceipt{}, err
	}
	if !readAt.Valid {
		if err := tx.QueryRowContext(ctx, `SELECT read_at FROM messages WHERE id=?`, cursor).Scan(&readAt); err != nil {
			return ReadReceipt{}, err
		}
	}
	if err := tx.Commit(); err != nil {
		return ReadReceipt{}, err
	}
	return ReadReceipt{
		ConversationID: conversationID,
		PeerID:         peerID,
		Cursor:         cursor,
		ReadAt:         readAt.Time.UnixMilli(),
		Updated:        updated,
	}, nil
}
