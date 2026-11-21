package auth

import (
	"context"
	"net/http"
	"strings"

	"secure-im/backend/internal/httpx"
)

type userContextKey struct{}

func UserID(ctx context.Context) string { id, _ := ctx.Value(userContextKey{}).(string); return id }

type Handler struct {
	service         *Service
	lookupUser      func(context.Context, string) (User, error)
	registerLimiter *rateLimiter
	loginLimiter    *rateLimiter
}

func NewHandler(service *Service, registerLimit, loginLimit RateLimitConfig) *Handler {
	return &Handler{
		service:         service,
		lookupUser:      service.Me,
		registerLimiter: newRateLimiter(registerLimit),
		loginLimiter:    newRateLimiter(loginLimit),
	}
}
func (h *Handler) Register(w http.ResponseWriter, r *http.Request) {
	if !allowRequest(w, r, h.registerLimiter, httpx.ClientIP(r)) {
		return
	}
	var input struct {
		Username    string `json:"username"`
		DisplayName string `json:"display_name"`
		Password    string `json:"password"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	result, err := h.service.Register(r.Context(), input.Username, input.DisplayName, input.Password)
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 201, result)
}
func (h *Handler) Login(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	key := httpx.ClientIP(r) + "\x00" + normalizeUsername(input.Username)
	if !allowRequest(w, r, h.loginLimiter, key) {
		return
	}
	result, err := h.service.Login(r.Context(), input.Username, input.Password)
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, result)
}
func (h *Handler) Refresh(w http.ResponseWriter, r *http.Request) {
	var input struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	result, err := h.service.Refresh(r.Context(), input.RefreshToken)
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, result)
}
func (h *Handler) Logout(w http.ResponseWriter, r *http.Request) {
	var input struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	if err := h.service.Logout(r.Context(), UserID(r.Context()), input.RefreshToken); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
func (h *Handler) Me(w http.ResponseWriter, r *http.Request) {
	user, err := h.service.Me(r.Context(), UserID(r.Context()))
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, user)
}
func (h *Handler) Users(w http.ResponseWriter, r *http.Request) {
	users, err := h.service.ListUsers(r.Context())
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, map[string]any{"users": users})
}
func (h *Handler) Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		parts := strings.SplitN(r.Header.Get("Authorization"), " ", 2)
		if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") || parts[1] == "" {
			httpx.Fail(w, r, httpx.Error(401, "unauthorized", "Bearer access token required"))
			return
		}
		id, err := h.service.tokens.ParseAccessToken(strings.TrimSpace(parts[1]))
		if err != nil {
			httpx.Fail(w, r, httpx.Error(401, "unauthorized", "invalid or expired access token"))
			return
		}
		// JWT validity alone is insufficient after permanent account deletion.
		if _, err := h.lookupUser(r.Context(), id); err != nil {
			httpx.Fail(w, r, err)
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), userContextKey{}, id)))
	})
}
