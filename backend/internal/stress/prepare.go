package stress

import (
	"bufio"
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/google/uuid"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/store"
)

type Environment struct {
	MySQLRootDSN                                               string
	JWTSecret                                                  []byte
	MinIOEndpoint, MinIOAccessKey, MinIOSecretKey, MinIOBucket string
	MinIOSSL                                                   bool
}

func readEnvFile(path string) (map[string]string, error) {
	values := make(map[string]string)
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		line = strings.TrimPrefix(line, "export ")
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		value = strings.TrimSpace(value)
		if len(value) >= 2 && ((value[0] == '"' && value[len(value)-1] == '"') || (value[0] == '\'' && value[len(value)-1] == '\'')) {
			value = value[1 : len(value)-1]
		}
		values[strings.TrimSpace(key)] = value
	}
	return values, scanner.Err()
}

func LoadEnvironment(path string) (Environment, error) {
	e := Environment{}
	values, err := readEnvFile(path)
	if err != nil {
		return e, fmt.Errorf("open env file: %w", err)
	}
	// The isolated stress env intentionally contains only STRESS_* secrets. Merge
	// the sibling base env for root DB and MinIO credentials without modifying it.
	if values["STRESS_JWT_SECRET"] != "" || values["STRESS_MYSQL_PASSWORD"] != "" {
		base, readErr := readEnvFile(filepath.Join(filepath.Dir(path), ".env"))
		if readErr != nil {
			return e, fmt.Errorf("open base env for stress credentials: %w", readErr)
		}
		for key, value := range base {
			if _, exists := values[key]; !exists {
				values[key] = value
			}
		}
	}
	lookup := func(key, fallback string) string {
		if value, ok := os.LookupEnv(key); ok && value != "" {
			return value
		}
		if value := values[key]; value != "" {
			return value
		}
		return fallback
	}
	isolated := lookup("STRESS_JWT_SECRET", "") != ""
	port := lookup("MYSQL_PORT", "13306")
	defaultDatabase := lookup("MYSQL_DATABASE", "secure_im")
	if isolated {
		defaultDatabase = "secure_im_stress"
	}
	database := lookup("STRESS_MYSQL_DATABASE", defaultDatabase)
	e.MySQLRootDSN = lookup("STRESS_MYSQL_ROOT_DSN", fmt.Sprintf("root:%s@tcp(%s)/%s?parseTime=true&loc=UTC&time_zone=%%27%%2B00:00%%27&charset=utf8mb4&collation=utf8mb4_unicode_ci&timeout=5s", lookup("MYSQL_ROOT_PASSWORD", ""), lookup("MYSQL_ADDR", "127.0.0.1:"+port), database))
	e.JWTSecret = []byte(lookup("STRESS_JWT_SECRET", lookup("JWT_SECRET", "")))
	e.MinIOEndpoint = lookup("MINIO_ENDPOINT", "127.0.0.1:"+lookup("MINIO_API_PORT", "19000"))
	e.MinIOAccessKey = lookup("MINIO_ACCESS_KEY", lookup("MINIO_ROOT_USER", "minioadmin"))
	e.MinIOSecretKey = lookup("MINIO_SECRET_KEY", lookup("MINIO_ROOT_PASSWORD", ""))
	defaultBucket := "secure-im-files"
	if isolated {
		defaultBucket = "secure-im-stress-files"
	}
	e.MinIOBucket = lookup("STRESS_MINIO_BUCKET", lookup("MINIO_BUCKET", defaultBucket))
	e.MinIOSSL, _ = strconv.ParseBool(lookup("MINIO_USE_SSL", "false"))
	if len(e.JWTSecret) < 32 {
		return e, fmt.Errorf("JWT_SECRET (at least 32 bytes) is required in env file or process environment")
	}
	return e, nil
}

type User struct {
	ID, Username, Token, Role string
	Partner                   int
}

func RunID() string { return time.Now().UTC().Format("20060102T150405Z") + "-" + uuid.NewString()[:6] }
func UserPrefix(runID string) string {
	var b strings.Builder
	for _, r := range runID {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' {
			b.WriteRune(r)
		}
	}
	id := b.String()
	if len(id) > 19 {
		id = id[:19]
	}
	return "STRESS_" + id
}

func OpenResources(ctx context.Context, env Environment) (*sql.DB, *minio.Client, error) {
	db, err := store.Open(ctx, env.MySQLRootDSN)
	if err != nil {
		return nil, nil, err
	}
	objects, err := minio.New(env.MinIOEndpoint, &minio.Options{Creds: credentials.NewStaticV4(env.MinIOAccessKey, env.MinIOSecretKey, ""), Secure: env.MinIOSSL})
	if err != nil {
		db.Close()
		return nil, nil, err
	}
	ok, err := objects.BucketExists(ctx, env.MinIOBucket)
	if err != nil || !ok {
		db.Close()
		if err == nil {
			err = fmt.Errorf("bucket %s does not exist", env.MinIOBucket)
		}
		return nil, nil, err
	}
	return db, objects, nil
}

func CreateUsers(ctx context.Context, db *sql.DB, tokens *auth.TokenManager, cfg Config, allocation Allocation) ([]User, error) {
	if allocation.File > 0 && cfg.Target < 2 {
		return nil, fmt.Errorf("file load requires at least two users")
	}
	hash, err := auth.HashPassword(uuid.NewString(), auth.DefaultArgonParams)
	if err != nil {
		return nil, err
	}
	roles := make([]string, 0, cfg.Target)
	for i := 0; i < allocation.Idle; i++ {
		roles = append(roles, "idle")
	}
	for i := 0; i < allocation.Chat; i++ {
		roles = append(roles, "chat")
	}
	for i := 0; i < allocation.File; i++ {
		roles = append(roles, "file")
	}
	users := make([]User, cfg.Target)
	prefix := UserPrefix(cfg.RunID)
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	for base := 0; base < cfg.Target; base += 100 {
		end := min(base+100, cfg.Target)
		values := make([]string, 0, end-base)
		args := make([]any, 0, (end-base)*4)
		for i := base; i < end; i++ {
			users[i] = User{ID: uuid.NewString(), Username: fmt.Sprintf("%s_%05d", prefix, i), Role: roles[i], Partner: -1}
			values = append(values, "(?,?,?,?)")
			args = append(args, users[i].ID, users[i].Username, "Stress load user", hash)
		}
		if _, err = tx.ExecContext(ctx, "INSERT INTO users(id,username,display_name,password_hash) VALUES "+strings.Join(values, ","), args...); err != nil {
			return nil, err
		}
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	chatStart := allocation.Idle
	for i := 0; i+1 < allocation.Chat; i += 2 {
		users[chatStart+i].Partner = chatStart + i + 1
		users[chatStart+i+1].Partner = chatStart + i
	}
	for i := range users {
		fileStart := allocation.Idle + allocation.Chat
		if users[i].Role == "file" {
			users[i].Partner = fileStart + (i-fileStart+1)%max(1, allocation.File)
			if users[i].Partner == i {
				users[i].Partner = (i + 1) % cfg.Target
			}
		}
		users[i].Token, err = tokens.AccessToken(users[i].ID)
		if err != nil {
			return users, err
		}
	}
	return users, nil
}
