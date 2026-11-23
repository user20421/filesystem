package chat

import (
	"context"
	"database/sql"
	"errors"
	"regexp"
	"testing"

	"github.com/DATA-DOG/go-sqlmock"
	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

const (
	readCursorQuery = `SELECT conversation_id,read_at FROM messages WHERE id=? AND sender_id=? AND recipient_id=? FOR UPDATE`
	readAtQuery     = `SELECT read_at FROM messages WHERE id=?`
)

func TestMarkReadMapsOnlyMissingCursorToForbidden(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	reader, peer := uuid.NewString(), uuid.NewString()
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(readCursorQuery)).WithArgs(uint64(42), peer, reader).WillReturnError(sql.ErrNoRows)
	mock.ExpectRollback()

	_, err = NewService(db).MarkRead(context.Background(), reader, peer, 42)
	var business *apperror.Error
	if !errors.As(err, &business) || business.Kind != apperror.KindForbidden || business.Code != "read_forbidden" {
		t.Fatalf("MarkRead error = %#v, want read_forbidden", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestMarkReadSecondLookupNoRowsRemainsInternalError(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	reader, peer := uuid.NewString(), uuid.NewString()
	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(readCursorQuery)).WithArgs(uint64(42), peer, reader).
		WillReturnRows(sqlmock.NewRows([]string{"conversation_id", "read_at"}).AddRow("conversation", nil))
	mock.ExpectExec(`UPDATE messages SET read_at=NOW\(6\) WHERE conversation_id=\? AND sender_id=\? AND recipient_id=\? AND id<=\? AND read_at IS NULL`).
		WithArgs("conversation", peer, reader, uint64(42)).WillReturnResult(sqlmock.NewResult(0, 1))
	mock.ExpectQuery(regexp.QuoteMeta(readAtQuery)).WithArgs(uint64(42)).WillReturnError(sql.ErrNoRows)
	mock.ExpectRollback()

	_, err = NewService(db).MarkRead(context.Background(), reader, peer, 42)
	if !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("MarkRead error = %v, want raw sql.ErrNoRows", err)
	}
	var business *apperror.Error
	if errors.As(err, &business) {
		t.Fatalf("second lookup was incorrectly mapped to a business error: %#v", business)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
