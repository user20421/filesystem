package chat

import (
	"context"
	"database/sql"
	"errors"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/go-sql-driver/mysql"
	"github.com/google/uuid"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

func TestValidateMessagePayload(t *testing.T) {
	text, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{Text: "hello"}})
	if err != nil {
		t.Fatal(err)
	}
	file, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_File{File: &impb.FileNotification{
		FileId: "file-1", Filename: "hello.txt", Size: 5,
	}}})
	if err != nil {
		t.Fatal(err)
	}
	valid := func(payload []byte) *impb.Envelope {
		return &impb.Envelope{
			Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: "message-1",
			DeviceId: "desktop", SenderId: "sender", Payload: payload,
		}
	}
	for name, payload := range map[string][]byte{"text": text, "file": file} {
		t.Run(name, func(t *testing.T) {
			if err := validateMessage(valid(payload), "sender"); err != nil {
				t.Fatalf("valid payload rejected: %v", err)
			}
		})
	}

	emptyText, _ := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{}})
	missingFileID, _ := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_File{File: &impb.FileNotification{
		Filename: "hello.txt", Size: 5,
	}}})
	emptyBody, _ := proto.Marshal(&impb.MessagePayload{})
	for name, payload := range map[string][]byte{
		"plain text":      []byte("hello"),
		"legacy JSON":     []byte(`{"type":"text","text":"hello"}`),
		"invalid wire":    {0xff},
		"empty body":      emptyBody,
		"empty text":      emptyText,
		"missing file id": missingFileID,
	} {
		t.Run(name, func(t *testing.T) {
			if err := validateMessage(valid(payload), "sender"); err == nil {
				t.Fatal("invalid payload accepted")
			}
		})
	}

	legacy := valid(text)
	legacy.Version = 1
	if err := validateMessage(legacy, "sender"); err == nil {
		t.Fatal("version 1 envelope accepted")
	}
}

func TestAckUsesProtobufPayload(t *testing.T) {
	ack := Ack(Saved{Envelope: &impb.Envelope{MessageId: "message-1"}, Cursor: 42})
	if ack.Version != protocolVersion || ack.Type != impb.Envelope_ACK || ack.Cursor != 0 {
		t.Fatalf("unexpected ACK envelope: %v", ack)
	}
	var payload impb.AckPayload
	if err := proto.Unmarshal(ack.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Cursor != 42 {
		t.Fatalf("ACK cursor = %d, want 42", payload.Cursor)
	}
}

func TestErrorUsesProtobufPayload(t *testing.T) {
	c := &client{send: make(chan *impb.Envelope, 1), done: make(chan struct{})}
	(&Handler{}).sendError(c, "invalid_payload", "message rejected", "message-1")
	envelope := <-c.send
	if envelope.Version != protocolVersion || envelope.Type != impb.Envelope_ERROR || envelope.MessageId != "message-1" {
		t.Fatalf("unexpected ERROR envelope: %v", envelope)
	}
	var payload impb.ErrorPayload
	if err := proto.Unmarshal(envelope.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Code != "invalid_payload" || payload.Message != "message rejected" {
		t.Fatalf("unexpected ERROR payload: %v", &payload)
	}
}

func TestRetryNoRowsConcurrent(t *testing.T) {
	const workers = 32
	var wg sync.WaitGroup
	errs := make(chan error, workers)
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			var attempts atomic.Int32
			err := retryNoRows(context.Background(), func() error {
				if attempts.Add(1) == 1 {
					return sql.ErrNoRows
				}
				return nil
			})
			if err != nil {
				errs <- err
				return
			}
			if got := attempts.Load(); got != 2 {
				errs <- errors.New("retry did not perform exactly two attempts")
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
}

func TestRetryNoRowsIsBoundedAndDoesNotHideSQLErrors(t *testing.T) {
	attempts := 0
	err := retryNoRows(context.Background(), func() error {
		attempts++
		return sql.ErrNoRows
	})
	if !errors.Is(err, sql.ErrNoRows) || attempts != noRowsRetryAttempts {
		t.Fatalf("no-rows retry: err=%v attempts=%d", err, attempts)
	}

	sqlErr := errors.New("connection reset")
	attempts = 0
	err = retryNoRows(context.Background(), func() error {
		attempts++
		return sqlErr
	})
	if !errors.Is(err, sqlErr) || attempts != 1 {
		t.Fatalf("SQL error was hidden or retried: err=%v attempts=%d", err, attempts)
	}
}

func TestDirectConversationExistingFastPath(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	sender, recipient, conversation := uuid.NewString(), uuid.NewString(), uuid.NewString()
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`)).
		WithArgs(low, high).WillReturnRows(sqlmock.NewRows([]string{"id"}).AddRow(conversation))

	got, err := NewService(db).directConversation(context.Background(), sender, recipient)
	if err != nil || got != conversation {
		t.Fatalf("directConversation = %q, %v; want %q", got, err, conversation)
	}
	// The single SELECT expectation also proves there was no transaction,
	// recipient query, or conversation_members write on the hot path.
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestDirectConversationRetriesQueryAfterCreate(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	sender, recipient, conversation := uuid.NewString(), uuid.NewString(), uuid.NewString()
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	const query = `SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`
	mock.ExpectQuery(regexp.QuoteMeta(query)).WithArgs(low, high).WillReturnError(sql.ErrNoRows)
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT COUNT(1) FROM users WHERE id=?`)).WithArgs(recipient).
		WillReturnRows(sqlmock.NewRows([]string{"count"}).AddRow(1))
	mock.ExpectQuery(regexp.QuoteMeta(query)).WithArgs(low, high).WillReturnError(sql.ErrNoRows)
	mock.ExpectExec(regexp.QuoteMeta(`INSERT INTO conversations(id,user_low_id,user_high_id) VALUES(?,?,?) ON DUPLICATE KEY UPDATE id=id`)).
		WithArgs(sqlmock.AnyArg(), low, high).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectQuery(regexp.QuoteMeta(query+` FOR UPDATE`)).WithArgs(low, high).WillReturnError(sql.ErrNoRows)
	mock.ExpectQuery(regexp.QuoteMeta(query+` FOR UPDATE`)).WithArgs(low, high).
		WillReturnRows(sqlmock.NewRows([]string{"id"}).AddRow(conversation))
	for _, userID := range []string{low, high} {
		mock.ExpectExec(regexp.QuoteMeta(`INSERT IGNORE INTO conversation_members(conversation_id,user_id) VALUES(?,?)`)).
			WithArgs(conversation, userID).WillReturnResult(sqlmock.NewResult(0, 1))
	}
	mock.ExpectCommit()

	got, err := NewService(db).directConversation(context.Background(), sender, recipient)
	if err != nil || got != conversation {
		t.Fatalf("directConversation = %q, %v; want %q", got, err, conversation)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func testMessageEnvelope(t *testing.T, sender, recipient string) *impb.Envelope {
	t.Helper()
	payload, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{Text: "hello"}})
	if err != nil {
		t.Fatal(err)
	}
	return &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: "message-1",
		DeviceId: "desktop", SenderId: sender, RecipientId: recipient, Payload: payload}
}

func expectExistingConversation(mock sqlmock.Sqlmock, sender, recipient, conversation string) {
	low, high := sender, recipient
	if low > high {
		low, high = high, low
	}
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT id FROM conversations WHERE user_low_id=? AND user_high_id=?`)).WithArgs(low, high).
		WillReturnRows(sqlmock.NewRows([]string{"id"}).AddRow(conversation))
}

func expectDuplicateMessageInsert(mock sqlmock.Sqlmock, input *impb.Envelope, conversation string) {
	mock.ExpectExec(regexp.QuoteMeta(`INSERT INTO messages(message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at) VALUES(?,?,?,?,?,?,?)`)).
		WithArgs(input.MessageId, conversation, input.SenderId, input.RecipientId, input.DeviceId, input.Payload, sqlmock.AnyArg()).
		WillReturnError(&mysql.MySQLError{Number: 1062, Message: "duplicate entry"})
}

const duplicateMessageQuery = `SELECT id,conversation_id,recipient_id,device_id,payload,created_at FROM messages WHERE sender_id=? AND message_id=?`

func TestSaveMessageRetriesDuplicateLookup(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	sender, recipient, conversation := uuid.NewString(), uuid.NewString(), uuid.NewString()
	input := testMessageEnvelope(t, sender, recipient)
	expectExistingConversation(mock, sender, recipient, conversation)
	expectDuplicateMessageInsert(mock, input, conversation)
	mock.ExpectQuery(regexp.QuoteMeta(duplicateMessageQuery)).WithArgs(sender, input.MessageId).WillReturnError(sql.ErrNoRows)
	created := time.Now()
	mock.ExpectQuery(regexp.QuoteMeta(duplicateMessageQuery)).WithArgs(sender, input.MessageId).
		WillReturnRows(sqlmock.NewRows(strings.Fields("id conversation_id recipient_id device_id payload created_at")).
			AddRow(42, conversation, recipient, input.DeviceId, input.Payload, created))

	saved, err := NewService(db).SaveMessage(context.Background(), sender, input)
	if err != nil || !saved.Duplicate || saved.Cursor != 42 || saved.Envelope.MessageId != input.MessageId {
		t.Fatalf("duplicate SaveMessage = %+v, %v", saved, err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestSaveMessageDuplicateLookupIsBoundedAndStaged(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	sender, recipient, conversation := uuid.NewString(), uuid.NewString(), uuid.NewString()
	input := testMessageEnvelope(t, sender, recipient)
	expectExistingConversation(mock, sender, recipient, conversation)
	expectDuplicateMessageInsert(mock, input, conversation)
	for i := 0; i < noRowsRetryAttempts; i++ {
		mock.ExpectQuery(regexp.QuoteMeta(duplicateMessageQuery)).WithArgs(sender, input.MessageId).WillReturnError(sql.ErrNoRows)
	}

	_, err = NewService(db).SaveMessage(context.Background(), sender, input)
	if !errors.Is(err, sql.ErrNoRows) || !strings.Contains(err.Error(), "message duplicate query") {
		t.Fatalf("duplicate lookup error = %v", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
