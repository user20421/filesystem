package stress

import (
	"context"
	"errors"
	"reflect"
	"regexp"
	"strings"
	"testing"

	"github.com/DATA-DOG/go-sqlmock"
)

type recordingFileDeleter struct {
	calls   []string
	failOn  string
	failErr error
}

func (d *recordingFileDeleter) DeleteForAccount(_ context.Context, id, owner string) error {
	d.calls = append(d.calls, id+":"+owner)
	if id == d.failOn {
		return d.failErr
	}
	return nil
}

func expectCleanupFiles(mock sqlmock.Sqlmock, runID string, rows *sqlmock.Rows) {
	prefix := "^" + UserPrefix(runID) + "_[0-9]{5}$"
	mock.ExpectQuery(regexp.QuoteMeta(`SELECT f.id,f.owner_id FROM files f LEFT JOIN users o ON o.id=f.owner_id LEFT JOIN users r ON r.id=f.recipient_id WHERE o.username REGEXP ? OR r.username REGEXP ? ORDER BY f.id`)).
		WithArgs(prefix, prefix).WillReturnRows(rows)
}

func cleanupFileRows(refs ...[2]string) *sqlmock.Rows {
	rows := sqlmock.NewRows(strings.Fields("id owner_id"))
	for _, ref := range refs {
		rows.AddRow(ref[0], ref[1])
	}
	return rows
}

func TestCleanupDeletesEachLogicalFileThroughService(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	expectCleanupFiles(mock, "run", cleanupFileRows([2]string{"file-1", "owner-1"}, [2]string{"file-2", "owner-2"}))
	deleter := &recordingFileDeleter{}
	if err := cleanupFiles(context.Background(), db, deleter, "run"); err != nil {
		t.Fatal(err)
	}
	want := []string{"file-1:owner-1", "file-2:owner-2"}
	if !reflect.DeepEqual(deleter.calls, want) {
		t.Fatalf("delete calls=%v, want %v", deleter.calls, want)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}

func TestCleanupFailureCanResumeRemainingFiles(t *testing.T) {
	db, mock, err := sqlmock.New()
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	expectCleanupFiles(mock, "run", cleanupFileRows([2]string{"file-1", "owner-1"}, [2]string{"file-2", "owner-2"}, [2]string{"file-3", "owner-3"}))
	deleter := &recordingFileDeleter{failOn: "file-2", failErr: errors.New("temporary failure")}
	if err := cleanupFiles(context.Background(), db, deleter, "run"); err == nil {
		t.Fatal("expected cleanup failure")
	}
	if want := []string{"file-1:owner-1", "file-2:owner-2"}; !reflect.DeepEqual(deleter.calls, want) {
		t.Fatalf("calls after failure=%v, want %v", deleter.calls, want)
	}

	// Committed deletions stay committed; the next run resumes the remaining rows.
	expectCleanupFiles(mock, "run", cleanupFileRows([2]string{"file-2", "owner-2"}, [2]string{"file-3", "owner-3"}))
	deleter.failOn = ""
	if err := cleanupFiles(context.Background(), db, deleter, "run"); err != nil {
		t.Fatal(err)
	}
	want := []string{"file-1:owner-1", "file-2:owner-2", "file-2:owner-2", "file-3:owner-3"}
	if !reflect.DeepEqual(deleter.calls, want) {
		t.Fatalf("delete calls after retry=%v, want %v", deleter.calls, want)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatal(err)
	}
}
