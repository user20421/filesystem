package main

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/account"
	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/chat"
	"secure-im/backend/internal/config"
	"secure-im/backend/internal/filetransfer"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/store"
)

// application 集中保存已装配的垂直服务，资源生命周期由 run 统一管理。
type application struct {
	cfg         config.Config
	log         *slog.Logger
	db          *sql.DB
	objects     *filetransfer.MinIOStore
	authHTTP    *auth.Handler
	chatHTTP    *chat.Handler
	accountHTTP *account.Handler
	fileHTTP    *filetransfer.Handler
	files       *filetransfer.Service
	hub         *chat.Hub
}

// newApplication 按“数据库→迁移→对象存储→业务服务”的顺序完成应用装配。
func newApplication(ctx context.Context, cfg config.Config, log *slog.Logger) (*application, error) {
	db, err := store.Open(ctx, cfg.MySQLDSN)
	if err != nil {
		return nil, err
	}
	ok := false
	defer func() {
		if !ok {
			_ = db.Close()
		}
	}()
	if err := store.Migrate(ctx, db); err != nil {
		return nil, err
	}
	objects, err := filetransfer.NewMinIOStore(cfg.MinIOEndpoint, cfg.MinIOAccessKey, cfg.MinIOSecretKey, cfg.MinIOBucket, cfg.MinIOUseSSL)
	if err != nil {
		return nil, err
	}
	if err := objects.Ensure(ctx); err != nil {
		return nil, err
	}

	tokens := auth.NewTokenManager(cfg.JWTSecret, cfg.AccessTTL, cfg.RefreshTTL)
	authService := auth.NewService(db, tokens)
	authHTTP := auth.NewHandler(authService,
		auth.RateLimitConfig{Limit: cfg.RegisterRateLimit, Window: cfg.AuthRateLimitWindow, MaxKeys: cfg.AuthRateLimitMaxKeys},
		auth.RateLimitConfig{Limit: cfg.LoginRateLimit, Window: cfg.AuthRateLimitWindow, MaxKeys: cfg.AuthRateLimitMaxKeys},
	)
	chatService := chat.NewService(db)
	hub := chat.NewHub()
	tickets := chat.NewTicketStore(cfg.WSTicketTTL)
	chatHTTP := chat.NewHandler(chatService, tickets, hub, log, cfg.WSMaxConnections)
	files := filetransfer.NewServiceWithMemoryBudget(db, objects, cfg.MaxActiveFiles, cfg.MaxChunkSize, cfg.MaxFileSize, cfg.UploadConcurrency, cfg.FileMemoryBudget)
	accountHTTP := account.NewHandler(account.NewService(db, files, hub.CloseUser, tickets.RevokeUser))
	audit := func(c *gin.Context, action, id string) {
		store.Audit(c.Request.Context(), db, auth.UserID(c.Request.Context()), action, id, httpx.ClientIP(c.Request))
	}
	fileHTTP := filetransfer.NewHandler(files, log, cfg.MaxChunkSize, audit)
	ok = true
	return &application{cfg: cfg, log: log, db: db, objects: objects, authHTTP: authHTTP, chatHTTP: chatHTTP, accountHTTP: accountHTTP, fileHTTP: fileHTTP, files: files, hub: hub}, nil
}

// run 管理启动、信号、清理器和数据库的完整生命周期；退出顺序保持不变。
func run(log *slog.Logger) error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	startup, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	app, err := newApplication(startup, cfg, log)
	if err != nil {
		return err
	}
	defer app.db.Close()

	cleanupCtx, stopCleanup := context.WithCancel(ctx)
	cleanupDone := make(chan struct{})
	sweeper := filetransfer.NewSweeper(app.files, log, filetransfer.CleanupConfig{
		UploadIdleTTL:      cfg.FileUploadIdleTTL,
		DeletingRetryAfter: cfg.FileDeletingRetryAfter,
		Interval:           cfg.FileCleanupInterval,
	})
	go func() {
		defer close(cleanupDone)
		sweeper.Run(cleanupCtx)
	}()
	defer func() {
		stopCleanup()
		<-cleanupDone
	}()
	return app.serve(ctx)
}

// serve 启动 HTTP，并在收到退出信号后先断开实时连接，再优雅 draining 请求。
func (a *application) serve(ctx context.Context) error {
	server := &http.Server{Addr: a.cfg.Address, Handler: a.routes(), ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 60 * time.Second, IdleTimeout: 90 * time.Second, MaxHeaderBytes: 1 << 20}
	errCh := make(chan error, 1)
	go func() { a.log.Info("server listening", "address", a.cfg.Address); errCh <- server.ListenAndServe() }()
	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		a.hub.CloseAll()
		shutdown, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		if err := server.Shutdown(shutdown); err != nil {
			return err
		}
		if err := <-errCh; err != nil && !errors.Is(err, http.ErrServerClosed) {
			return err
		}
		return nil
	}
}
