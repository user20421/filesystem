package auth

import (
	"context"
	"net/http"
	"strings"

	"github.com/gin-gonic/gin"
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
func (h *Handler) Register(c *gin.Context) {
	if !allowRequest(c, h.registerLimiter, httpx.ClientIP(c.Request)) {
		return
	}
	var input struct {
		Username    string `json:"username"`
		DisplayName string `json:"display_name"`
		Password    string `json:"password"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	result, err := h.service.Register(c.Request.Context(), input.Username, input.DisplayName, input.Password)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusCreated, result)
}
func (h *Handler) Login(c *gin.Context) {
	var input struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	key := httpx.ClientIP(c.Request) + "\x00" + normalizeUsername(input.Username)
	if !allowRequest(c, h.loginLimiter, key) {
		return
	}
	result, err := h.service.Login(c.Request.Context(), input.Username, input.Password)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, result)
}
func (h *Handler) Refresh(c *gin.Context) {
	var input struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	result, err := h.service.Refresh(c.Request.Context(), input.RefreshToken)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, result)
}
func (h *Handler) Logout(c *gin.Context) {
	var input struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	if err := h.service.Logout(c.Request.Context(), UserID(c.Request.Context()), input.RefreshToken); err != nil {
		httpx.Fail(c, err)
		return
	}
	c.Writer.WriteHeader(http.StatusNoContent)
}
func (h *Handler) Me(c *gin.Context) {
	user, err := h.service.Me(c.Request.Context(), UserID(c.Request.Context()))
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, user)
}
func (h *Handler) Users(c *gin.Context) {
	users, err := h.service.ListUsers(c.Request.Context())
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, map[string]any{"users": users})
}
func (h *Handler) Middleware() gin.HandlerFunc {
	return func(c *gin.Context) {
		parts := strings.SplitN(c.Request.Header.Get("Authorization"), " ", 2)
		if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") || parts[1] == "" {
			c.Abort()
			httpx.Fail(c, httpx.Error(http.StatusUnauthorized, "unauthorized", "Bearer access token required"))
			return
		}
		id, err := h.service.tokens.ParseAccessToken(strings.TrimSpace(parts[1]))
		if err != nil {
			c.Abort()
			httpx.Fail(c, httpx.Error(http.StatusUnauthorized, "unauthorized", "invalid or expired access token"))
			return
		}
		// JWT validity alone is insufficient after permanent account deletion.
		if _, err := h.lookupUser(c.Request.Context(), id); err != nil {
			c.Abort()
			httpx.Fail(c, err)
			return
		}
		c.Request = c.Request.WithContext(context.WithValue(c.Request.Context(), userContextKey{}, id))
		c.Next()
	}
}
