package stress

import (
	"context"
	"database/sql"
	"fmt"
	"strings"

	"github.com/minio/minio-go/v7"
	"secure-im/backend/internal/filetransfer"
)

type cleanupFileDeleter interface {
	DeleteForAccount(context.Context, string, string) error
}

// Cleanup removes only this run's rows. File deletion goes through the normal
// service so shared references, deleting state, and object cleanup stay safe.
func Cleanup(ctx context.Context, db *sql.DB, objects *minio.Client, bucket, runID string) error {
	if db == nil || objects == nil {
		return fmt.Errorf("database/object client unavailable")
	}
	store := filetransfer.NewMinIOStoreFromClient(objects, bucket)
	files := filetransfer.NewDeletionService(db, store)
	if err := cleanupFiles(ctx, db, files, runID); err != nil {
		return err
	}
	return cleanupRows(ctx, db, runID)
}

func cleanupFiles(ctx context.Context, db *sql.DB, files cleanupFileDeleter, runID string) error {
	prefix := "^" + UserPrefix(runID) + "_[0-9]{5}$"
	rows, err := db.QueryContext(ctx, `SELECT f.id,f.owner_id FROM files f LEFT JOIN users o ON o.id=f.owner_id LEFT JOIN users r ON r.id=f.recipient_id WHERE o.username REGEXP ? OR r.username REGEXP ? ORDER BY f.id`, prefix, prefix)
	if err != nil {
		return err
	}
	type fileRef struct{ id, ownerID string }
	var refs []fileRef
	for rows.Next() {
		var ref fileRef
		if err := rows.Scan(&ref.id, &ref.ownerID); err != nil {
			rows.Close()
			return err
		}
		refs = append(refs, ref)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	if err := rows.Close(); err != nil {
		return err
	}
	for _, ref := range refs {
		if err := files.DeleteForAccount(ctx, ref.id, ref.ownerID); err != nil {
			return fmt.Errorf("cleanup file %s: %w", ref.id, err)
		}
	}
	return nil
}

func cleanupRows(ctx context.Context, db *sql.DB, runID string) error {
	prefix := "^" + UserPrefix(runID) + "_[0-9]{5}$"
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	statements := []string{
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
	return tx.Commit()
}
