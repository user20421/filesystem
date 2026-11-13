package account

import (
	"context"
	"net/http"

	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/httpx"
)

type deleter interface {
	Delete(context.Context, string) error
}

type Handler struct{ service deleter }

func NewHandler(service deleter) *Handler { return &Handler{service: service} }

func (h *Handler) DeleteMe(w http.ResponseWriter, r *http.Request) {
	if err := h.service.Delete(r.Context(), auth.UserID(r.Context())); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
