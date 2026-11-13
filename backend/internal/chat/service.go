package chat

import (
	"bytes"
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/go-sql-driver/mysql"
	"github.com/google/uuid"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/proto/impb"
)

type Service struct{ db *sql.DB }

func NewService(db *sql.DB) *Service { return &Service{db: db} }

type Saved struct {
	Envelope  *impb.Envelope
	Cursor    uint64
	Duplicate bool
}

func (s *Service) directConversation(ctx context.Context, sender, recipient string) (string, error) {
	if sender == recipient {
		return "", httpx.Error(400, "invalid_recipient", "cannot message yourself")
	}
	if _, err := uuid.Parse(recipient); err != nil {
		return "", httpx.Error(400, "invalid_recipient", "recipient must be a user ID")
	}
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	var exists int
	err = tx.QueryRowContext(ctx, `SELECT COUNT(1) FROM users WHERE id=?`, recipient).Scan(&exists)
	if err != nil {
		return "", err
	}
	if exists != 1 {
		return "", httpx.Error(404, "recipient_not_found", "recipient not found")
	}
	id := uuid.NewString()
	_, err = tx.ExecContext(ctx, `INSERT INTO conversations(id,user_low_id,user_high_id) VALUES(?,?,?) ON DUPLICATE KEY UPDATE id=id`, id, low, high)
	if err != nil {
		return "", err
	}
	if err := tx.QueryRowContext(ctx, `SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`, low, high).Scan(&id); err != nil {
		return "", err
	}
	for _, userID := range []string{low, high} {
		if _, err := tx.ExecContext(ctx, `INSERT IGNORE INTO conversation_members(conversation_id,user_id) VALUES(?,?)`, id, userID); err != nil {
			return "", err
		}
	}
	return id, tx.Commit()
}
func validateMessage(e *impb.Envelope, sender string) error {
	if e.Version != 1 || e.Type != impb.Envelope_MESSAGE {
		return httpx.Error(400, "invalid_envelope", "only version 1 MESSAGE envelopes are accepted")
	}
	if len(e.MessageId) < 1 || len(e.MessageId) > 64 {
		return httpx.Error(400, "invalid_message_id", "message_id must contain 1-64 characters")
	}
	if len(e.DeviceId) < 1 || len(e.DeviceId) > 128 {
		return httpx.Error(400, "invalid_device_id", "device_id must contain 1-128 characters")
	}
	if e.SenderId != "" && e.SenderId != sender {
		return httpx.Error(403, "sender_mismatch", "sender_id does not match authenticated user")
	}
	if len(e.Payload) == 0 || len(e.Payload) > 256<<10 {
		return httpx.Error(400, "invalid_payload", "payload must contain 1 byte to 256KiB")
	}
	return nil
}
func (s *Service) SaveMessage(ctx context.Context, sender string, input *impb.Envelope) (Saved, error) {
	if err := validateMessage(input, sender); err != nil {
		return Saved{}, err
	}
	conversation, err := s.directConversation(ctx, sender, input.RecipientId)
	if err != nil {
		return Saved{}, err
	}
	now := time.Now().UTC()
	result, err := s.db.ExecContext(ctx, `INSERT INTO messages(message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at) VALUES(?,?,?,?,?,?,?)`, input.MessageId, conversation, sender, input.RecipientId, input.DeviceId, input.Payload, now)
	if err == nil {
		cursor, _ := result.LastInsertId()
		return Saved{Envelope: &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: input.MessageId, ConversationId: conversation, SenderId: sender, RecipientId: input.RecipientId, DeviceId: input.DeviceId, Timestamp: now.UnixMilli(), Payload: input.Payload}, Cursor: uint64(cursor)}, nil
	}
	var mysqlErr *mysql.MySQLError
	if !errors.As(err, &mysqlErr) || mysqlErr.Number != 1062 {
		return Saved{}, err
	}
	var saved Saved
	saved.Duplicate = true
	var created time.Time
	saved.Envelope = &impb.Envelope{}
	e := saved.Envelope
	err = s.db.QueryRowContext(ctx, `SELECT id,conversation_id,recipient_id,device_id,payload,created_at FROM messages WHERE sender_id=? AND message_id=?`, sender, input.MessageId).Scan(&saved.Cursor, &e.ConversationId, &e.RecipientId, &e.DeviceId, &e.Payload, &created)
	if err != nil {
		return Saved{}, err
	}
	if e.ConversationId != conversation || e.RecipientId != input.RecipientId || e.DeviceId != input.DeviceId || !bytes.Equal(e.Payload, input.Payload) {
		return Saved{}, httpx.Error(409, "message_id_conflict", "message_id was already used with different content")
	}
	e.Version = 1
	e.Type = impb.Envelope_MESSAGE
	e.MessageId = input.MessageId
	e.SenderId = sender
	e.Timestamp = created.UnixMilli()
	return saved, nil
}
func Ack(saved Saved) *impb.Envelope {
	return &impb.Envelope{Version: 1, Type: impb.Envelope_ACK, MessageId: saved.Envelope.MessageId, ConversationId: saved.Envelope.ConversationId, SenderId: saved.Envelope.SenderId, RecipientId: saved.Envelope.RecipientId, Timestamp: time.Now().UnixMilli(), Payload: []byte(fmt.Sprintf("%d", saved.Cursor))}
}
