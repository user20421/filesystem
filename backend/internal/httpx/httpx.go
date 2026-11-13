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

	"github.com/google/uuid"
)

type contextKey int

const requestIDKey contextKey = 1

type APIError struct {
	Status  int    `json:"-"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *APIError) Error() string { return e.Message }
func Error(status int, code, message string) *APIError {
	return &APIError{Status: status, Code: code, Message: message}
}

func JSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func Data(w http.ResponseWriter, status int, value any) {
	JSON(w, status, map[string]any{"data": value})
}
func Fail(w http.ResponseWriter, r *http.Request, err error) {
	var api *APIError
	if !errors.As(err, &api) {
		api = Error(http.StatusInternalServerError, "internal_error", "internal server error")
	}
	JSON(w, api.Status, map[string]any{"error": map[string]string{"code": api.Code, "message": api.Message, "request_id": RequestID(r.Context())}})
}
func Decode(w http.ResponseWriter, r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	dec := json.NewDecoder(r.Body)
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

func RequestContext(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-ID")
		if len(id) < 1 || len(id) > 64 || strings.ContainsAny(id, "\r\n") {
			id = uuid.NewString()
		}
		w.Header().Set("X-Request-ID", id)
		w.Header().Set("X-Content-Type-Options", "nosniff")
		ctx := context.WithValue(r.Context(), requestIDKey, id)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func Recover(log *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if recovered := recover(); recovered != nil {
				log.Error("panic serving request", "error", recovered, "stack", string(debug.Stack()), "request_id", RequestID(r.Context()))
				Fail(w, r, Error(http.StatusInternalServerError, "internal_error", "internal server error"))
			}
		}()
		next.ServeHTTP(w, r)
	})
}
