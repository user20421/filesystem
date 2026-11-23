package chat

import (
	"context"
	"encoding/base64"
	"errors"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

const historyQuery = `SELECT id,message_id,conversation_id,sender_id,recipient_id,device_id,payload,created_at,read_at FROM messages WHERE ((sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)) AND id>? ORDER BY id ASC LIMIT ?`

func historyRows() *sqlmock.Rows {
	return sqlmock.NewRows(strings.Fields("id message_id conversation_id sender_id recipient_id device_id payload created_at read_at"))
}

func TestSQLRepositoryHistoryStreamsBeforeReadingNextRow(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	rowFailure := errors.New("second row failed")
	created := time.Date(2026, 11, 23, 1, 2, 3, 0, time.UTC)
	rows := historyRows().
		AddRow(1, "message-1", "conversation", "sender", "recipient", "device", []byte("first payload"), created, nil).
		AddRow(2, "message-2", "conversation", "sender", "recipient", "device", []byte("second payload"), created, nil).
		RowError(1, rowFailure)
	mock.ExpectQuery(regexp.QuoteMeta(historyQuery)).WithArgs("reader", "peer", "peer", "reader", uint64(0), 500).WillReturnRows(rows)

	consumed := 0
	err = (&sqlRepository{db: db}).History(context.Background(), "reader", "peer", 0, 500, func(record historyRecord) error {
		consumed++
		if record.cursor != 1 || string(record.envelope.Payload) != "first payload" {
			t.Fatalf("unexpected first record: %+v", record)
		}
		return nil
	})
	if !errors.Is(err, rowFailure) {
		t.Fatalf("History error = %v, want second-row failure", err)
	}
	if consumed != 1 {
		t.Fatalf("consumed %d rows before second-row failure, want 1; rows must not be batched", consumed)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestHistoryEncodesEachRecordWithoutChangingResponse(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	reader, peer := uuid.NewString(), uuid.NewString()
	created := time.Date(2026, 11, 23, 1, 2, 3, 456000000, time.UTC)
	readAt := created.Add(time.Minute)
	mock.ExpectQuery(regexp.QuoteMeta(historyQuery)).WithArgs(reader, peer, peer, reader, uint64(7), 200).
		WillReturnRows(historyRows().AddRow(8, "message-1", "conversation", peer, reader, "device", []byte("payload"), created, readAt))

	items, err := NewService(db).History(context.Background(), reader, peer, 7, 200)
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 1 || items[0].Cursor != 8 {
		t.Fatalf("unexpected history items: %+v", items)
	}
	binary, err := base64.StdEncoding.DecodeString(items[0].Envelope)
	if err != nil {
		t.Fatal(err)
	}
	var got impb.Envelope
	if err := proto.Unmarshal(binary, &got); err != nil {
		t.Fatal(err)
	}
	want := &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: "message-1", ConversationId: "conversation", SenderId: peer, RecipientId: reader, DeviceId: "device", Timestamp: created.UnixMilli(), ReadAt: readAt.UnixMilli(), Payload: []byte("payload")}
	if !proto.Equal(&got, want) {
		t.Fatalf("decoded envelope = %v, want %v", &got, want)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
