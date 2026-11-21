package store

import (
	"context"
	"database/sql"
	"embed"
	"fmt"
	"sort"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

//go:embed migrations/*.sql
var migrations embed.FS

func Open(ctx context.Context, dsn string) (*sql.DB, error) {
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(25)
	db.SetMaxIdleConns(10)
	db.SetConnMaxLifetime(30 * time.Minute)
	pingCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	if err := db.PingContext(pingCtx); err != nil {
		db.Close()
		return nil, fmt.Errorf("mysql ping: %w", err)
	}
	return db, nil
}

func Migrate(ctx context.Context, db *sql.DB) error {
	if _, err := db.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (version VARCHAR(255) PRIMARY KEY, applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)) ENGINE=InnoDB`); err != nil {
		return err
	}
	conn, err := db.Conn(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	var lock int
	if err := conn.QueryRowContext(ctx, `SELECT GET_LOCK('secure_im_schema_migration', 15)`).Scan(&lock); err != nil {
		return err
	}
	if lock != 1 {
		return fmt.Errorf("could not acquire migration lock")
	}
	defer conn.ExecContext(context.Background(), `SELECT RELEASE_LOCK('secure_im_schema_migration')`)
	entries, err := migrations.ReadDir("migrations")
	if err != nil {
		return err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
	for _, entry := range entries {
		var exists int
		err := conn.QueryRowContext(ctx, `SELECT COUNT(1) FROM schema_migrations WHERE version=?`, entry.Name()).Scan(&exists)
		if err != nil {
			return err
		}
		if exists != 0 {
			continue
		}
		if entry.Name() == "003_shared_plaintext_files.sql" {
			if err := guardLegacyFileReset(ctx, conn); err != nil {
				return err
			}
		}
		body, err := migrations.ReadFile("migrations/" + entry.Name())
		if err != nil {
			return err
		}
		for _, statement := range strings.Split(string(body), ";") {
			if strings.TrimSpace(statement) == "" {
				continue
			}
			if _, err := conn.ExecContext(ctx, statement); err != nil {
				return fmt.Errorf("migration %s: %w", entry.Name(), err)
			}
		}
		if _, err := conn.ExecContext(ctx, `INSERT INTO schema_migrations(version) VALUES(?)`, entry.Name()); err != nil {
			return err
		}
	}
	return nil
}

func guardLegacyFileReset(ctx context.Context, conn *sql.Conn) error {
	var legacy int
	if err := conn.QueryRowContext(ctx, `SELECT COUNT(1) FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='files' AND column_name='wrapped_dek'`).Scan(&legacy); err != nil {
		return err
	}
	if legacy == 0 {
		return nil
	}
	var files, chunks int
	if err := conn.QueryRowContext(ctx, `SELECT (SELECT COUNT(1) FROM files),(SELECT COUNT(1) FROM file_chunks)`).Scan(&files, &chunks); err != nil {
		return err
	}
	if files != 0 || chunks != 0 {
		return fmt.Errorf("legacy encrypted file data is not empty (%d files, %d chunks); reset the database and object storage before applying the plaintext shared-content schema", files, chunks)
	}
	return nil
}

func Audit(ctx context.Context, db *sql.DB, userID, action, resourceID, ip string) {
	_, _ = db.ExecContext(ctx, `INSERT INTO audit_logs(user_id,action,resource_id,ip_address) VALUES(NULLIF(?,''),?,?,?)`, userID, action, resourceID, ip)
}
