package main

import (
	"io"
	"log/slog"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/httpx"
)

// newLogger 保持进程统一使用 JSON 结构化日志。
func newLogger(output io.Writer) *slog.Logger {
	return slog.New(slog.NewJSONHandler(output, &slog.HandlerOptions{Level: slog.LevelInfo}))
}

// requestLogger 在请求完成后记录方法、路径、耗时和请求标识。
func requestLogger(log *slog.Logger) gin.HandlerFunc {
	return func(c *gin.Context) {
		start := time.Now()
		c.Next()
		log.Info("http request", "method", c.Request.Method, "path", c.Request.URL.Path, "duration", time.Since(start), "request_id", httpx.RequestID(c.Request.Context()))
	}
}
