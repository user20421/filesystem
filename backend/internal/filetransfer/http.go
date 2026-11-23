package filetransfer

import (
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/httpx"
)

type Handler struct {
	service  *Service
	dbAudit  AuditFunc
	log      *slog.Logger
	maxChunk int64
}
type AuditFunc func(c *gin.Context, action, resourceID string)

const downloadWriteIdleTimeout = 30 * time.Second

// setWriteDeadline manages the deadline for this request only. Response
// recorders and other wrappers may not expose connection deadlines.
func setWriteDeadline(w http.ResponseWriter, deadline time.Time) error {
	err := http.NewResponseController(w).SetWriteDeadline(deadline)
	if errors.Is(err, http.ErrNotSupported) {
		return nil
	}
	return err
}

type idleTimeoutResponseWriter struct {
	w           http.ResponseWriter
	idleTimeout time.Duration
}

func (w *idleTimeoutResponseWriter) Write(p []byte) (int, error) {
	// Refresh immediately before every actual payload write. This is an idle
	// timeout, not a total response timeout; a slow but active download can run
	// longer than the server's global WriteTimeout.
	if err := setWriteDeadline(w.w, time.Now().Add(w.idleTimeout)); err != nil {
		return 0, fmt.Errorf("set download write deadline: %w", err)
	}
	return w.w.Write(p)
}

func NewHandler(service *Service, log *slog.Logger, maxChunk int64, audit AuditFunc) *Handler {
	return &Handler{service: service, log: log, maxChunk: maxChunk, dbAudit: audit}
}
func (h *Handler) audit(c *gin.Context, action, id string) {
	if h.dbAudit != nil {
		h.dbAudit(c, action, id)
	}
}
func (h *Handler) Init(c *gin.Context) {
	var input struct {
		Name        string `json:"filename"`
		Size        int64  `json:"size"`
		SHA256      string `json:"sha256"`
		RecipientID string `json:"recipient_id"`
		ChunkSize   int64  `json:"chunk_size"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	f, err := h.service.Init(c.Request.Context(), auth.UserID(c.Request.Context()), InitInput{Name: input.Name, Size: input.Size, SHA256: input.SHA256, RecipientID: input.RecipientID, ChunkSize: input.ChunkSize})
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	h.audit(c, "file.init", f.ID)
	httpx.Data(c, http.StatusCreated, f)
}
func (h *Handler) Status(c *gin.Context) {
	f, chunks, err := h.service.Status(c.Request.Context(), c.Param("file_id"), auth.UserID(c.Request.Context()))
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, map[string]any{"file": f, "uploaded_chunks": chunks})
}
func (h *Handler) Upload(c *gin.Context) {
	index, err := strconv.Atoi(c.Param("index"))
	if err != nil {
		httpx.Fail(c, httpx.Error(http.StatusBadRequest, "invalid_chunk_index", "chunk index must be an integer"))
		return
	}
	c.Request.Body = http.MaxBytesReader(c.Writer, c.Request.Body, h.maxChunk+1)
	if c.Request.ContentLength > h.maxChunk {
		httpx.Fail(c, httpx.Error(http.StatusRequestEntityTooLarge, "chunk_too_large", "chunk exceeds configured maximum"))
		return
	}
	err = h.service.UploadChunk(c.Request.Context(), c.Param("file_id"), auth.UserID(c.Request.Context()), index, c.Request.Header.Get("X-Chunk-SHA256"), c.Request.Body)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	h.audit(c, "file.chunk.upload", c.Param("file_id"))
	httpx.Data(c, http.StatusOK, map[string]any{"uploaded": true, "index": index})
}
func (h *Handler) Complete(c *gin.Context) {
	// Hashing a large file may legitimately exceed the global API WriteTimeout.
	if err := setWriteDeadline(c.Writer, time.Time{}); err != nil {
		h.log.Error("clear completion write deadline", "file_id", c.Param("file_id"), "error", err)
		httpx.Fail(c, httpx.Error(http.StatusInternalServerError, "internal_error", "internal server error"))
		return
	}
	f, err := h.service.Complete(c.Request.Context(), c.Param("file_id"), auth.UserID(c.Request.Context()))
	if err != nil {
		h.log.Error("file completion failed", "file_id", c.Param("file_id"), "error", err)
		httpx.Fail(c, err)
		return
	}
	h.audit(c, "file.complete", f.ID)
	httpx.Data(c, http.StatusOK, f)
}
func (h *Handler) Download(c *gin.Context) {
	// Remove the fixed global deadline, then enforce a rolling per-write idle
	// deadline below. Client disconnects and stalled writes still fail.
	if err := setWriteDeadline(c.Writer, time.Time{}); err != nil {
		h.log.Error("clear download write deadline", "file_id", c.Param("file_id"), "error", err)
		httpx.Fail(c, httpx.Error(http.StatusInternalServerError, "internal_error", "internal server error"))
		return
	}
	started := false
	output := &idleTimeoutResponseWriter{w: c.Writer, idleTimeout: downloadWriteIdleTimeout}
	err := h.service.Download(c.Request.Context(), c.Param("file_id"), auth.UserID(c.Request.Context()), output, func(f File) {
		c.Writer.Header().Set("Content-Type", "application/octet-stream")
		c.Writer.Header().Set("Content-Disposition", "attachment; filename*=UTF-8''"+url.PathEscape(f.OriginalName))
		c.Writer.Header().Set("Content-Length", strconv.FormatInt(f.Size, 10))
		started = true
	})
	if err != nil {
		if started {
			h.log.Error("download interrupted", "file_id", c.Param("file_id"), "error", err)
		} else {
			httpx.Fail(c, err)
		}
	}
}
func (h *Handler) Delete(c *gin.Context) {
	id := c.Param("file_id")
	if err := h.service.Delete(c.Request.Context(), id, auth.UserID(c.Request.Context())); err != nil {
		httpx.Fail(c, err)
		return
	}
	h.audit(c, "file.delete", id)
	c.Writer.WriteHeader(http.StatusNoContent)
}
