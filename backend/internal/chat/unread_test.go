package chat

import (
	"context"
	"testing"
	"time"

	"github.com/DATA-DOG/go-sqlmock"
)

func TestUnreadGroupsIncomingMessagesBySender(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	latestAlice := time.Date(2026, 11, 22, 10, 11, 12, 123456000, time.UTC)
	latestBob := latestAlice.Add(time.Minute)
	mock.ExpectQuery(`SELECT sender_id,COUNT\(\*\),MAX\(created_at\) FROM messages WHERE recipient_id=\? AND read_at IS NULL GROUP BY sender_id ORDER BY sender_id`).
		WithArgs("current-user").
		WillReturnRows(sqlmock.NewRows([]string{"sender_id", "unread_count", "last_message_at"}).
			AddRow("alice-id", int64(2), latestAlice).
			AddRow("bob-id", int64(1), latestBob))

	items, err := NewService(db).Unread(context.Background(), "current-user")
	if err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 {
		t.Fatalf("got %d summaries, want 2", len(items))
	}
	if items[0].PeerID != "alice-id" || items[0].UnreadCount != 2 || items[0].LastMessageAt != latestAlice.UnixMilli() {
		t.Fatalf("unexpected Alice summary: %+v", items[0])
	}
	if items[1].PeerID != "bob-id" || items[1].UnreadCount != 1 || items[1].LastMessageAt != latestBob.UnixMilli() {
		t.Fatalf("unexpected Bob summary: %+v", items[1])
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestUnreadReturnsEmptyArray(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	mock.ExpectQuery(`SELECT sender_id,COUNT\(\*\),MAX\(created_at\) FROM messages`).
		WithArgs("current-user").
		WillReturnRows(sqlmock.NewRows([]string{"sender_id", "unread_count", "last_message_at"}))
	items, err := NewService(db).Unread(context.Background(), "current-user")
	if err != nil {
		t.Fatal(err)
	}
	if items == nil || len(items) != 0 {
		t.Fatalf("empty result must be a non-nil empty slice: %#v", items)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
