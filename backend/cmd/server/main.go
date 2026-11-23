package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"runtime"
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

func main() {
	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	if err := run(log); err != nil {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

// run 按“连接数据库→执行迁移→检查对象存储→启动文件清理→启动 HTTP”的顺序装配服务。
// 退出时先停止清理器和实时连接，再优雅关闭 HTTP 与数据库，避免后台任务访问已释放资源。
func run(log *slog.Logger) error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	startup, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	db, err := store.Open(startup, cfg.MySQLDSN)
	if err != nil {
		return err
	}
	defer db.Close()
	if err := store.Migrate(startup, db); err != nil {
		return err
	}
	objects, err := filetransfer.NewMinIOStore(cfg.MinIOEndpoint, cfg.MinIOAccessKey, cfg.MinIOSecretKey, cfg.MinIOBucket, cfg.MinIOUseSSL)
	if err != nil {
		return err
	}
	if err := objects.Ensure(startup); err != nil {
		return err
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
	cleanupCtx, stopCleanup := context.WithCancel(ctx)
	cleanupDone := make(chan struct{})
	sweeper := filetransfer.NewSweeper(files, log, filetransfer.CleanupConfig{
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
	accountHTTP := account.NewHandler(account.NewService(db, files, hub.CloseUser, tickets.RevokeUser))
	audit := func(c *gin.Context, action, id string) {
		store.Audit(c.Request.Context(), db, auth.UserID(c.Request.Context()), action, id, httpx.ClientIP(c.Request))
	}
	fileHTTP := filetransfer.NewHandler(files, log, cfg.MaxChunkSize, audit)
	mux := newRouter(authHTTP.Middleware(), chatHTTP.WebSocket, httpx.RequestContext(), httpx.Recover(log), requestLogger(log))
	get(mux.engine, "/health", func(c *gin.Context) { httpx.Data(c, http.StatusOK, map[string]string{"status": "ok"}) })
	get(mux.engine, "/ready", func(c *gin.Context) {
		readyCtx, cancel := context.WithTimeout(c.Request.Context(), 3*time.Second)
		defer cancel()
		if err := db.PingContext(readyCtx); err != nil {
			httpx.Fail(c, httpx.Error(http.StatusServiceUnavailable, "database_unavailable", "database is unavailable"))
			return
		}
		if err := objects.Ensure(readyCtx); err != nil {
			httpx.Fail(c, httpx.Error(http.StatusServiceUnavailable, "storage_unavailable", "object storage is unavailable"))
			return
		}
		httpx.Data(c, http.StatusOK, map[string]string{"status": "ready"})
	})
	if cfg.EnableDebugStats {
		get(mux.engine, "/debug/stats", func(c *gin.Context) {
			var mem runtime.MemStats
			runtime.ReadMemStats(&mem)
			used, limit := files.MemoryStats()
			slotsUsed, slotsLimit := files.ChunkSlotStats()
			httpx.Data(c, http.StatusOK, map[string]any{
				"go": map[string]any{
					"mem_alloc_bytes":       mem.Alloc,
					"mem_total_alloc_bytes": mem.TotalAlloc,
					"mem_sys_bytes":         mem.Sys,
					"heap_alloc_bytes":      mem.HeapAlloc,
					"heap_inuse_bytes":      mem.HeapInuse,
					"heap_objects":          mem.HeapObjects,
					"goroutines":            runtime.NumGoroutine(),
					"gomaxprocs":            runtime.GOMAXPROCS(0),
				},
				"file_memory_budget": map[string]int64{"used": used, "limit": limit},
				"file_chunk_slots":   map[string]int{"used": slotsUsed, "limit": slotsLimit},
			})
		})
	}
	mux.public.POST("/auth/register", authHTTP.Register)
	mux.public.POST("/auth/login", authHTTP.Login)
	mux.public.POST("/auth/refresh", authHTTP.Refresh)
	mux.protected.POST("/auth/logout", authHTTP.Logout)
	get(mux.protected, "/users/me", authHTTP.Me)
	mux.protected.DELETE("/users/me", accountHTTP.DeleteMe)
	get(mux.protected, "/users", authHTTP.Users)
	get(mux.protected, "/presence", chatHTTP.Presence)
	mux.protected.POST("/ws-ticket", chatHTTP.Ticket)
	get(mux.protected, "/messages", chatHTTP.History)
	get(mux.protected, "/messages/unread", chatHTTP.Unread)
	mux.protected.POST("/messages/read", chatHTTP.MarkRead)
	mux.protected.POST("/files/init", fileHTTP.Init)
	get(mux.protected, "/files/:file_id", fileHTTP.Status)
	mux.protected.PUT("/files/:file_id/chunks/:index", fileHTTP.Upload)
	mux.protected.POST("/files/:file_id/complete", fileHTTP.Complete)
	get(mux.protected, "/files/:file_id/download", fileHTTP.Download)
	mux.protected.DELETE("/files/:file_id", fileHTTP.Delete)
	server := &http.Server{Addr: cfg.Address, Handler: mux, ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 60 * time.Second, IdleTimeout: 90 * time.Second, MaxHeaderBytes: 1 << 20}
	errCh := make(chan error, 1)
	go func() { log.Info("server listening", "address", cfg.Address); errCh <- server.ListenAndServe() }()
	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		hub.CloseAll()
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
func requestLogger(log *slog.Logger) gin.HandlerFunc {
	return func(c *gin.Context) {
		start := time.Now()
		c.Next()
		log.Info("http request", "method", c.Request.Method, "path", c.Request.URL.Path, "duration", time.Since(start), "request_id", httpx.RequestID(c.Request.Context()))
	}
}
