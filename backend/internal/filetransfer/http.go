package filetransfer

import (
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/httpx"
)

type Handler struct {
	service  *Service
	dbAudit  AuditFunc
	log      *slog.Logger
	maxChunk int64
}
type AuditFunc func(http *http.Request, action, resourceID string)

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
func (h *Handler) audit(r *http.Request, action, id string) {
	if h.dbAudit != nil {
		h.dbAudit(r, action, id)
	}
}
func (h *Handler) Init(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name        string `json:"filename"`
		Size        int64  `json:"size"`
		SHA256      string `json:"sha256"`
		RecipientID string `json:"recipient_id"`
		ChunkSize   int64  `json:"chunk_size"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	f, err := h.service.Init(r.Context(), auth.UserID(r.Context()), InitInput{Name: input.Name, Size: input.Size, SHA256: input.SHA256, RecipientID: input.RecipientID, ChunkSize: input.ChunkSize})
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	h.audit(r, "file.init", f.ID)
	httpx.Data(w, 201, f)
}
func (h *Handler) Status(w http.ResponseWriter, r *http.Request) {
	f, chunks, err := h.service.Status(r.Context(), r.PathValue("file_id"), auth.UserID(r.Context()))
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, map[string]any{"file": f, "uploaded_chunks": chunks})
}
func (h *Handler) Upload(w http.ResponseWriter, r *http.Request) {
	index, err := strconv.Atoi(r.PathValue("index"))
	if err != nil {
		httpx.Fail(w, r, httpx.Error(400, "invalid_chunk_index", "chunk index must be an integer"))
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, h.maxChunk+1)
	if r.ContentLength > h.maxChunk {
		httpx.Fail(w, r, httpx.Error(413, "chunk_too_large", "chunk exceeds configured maximum"))
		return
	}
	err = h.service.UploadChunk(r.Context(), r.PathValue("file_id"), auth.UserID(r.Context()), index, r.Header.Get("X-Chunk-SHA256"), r.Body)
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	h.audit(r, "file.chunk.upload", r.PathValue("file_id"))
	httpx.Data(w, 200, map[string]any{"uploaded": true, "index": index})
}
func (h *Handler) Complete(w http.ResponseWriter, r *http.Request) {
	// Hashing a large file may legitimately exceed the global API WriteTimeout.
	if err := setWriteDeadline(w, time.Time{}); err != nil {
		h.log.Error("clear completion write deadline", "file_id", r.PathValue("file_id"), "error", err)
		httpx.Fail(w, r, httpx.Error(http.StatusInternalServerError, "internal_error", "internal server error"))
		return
	}
	f, err := h.service.Complete(r.Context(), r.PathValue("file_id"), auth.UserID(r.Context()))
	if err != nil {
		h.log.Error("file completion failed", "file_id", r.PathValue("file_id"), "error", err)
		httpx.Fail(w, r, err)
		return
	}
	h.audit(r, "file.complete", f.ID)
	httpx.Data(w, 200, f)
}
func (h *Handler) Download(w http.ResponseWriter, r *http.Request) {
	// Remove the fixed global deadline, then enforce a rolling per-write idle
	// deadline below. Client disconnects and stalled writes still fail.
	if err := setWriteDeadline(w, time.Time{}); err != nil {
		h.log.Error("clear download write deadline", "file_id", r.PathValue("file_id"), "error", err)
		httpx.Fail(w, r, httpx.Error(http.StatusInternalServerError, "internal_error", "internal server error"))
		return
	}
	started := false
	output := &idleTimeoutResponseWriter{w: w, idleTimeout: downloadWriteIdleTimeout}
	err := h.service.Download(r.Context(), r.PathValue("file_id"), auth.UserID(r.Context()), output, func(f File) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Disposition", "attachment; filename*=UTF-8''"+url.PathEscape(f.OriginalName))
		w.Header().Set("Content-Length", strconv.FormatInt(f.Size, 10))
		started = true
	})
	if err != nil {
		if started {
			h.log.Error("download interrupted", "file_id", r.PathValue("file_id"), "error", err)
		} else {
			httpx.Fail(w, r, err)
		}
	}
}
func (h *Handler) Delete(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("file_id")
	if err := h.service.Delete(r.Context(), id, auth.UserID(r.Context())); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	h.audit(r, "file.delete", id)
	w.WriteHeader(204)
}
