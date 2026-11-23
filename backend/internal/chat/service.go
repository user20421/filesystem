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
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/proto/impb"
)

const protocolVersion = 2

type Service struct{ db *sql.DB }

func NewService(db *sql.DB) *Service { return &Service{db: db} }

type Saved struct {
	Envelope  *impb.Envelope
	Cursor    uint64
	Duplicate bool
}

const (
	noRowsRetryAttempts = 3
	noRowsRetryDelay    = 5 * time.Millisecond
)

// retryNoRows handles the transient insert-then-select visibility failures
// seen under load. All other SQL errors are returned immediately.
func retryNoRows(ctx context.Context, operation func() error) error {
	for attempt := 0; attempt < noRowsRetryAttempts; attempt++ {
		err := operation()
		if !errors.Is(err, sql.ErrNoRows) || attempt+1 == noRowsRetryAttempts {
			return err
		}
		timer := time.NewTimer(noRowsRetryDelay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
	return nil
}

func (s *Service) directConversation(ctx context.Context, sender, recipient string) (string, error) {
	if sender == recipient {
		return "", fmt.Errorf("recipient validation: %w", httpx.Error(400, "invalid_recipient", "cannot message yourself"))
	}
	if _, err := uuid.Parse(recipient); err != nil {
		return "", fmt.Errorf("recipient validation: %w", httpx.Error(400, "invalid_recipient", "recipient must be a user ID"))
	}
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	const conversationQuery = `SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`
	var id string
	err := s.db.QueryRowContext(ctx, conversationQuery, low, high).Scan(&id)
	if err == nil {
		// 已有会话直接返回，避免每条消息都开启创建事务及重复校验成员关系。
		return id, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", fmt.Errorf("conversation query: %w", err)
	}

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", fmt.Errorf("conversation transaction: %w", err)
	}
	defer tx.Rollback()
	var exists int
	err = tx.QueryRowContext(ctx, `SELECT COUNT(1) FROM users WHERE id=?`, recipient).Scan(&exists)
	if err != nil {
		return "", fmt.Errorf("recipient validation query: %w", err)
	}
	if exists != 1 {
		return "", fmt.Errorf("recipient validation: %w", httpx.Error(404, "recipient_not_found", "recipient not found"))
	}

	err = tx.QueryRowContext(ctx, conversationQuery, low, high).Scan(&id)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return "", fmt.Errorf("conversation query: %w", err)
	}
	if errors.Is(err, sql.ErrNoRows) {
		candidate := uuid.NewString()
		if _, err = tx.ExecContext(ctx, `INSERT INTO conversations(id,user_low_id,user_high_id) VALUES(?,?,?) ON DUPLICATE KEY UPDATE id=id`, candidate, low, high); err != nil {
			return "", fmt.Errorf("conversation create: %w", err)
		}
		// A normal SELECT can keep the transaction's earlier REPEATABLE READ
		// snapshot after a concurrent INSERT wins the unique-key race. A current
		// locking read observes that committed winner; bounded retries cover the
		// remaining transient visibility window.
		err = retryNoRows(ctx, func() error {
			return tx.QueryRowContext(ctx, conversationQuery+` FOR UPDATE`, low, high).Scan(&id)
		})
		if err != nil {
			return "", fmt.Errorf("conversation query after create: %w", err)
		}
	}
	for _, userID := range []string{low, high} {
		if _, err := tx.ExecContext(ctx, `INSERT IGNORE INTO conversation_members(conversation_id,user_id) VALUES(?,?)`, id, userID); err != nil {
			return "", fmt.Errorf("conversation membership create: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return "", fmt.Errorf("conversation commit: %w", err)
	}
	return id, nil
}
func validateMessage(e *impb.Envelope, sender string) error {
	if e.Version != protocolVersion || e.Type != impb.Envelope_MESSAGE {
		return httpx.Error(400, "invalid_envelope", "only version 2 MESSAGE envelopes are accepted")
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
	var payload impb.MessagePayload
	if err := proto.Unmarshal(e.Payload, &payload); err != nil {
		return httpx.Error(400, "invalid_payload", "payload must be a valid MessagePayload")
	}
	switch content := payload.Content.(type) {
	case *impb.MessagePayload_Text:
		if content.Text == "" {
			return httpx.Error(400, "invalid_payload", "text payload must not be empty")
		}
	case *impb.MessagePayload_File:
		if content.File == nil || content.File.FileId == "" || content.File.Filename == "" || content.File.Size < 0 {
			return httpx.Error(400, "invalid_payload", "file payload requires file_id, filename and a non-negative size")
		}
	default:
		return httpx.Error(400, "invalid_payload", "payload must contain text or a file notification")
	}
	return nil
}

// SaveMessage 校验并持久化消息；相同发送者和 message_id 的相同内容幂等返回原游标，内容不同则拒绝冲突。
func (s *Service) SaveMessage(ctx context.Context, sender string, input *impb.Envelope) (Saved, error) {
	if err := validateMessage(input, sender); err != nil {
		return Saved{}, fmt.Errorf("message validation: %w", err)
	}
	conversation, err := s.directConversation(ctx, sender, input.RecipientId)
	if err != nil {
		return Saved{}, fmt.Errorf("conversation query/create: %w", err)
	}
	now := time.Now().UTC()
	result, err := s.db.ExecContext(ctx, `INSERT INTO messages(message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at) VALUES(?,?,?,?,?,?,?)`, input.MessageId, conversation, sender, input.RecipientId, input.DeviceId, input.Payload, now)
	if err == nil {
		cursor, _ := result.LastInsertId()
		return Saved{Envelope: &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: input.MessageId, ConversationId: conversation, SenderId: sender, RecipientId: input.RecipientId, DeviceId: input.DeviceId, Timestamp: now.UnixMilli(), Payload: input.Payload}, Cursor: uint64(cursor)}, nil
	}
	var mysqlErr *mysql.MySQLError
	if !errors.As(err, &mysqlErr) || mysqlErr.Number != 1062 {
		return Saved{}, fmt.Errorf("message insert: %w", err)
	}
	var saved Saved
	saved.Duplicate = true
	var created time.Time
	saved.Envelope = &impb.Envelope{}
	e := saved.Envelope
	err = retryNoRows(ctx, func() error {
		return s.db.QueryRowContext(ctx, `SELECT id,conversation_id,recipient_id,device_id,payload,created_at FROM messages WHERE sender_id=? AND message_id=?`, sender, input.MessageId).Scan(&saved.Cursor, &e.ConversationId, &e.RecipientId, &e.DeviceId, &e.Payload, &created)
	})
	if err != nil {
		return Saved{}, fmt.Errorf("message duplicate query: %w", err)
	}
	if e.ConversationId != conversation || e.RecipientId != input.RecipientId || e.DeviceId != input.DeviceId || !bytes.Equal(e.Payload, input.Payload) {
		return Saved{}, fmt.Errorf("message duplicate validation: %w", httpx.Error(409, "message_id_conflict", "message_id was already used with different content"))
	}
	e.Version = protocolVersion
	e.Type = impb.Envelope_MESSAGE
	e.MessageId = input.MessageId
	e.SenderId = sender
	e.Timestamp = created.UnixMilli()
	return saved, nil
}
func Ack(saved Saved) *impb.Envelope {
	payload, _ := proto.Marshal(&impb.AckPayload{Cursor: saved.Cursor})
	return &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_ACK, MessageId: saved.Envelope.MessageId, ConversationId: saved.Envelope.ConversationId, SenderId: saved.Envelope.SenderId, RecipientId: saved.Envelope.RecipientId, Timestamp: time.Now().UnixMilli(), Payload: payload}
}
