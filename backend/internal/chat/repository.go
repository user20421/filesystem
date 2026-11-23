package chat

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
	"secure-im/backend/internal/proto/impb"
)

// repository 是聊天垂直包的窄持久化接口，保留会话、消息和已读事务边界。
type repository interface {
	DirectConversation(context.Context, string, string, string) (string, error)
	InsertMessage(context.Context, *impb.Envelope, string, string, time.Time) (int64, error)
	DuplicateMessage(context.Context, string, string) (Saved, time.Time, error)
	History(context.Context, string, string, uint64, int, func(historyRecord) error) error
	MarkRead(context.Context, string, string, uint64) (ReadReceipt, error)
	Unread(context.Context, string) ([]UnreadSummary, error)
}

type sqlRepository struct{ db *sql.DB }

type historyRecord struct {
	cursor   uint64
	envelope *impb.Envelope
	created  time.Time
	readAt   sql.NullTime
}

// DirectConversation 保留创建会话时的行锁、可见性重试和成员写入顺序。
func (r *sqlRepository) DirectConversation(ctx context.Context, recipient, low, high string) (string, error) {
	const conversationQuery = `SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`
	var id string
	err := r.db.QueryRowContext(ctx, conversationQuery, low, high).Scan(&id)
	if err == nil {
		return id, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", fmt.Errorf("conversation query: %w", err)
	}

	tx, err := r.db.BeginTx(ctx, nil)
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
		return "", fmt.Errorf("recipient validation: %w", apperror.New(apperror.KindNotFound, "recipient_not_found", "recipient not found"))
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

func (r *sqlRepository) InsertMessage(ctx context.Context, input *impb.Envelope, conversation, sender string, now time.Time) (int64, error) {
	result, err := r.db.ExecContext(ctx, `INSERT INTO messages(message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at) VALUES(?,?,?,?,?,?,?)`, input.MessageId, conversation, sender, input.RecipientId, input.DeviceId, input.Payload, now)
	if err != nil {
		return 0, err
	}
	cursor, _ := result.LastInsertId()
	return cursor, nil
}

func (r *sqlRepository) DuplicateMessage(ctx context.Context, sender, messageID string) (Saved, time.Time, error) {
	var saved Saved
	var created time.Time
	saved.Duplicate = true
	saved.Envelope = &impb.Envelope{}
	e := saved.Envelope
	err := retryNoRows(ctx, func() error {
		return r.db.QueryRowContext(ctx, `SELECT id,conversation_id,recipient_id,device_id,payload,created_at FROM messages WHERE sender_id=? AND message_id=?`, sender, messageID).Scan(&saved.Cursor, &e.ConversationId, &e.RecipientId, &e.DeviceId, &e.Payload, &created)
	})
	return saved, created, err
}

// History 逐行扫描并同步交付，原始 Payload 不会随整批历史记录一起保留。
func (r *sqlRepository) History(ctx context.Context, userID, peer string, after uint64, limit int, consume func(historyRecord) error) error {
	rows, err := r.db.QueryContext(ctx, `SELECT id,message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at,read_at FROM messages WHERE ((sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)) AND id>? ORDER BY id ASC LIMIT ?`, userID, peer, peer, userID, after, limit)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		record := historyRecord{envelope: &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE}}
		if err := rows.Scan(&record.cursor, &record.envelope.MessageId, &record.envelope.ConversationId, &record.envelope.SenderId, &record.envelope.RecipientId, &record.envelope.DeviceId, &record.envelope.Payload, &record.created, &record.readAt); err != nil {
			return err
		}
		if err := consume(record); err != nil {
			return err
		}
	}
	return rows.Err()
}

// readCursorNotFoundError 只标记第一次游标/参与者查询的缺失，避免误映射后续内部查询。
type readCursorNotFoundError struct{ err error }

func (e *readCursorNotFoundError) Error() string { return e.err.Error() }
func (e *readCursorNotFoundError) Unwrap() error { return e.err }

// MarkRead 在同一事务中锁定游标消息、批量标记并读取最终 read_at。
func (r *sqlRepository) MarkRead(ctx context.Context, readerID, peerID string, cursor uint64) (ReadReceipt, error) {
	tx, err := r.db.BeginTx(ctx, nil)
	if err != nil {
		return ReadReceipt{}, err
	}
	defer tx.Rollback()

	var conversationID string
	var readAt sql.NullTime
	err = tx.QueryRowContext(ctx, `SELECT conversation_id,read_at FROM messages WHERE id=? AND sender_id=? AND recipient_id=? FOR UPDATE`, cursor, peerID, readerID).Scan(&conversationID, &readAt)
	if errors.Is(err, sql.ErrNoRows) {
		return ReadReceipt{}, &readCursorNotFoundError{err: err}
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
	return ReadReceipt{ConversationID: conversationID, PeerID: peerID, Cursor: cursor, ReadAt: readAt.Time.UnixMilli(), Updated: updated}, nil
}

func (r *sqlRepository) Unread(ctx context.Context, recipientID string) ([]UnreadSummary, error) {
	rows, err := r.db.QueryContext(ctx, `SELECT sender_id,COUNT(*),MAX(created_at) FROM messages WHERE recipient_id=? AND read_at IS NULL GROUP BY sender_id ORDER BY sender_id`, recipientID)
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
