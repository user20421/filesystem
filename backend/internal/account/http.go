package account

import (
	"context"
	"net/http"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/httpx"
)

type deleter interface {
	Delete(context.Context, string) error
}

type Handler struct{ service deleter }

func NewHandler(service deleter) *Handler { return &Handler{service: service} }

func (h *Handler) DeleteMe(c *gin.Context) {
	if err := h.service.Delete(c.Request.Context(), auth.UserID(c.Request.Context())); err != nil {
		httpx.Fail(c, err)
		return
	}
	c.Writer.WriteHeader(http.StatusNoContent)
}
