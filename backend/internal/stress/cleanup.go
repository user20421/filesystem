package stress

import (
	"context"
	"database/sql"
	"fmt"
	"strings"

	"github.com/minio/minio-go/v7"
)

// Cleanup removes only this run's objects and rows. It never drops a shared database or bucket.
func Cleanup(ctx context.Context, db *sql.DB, objects *minio.Client, bucket, runID string) error {
	if db == nil || objects == nil {
		return fmt.Errorf("database/object client unavailable")
	}
	prefix := "^" + UserPrefix(runID) + "_[0-9]{5}$"
	rows, err := db.QueryContext(ctx, `SELECT fc.object_key FROM file_chunks fc JOIN files f ON f.id=fc.file_id JOIN users u ON u.id=f.owner_id WHERE u.username REGEXP ?`, prefix)
	if err != nil {
		return err
	}
	var keys []string
	for rows.Next() {
		var key string
		if err = rows.Scan(&key); err != nil {
			rows.Close()
			return err
		}
		keys = append(keys, key)
	}
	if err = rows.Err(); err != nil {
		rows.Close()
		return err
	}
	rows.Close()
	var objectErrors []string
	for _, key := range keys {
		if err := objects.RemoveObject(ctx, bucket, key, minio.RemoveObjectOptions{}); err != nil {
			objectErrors = append(objectErrors, err.Error())
		}
	}
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	statements := []string{
		`DELETE fc FROM file_chunks fc JOIN files f ON f.id=fc.file_id JOIN users u ON u.id=f.owner_id WHERE u.username REGEXP ?`,
		`DELETE f FROM files f LEFT JOIN users o ON o.id=f.owner_id LEFT JOIN users r ON r.id=f.recipient_id WHERE o.username REGEXP ? OR r.username REGEXP ?`,
		`DELETE m FROM messages m LEFT JOIN users s ON s.id=m.sender_id LEFT JOIN users r ON r.id=m.recipient_id WHERE s.username REGEXP ? OR r.username REGEXP ?`,
		`DELETE cm FROM conversation_members cm JOIN conversations c ON c.id=cm.conversation_id LEFT JOIN users l ON l.id=c.user_low_id LEFT JOIN users h ON h.id=c.user_high_id WHERE l.username REGEXP ? OR h.username REGEXP ?`,
		`DELETE c FROM conversations c LEFT JOIN users l ON l.id=c.user_low_id LEFT JOIN users h ON h.id=c.user_high_id WHERE l.username REGEXP ? OR h.username REGEXP ?`,
		`DELETE rt FROM refresh_tokens rt JOIN users u ON u.id=rt.user_id WHERE u.username REGEXP ?`,
		`DELETE FROM users WHERE username REGEXP ?`,
	}
	for _, statement := range statements {
		args := []any{prefix}
		if strings.Count(statement, "?") == 2 {
			args = append(args, prefix)
		}
		if _, err = tx.ExecContext(ctx, statement, args...); err != nil {
			return fmt.Errorf("cleanup rows: %w", err)
		}
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	if len(objectErrors) > 0 {
		return fmt.Errorf("removed rows but some objects failed: %s", strings.Join(objectErrors, "; "))
	}
	return nil
}
