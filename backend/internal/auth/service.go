package auth

import (
	"context"
	"database/sql"
	"errors"
	"regexp"
	"strings"
	"time"

	"github.com/go-sql-driver/mysql"
	"github.com/google/uuid"
	"secure-im/backend/internal/apperror"
)

type User struct {
	ID          string    `json:"id"`
	Username    string    `json:"username"`
	DisplayName string    `json:"display_name"`
	CreatedAt   time.Time `json:"created_at"`
}
type Tokens struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	TokenType    string `json:"token_type"`
	ExpiresIn    int64  `json:"expires_in"`
}
type Result struct {
	User   User   `json:"user"`
	Tokens Tokens `json:"tokens"`
}
type Service struct {
	repo      repository
	tokens    *TokenManager
	dummyHash string
}

var usernamePattern = regexp.MustCompile(`^[a-zA-Z0-9_.-]{3,32}$`)

func normalizeUsername(username string) string { return strings.ToUpper(username) }

func NewService(db *sql.DB, tokens *TokenManager) *Service {
	dummy, _ := HashPassword("not-a-real-user-password", DefaultArgonParams)
	return &Service{repo: &sqlRepository{db: db}, tokens: tokens, dummyHash: dummy}
}
func validateRegistration(username, displayName, password string) error {
	if !usernamePattern.MatchString(username) {
		return apperror.New(apperror.KindBadRequest, "invalid_username", "username must be 3-32 characters using letters, numbers, dot, underscore or dash")
	}
	if strings.TrimSpace(displayName) == "" || len(displayName) > 64 {
		return apperror.New(apperror.KindBadRequest, "invalid_display_name", "display name is required and must not exceed 64 bytes")
	}
	if len(password) < 6 || len(password) > 128 {
		return apperror.New(apperror.KindBadRequest, "invalid_password", "password must be 6 to 128 bytes")
	}
	return nil
}
func (s *Service) Register(ctx context.Context, username, displayName, password string) (Result, error) {
	username = normalizeUsername(username)
	if err := validateRegistration(username, displayName, password); err != nil {
		return Result{}, err
	}
	hash, err := HashPassword(password, DefaultArgonParams)
	if err != nil {
		return Result{}, err
	}
	user := User{ID: uuid.NewString(), Username: username, DisplayName: strings.TrimSpace(displayName), CreatedAt: time.Now().UTC()}
	err = s.repo.CreateUser(ctx, user, hash)
	var mysqlErr *mysql.MySQLError
	if errors.As(err, &mysqlErr) && mysqlErr.Number == 1062 {
		return Result{}, apperror.New(apperror.KindConflict, "username_taken", "username is already registered")
	}
	if err != nil {
		return Result{}, err
	}
	return s.result(ctx, user)
}
func (s *Service) Login(ctx context.Context, username, password string) (Result, error) {
	username = normalizeUsername(username)
	if len(password) > 128 {
		return Result{}, apperror.New(apperror.KindUnauthorized, "invalid_credentials", "invalid username or password")
	}
	user, hash, err := s.repo.LoginUser(ctx, username)
	if errors.Is(err, sql.ErrNoRows) {
		_, _ = VerifyPassword(s.dummyHash, password)
		return Result{}, apperror.New(apperror.KindUnauthorized, "invalid_credentials", "invalid username or password")
	}
	if err != nil {
		return Result{}, err
	}
	user.Username = normalizeUsername(user.Username)
	valid, verifyErr := VerifyPassword(hash, password)
	if verifyErr != nil || !valid {
		return Result{}, apperror.New(apperror.KindUnauthorized, "invalid_credentials", "invalid username or password")
	}
	return s.result(ctx, user)
}
func (s *Service) result(ctx context.Context, user User) (Result, error) {
	access, err := s.tokens.AccessToken(user.ID)
	if err != nil {
		return Result{}, err
	}
	refresh, hash, expires, err := s.tokens.RefreshToken()
	if err != nil {
		return Result{}, err
	}
	if err := s.repo.CreateRefreshToken(ctx, uuid.NewString(), user.ID, hash, expires); err != nil {
		return Result{}, err
	}
	return Result{User: user, Tokens: Tokens{AccessToken: access, RefreshToken: refresh, TokenType: "Bearer", ExpiresIn: int64(s.tokens.accessTTL.Seconds())}}, nil
}
