package httpx

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"runtime/debug"
	"strings"

	"github.com/gin-gonic/gin"
	"github.com/google/uuid"
)

type contextKey int

const requestIDKey contextKey = 1

// APIError 是可安全返回给客户端的业务错误，HTTP 状态只用于响应头，不进入 JSON 字段。
type APIError struct {
	Status  int    `json:"-"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *APIError) Error() string { return e.Message }
func Error(status int, code, message string) *APIError {
	return &APIError{Status: status, Code: code, Message: message}
}

// JSON 按指定状态写出 UTF-8 JSON；编码失败不会被包装成第二次成功响应。
func JSON(c *gin.Context, status int, value any) {
	c.Writer.Header().Set("Content-Type", "application/json; charset=utf-8")
	c.Writer.WriteHeader(status)
	_ = json.NewEncoder(c.Writer).Encode(value)
}

// Data 使用统一的 data 信封返回成功结果。
func Data(c *gin.Context, status int, value any) {
	JSON(c, status, map[string]any{"data": value})
}

// Fail 将 APIError 转为稳定错误响应，其他内部错误统一隐藏为 500，并携带请求 ID 便于排查。
func Fail(c *gin.Context, err error) {
	var api *APIError
	if !errors.As(err, &api) {
		api = Error(http.StatusInternalServerError, "internal_error", "internal server error")
	}
	JSON(c, api.Status, map[string]any{"error": map[string]string{"code": api.Code, "message": api.Message, "request_id": RequestID(c.Request.Context())}})
}

// Decode 限制请求体为 1 MiB、拒绝未知字段，并确保请求只包含一个 JSON 值。
func Decode(c *gin.Context, dst any) error {
	c.Request.Body = http.MaxBytesReader(c.Writer, c.Request.Body, 1<<20)
	dec := json.NewDecoder(c.Request.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return Error(http.StatusBadRequest, "invalid_json", "invalid JSON request")
	}
	if err := dec.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		return Error(http.StatusBadRequest, "invalid_json", "request must contain one JSON object")
	}
	return nil
}
func RequestID(ctx context.Context) string { v, _ := ctx.Value(requestIDKey).(string); return v }
func ClientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil {
		return host
	}
	return r.RemoteAddr
}

// RequestContext 透传合法请求 ID 或生成新 ID，同时设置 nosniff 防止内容类型嗅探。
func RequestContext() gin.HandlerFunc {
	return func(c *gin.Context) {
		id := c.Request.Header.Get("X-Request-ID")
		if len(id) < 1 || len(id) > 64 || strings.ContainsAny(id, "\r\n") {
			id = uuid.NewString()
		}
		c.Writer.Header().Set("X-Request-ID", id)
		c.Writer.Header().Set("X-Content-Type-Options", "nosniff")
		ctx := context.WithValue(c.Request.Context(), requestIDKey, id)
		c.Request = c.Request.WithContext(ctx)
		c.Next()
	}
}

// Recover 记录 panic 堆栈并返回统一 500，避免把内部实现细节暴露给调用方。
func Recover(log *slog.Logger) gin.HandlerFunc {
	return func(c *gin.Context) {
		defer func() {
			if recovered := recover(); recovered != nil {
				log.Error("panic serving request", "error", recovered, "stack", string(debug.Stack()), "request_id", RequestID(c.Request.Context()))
				c.Abort()
				Fail(c, Error(http.StatusInternalServerError, "internal_error", "internal server error"))
			}
		}()
		c.Next()
	}
}
