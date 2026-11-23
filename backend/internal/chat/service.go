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
	"secure-im/backend/internal/apperror"
	"secure-im/backend/internal/proto/impb"
)

const protocolVersion = 2

// Service 负责聊天业务校验，SQL 与事务由窄 repository 执行。
type Service struct{ repo repository }

func NewService(db *sql.DB) *Service { return &Service{repo: &sqlRepository{db: db}} }

type Saved struct {
	Envelope  *impb.Envelope
	Cursor    uint64
	Duplicate bool
}

const (
	noRowsRetryAttempts = 3
	noRowsRetryDelay    = 5 * time.Millisecond
)

// retryNoRows 只重试瞬时 sql.ErrNoRows，其他 SQL 错误立即返回。
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
		return "", fmt.Errorf("recipient validation: %w", apperror.New(apperror.KindBadRequest, "invalid_recipient", "cannot message yourself"))
	}
	if _, err := uuid.Parse(recipient); err != nil {
		return "", fmt.Errorf("recipient validation: %w", apperror.New(apperror.KindBadRequest, "invalid_recipient", "recipient must be a user ID"))
	}
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	return s.repo.DirectConversation(ctx, recipient, low, high)
}

func validateMessage(e *impb.Envelope, sender string) error {
	if e.Version != protocolVersion || e.Type != impb.Envelope_MESSAGE {
		return apperror.New(apperror.KindBadRequest, "invalid_envelope", "only version 2 MESSAGE envelopes are accepted")
	}
	if len(e.MessageId) < 1 || len(e.MessageId) > 64 {
		return apperror.New(apperror.KindBadRequest, "invalid_message_id", "message_id must contain 1-64 characters")
	}
	if len(e.DeviceId) < 1 || len(e.DeviceId) > 128 {
		return apperror.New(apperror.KindBadRequest, "invalid_device_id", "device_id must contain 1-128 characters")
	}
	if e.SenderId != "" && e.SenderId != sender {
		return apperror.New(apperror.KindForbidden, "sender_mismatch", "sender_id does not match authenticated user")
	}
	if len(e.Payload) == 0 || len(e.Payload) > 256<<10 {
		return apperror.New(apperror.KindBadRequest, "invalid_payload", "payload must contain 1 byte to 256KiB")
	}
	var payload impb.MessagePayload
	if err := proto.Unmarshal(e.Payload, &payload); err != nil {
		return apperror.New(apperror.KindBadRequest, "invalid_payload", "payload must be a valid MessagePayload")
	}
	switch content := payload.Content.(type) {
	case *impb.MessagePayload_Text:
		if content.Text == "" {
			return apperror.New(apperror.KindBadRequest, "invalid_payload", "text payload must not be empty")
		}
	case *impb.MessagePayload_File:
		if content.File == nil || content.File.FileId == "" || content.File.Filename == "" || content.File.Size < 0 {
			return apperror.New(apperror.KindBadRequest, "invalid_payload", "file payload requires file_id, filename and a non-negative size")
		}
	default:
		return apperror.New(apperror.KindBadRequest, "invalid_payload", "payload must contain text or a file notification")
	}
	return nil
}

// SaveMessage 校验并持久化消息；相同发送者和 message_id 的相同内容幂等返回原游标。
func (s *Service) SaveMessage(ctx context.Context, sender string, input *impb.Envelope) (Saved, error) {
	if err := validateMessage(input, sender); err != nil {
		return Saved{}, fmt.Errorf("message validation: %w", err)
	}
	conversation, err := s.directConversation(ctx, sender, input.RecipientId)
	if err != nil {
		return Saved{}, fmt.Errorf("conversation query/create: %w", err)
	}
	now := time.Now().UTC()
	cursor, err := s.repo.InsertMessage(ctx, input, conversation, sender, now)
	if err == nil {
		return Saved{Envelope: &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: input.MessageId, ConversationId: conversation, SenderId: sender, RecipientId: input.RecipientId, DeviceId: input.DeviceId, Timestamp: now.UnixMilli(), Payload: input.Payload}, Cursor: uint64(cursor)}, nil
	}
	var mysqlErr *mysql.MySQLError
	if !errors.As(err, &mysqlErr) || mysqlErr.Number != 1062 {
		return Saved{}, fmt.Errorf("message insert: %w", err)
	}
	saved, created, err := s.repo.DuplicateMessage(ctx, sender, input.MessageId)
	if err != nil {
		return Saved{}, fmt.Errorf("message duplicate query: %w", err)
	}
	e := saved.Envelope
	if e.ConversationId != conversation || e.RecipientId != input.RecipientId || e.DeviceId != input.DeviceId || !bytes.Equal(e.Payload, input.Payload) {
		return Saved{}, fmt.Errorf("message duplicate validation: %w", apperror.New(apperror.KindConflict, "message_id_conflict", "message_id was already used with different content"))
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
