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
	accountHTTP := account.NewHandler(account.NewService(db, files, hub.CloseUser, tickets.RevokeUser))
	audit := func(r *http.Request, action, id string) {
		store.Audit(r.Context(), db, auth.UserID(r.Context()), action, id, httpx.ClientIP(r))
	}
	fileHTTP := filetransfer.NewHandler(files, log, cfg.MaxChunkSize, audit)
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, r *http.Request) { httpx.Data(w, 200, map[string]string{"status": "ok"}) })
	mux.HandleFunc("GET /ready", func(w http.ResponseWriter, r *http.Request) {
		readyCtx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
		defer cancel()
		if err := db.PingContext(readyCtx); err != nil {
			httpx.Fail(w, r, httpx.Error(503, "database_unavailable", "database is unavailable"))
			return
		}
		if err := objects.Ensure(readyCtx); err != nil {
			httpx.Fail(w, r, httpx.Error(503, "storage_unavailable", "object storage is unavailable"))
			return
		}
		httpx.Data(w, 200, map[string]string{"status": "ready"})
	})
	if cfg.EnableDebugStats {
		mux.HandleFunc("GET /debug/stats", func(w http.ResponseWriter, r *http.Request) {
			var mem runtime.MemStats
			runtime.ReadMemStats(&mem)
			used, limit := files.MemoryStats()
			httpx.Data(w, 200, map[string]any{
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
			})
		})
	}
	mux.HandleFunc("POST /api/v1/auth/register", authHTTP.Register)
	mux.HandleFunc("POST /api/v1/auth/login", authHTTP.Login)
	mux.HandleFunc("POST /api/v1/auth/refresh", authHTTP.Refresh)
	protect := func(handler http.HandlerFunc) http.Handler { return authHTTP.Middleware(handler) }
	mux.Handle("POST /api/v1/auth/logout", protect(authHTTP.Logout))
	mux.Handle("GET /api/v1/users/me", protect(authHTTP.Me))
	mux.Handle("DELETE /api/v1/users/me", protect(accountHTTP.DeleteMe))
	mux.Handle("GET /api/v1/users", protect(authHTTP.Users))
	mux.Handle("GET /api/v1/presence", protect(chatHTTP.Presence))
	mux.Handle("POST /api/v1/ws-ticket", protect(chatHTTP.Ticket))
	mux.Handle("GET /api/v1/messages", protect(chatHTTP.History))
	mux.Handle("POST /api/v1/messages/read", protect(chatHTTP.MarkRead))
	mux.Handle("POST /api/v1/files/init", protect(fileHTTP.Init))
	mux.Handle("GET /api/v1/files/{file_id}", protect(fileHTTP.Status))
	mux.Handle("PUT /api/v1/files/{file_id}/chunks/{index}", protect(fileHTTP.Upload))
	mux.Handle("POST /api/v1/files/{file_id}/complete", protect(fileHTTP.Complete))
	mux.Handle("GET /api/v1/files/{file_id}/download", protect(fileHTTP.Download))
	mux.Handle("DELETE /api/v1/files/{file_id}", protect(fileHTTP.Delete))
	mux.HandleFunc("/ws", chatHTTP.WebSocket)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		httpx.Fail(w, r, httpx.Error(404, "not_found", "route not found"))
	})
	server := &http.Server{Addr: cfg.Address, Handler: httpx.RequestContext(httpx.Recover(log, requestLogger(log, mux))), ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 60 * time.Second, IdleTimeout: 90 * time.Second, MaxHeaderBytes: 1 << 20}
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
func requestLogger(log *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Info("http request", "method", r.Method, "path", r.URL.Path, "duration", time.Since(start), "request_id", httpx.RequestID(r.Context()))
	})
}
