package chat

import (
	"context"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/auth"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/proto/impb"
)

type Handler struct {
	service  *Service
	tickets  *TicketStore
	hub      *Hub
	log      *slog.Logger
	upgrader websocket.Upgrader
	slots    chan struct{}
}

func NewHandler(service *Service, tickets *TicketStore, hub *Hub, log *slog.Logger, maxConnections int) *Handler {
	h := &Handler{service: service, tickets: tickets, hub: hub, log: log, slots: make(chan struct{}, maxConnections)}
	h.upgrader = websocket.Upgrader{ReadBufferSize: 4096, WriteBufferSize: 4096, CheckOrigin: func(r *http.Request) bool {
		origin := r.Header.Get("Origin")
		if origin == "" {
			return true
		}
		u, err := url.Parse(origin)
		return err == nil && u.Host == r.Host
	}}
	return h
}
func (h *Handler) Ticket(c *gin.Context) {
	ticket, err := h.tickets.Issue(auth.UserID(c.Request.Context()))
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusCreated, map[string]any{"ticket": ticket, "expires_in": int64(h.tickets.ttl.Seconds())})
}
func (h *Handler) Presence(c *gin.Context) {
	httpx.Data(c, http.StatusOK, map[string]any{"online_user_ids": h.hub.OnlineUsers()})
}

func (h *Handler) MarkRead(c *gin.Context) {
	var input struct {
		PeerID string `json:"peer_id"`
		Cursor uint64 `json:"cursor"`
	}
	if err := httpx.Decode(c, &input); err != nil {
		httpx.Fail(c, err)
		return
	}
	readerID := auth.UserID(c.Request.Context())
	receipt, err := h.service.MarkRead(c.Request.Context(), readerID, input.PeerID, input.Cursor)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	event := &impb.Envelope{
		Version:        protocolVersion,
		Type:           impb.Envelope_READ,
		ConversationId: receipt.ConversationID,
		SenderId:       readerID,
		RecipientId:    receipt.PeerID,
		Cursor:         receipt.Cursor,
		ReadAt:         receipt.ReadAt,
		Timestamp:      receipt.ReadAt,
	}
	h.hub.Publish(receipt.PeerID, event)
	h.hub.Publish(readerID, event)
	httpx.Data(c, http.StatusOK, receipt)
}

func (h *Handler) Unread(c *gin.Context) {
	items, err := h.service.Unread(c.Request.Context(), auth.UserID(c.Request.Context()))
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, map[string]any{"unread": items})
}

func (h *Handler) History(c *gin.Context) {
	after, err := strconv.ParseUint(c.Request.URL.Query().Get("after_id"), 10, 64)
	if c.Request.URL.Query().Get("after_id") == "" {
		after = 0
	} else if err != nil {
		httpx.Fail(c, httpx.Error(http.StatusBadRequest, "invalid_cursor", "after_id must be an unsigned integer"))
		return
	}
	limit := 200
	if raw := c.Request.URL.Query().Get("limit"); raw != "" {
		limit, err = strconv.Atoi(raw)
		if err != nil || limit < 1 || limit > 500 {
			httpx.Fail(c, httpx.Error(http.StatusBadRequest, "invalid_limit", "limit must be between 1 and 500"))
			return
		}
	}
	items, err := h.service.History(c.Request.Context(), auth.UserID(c.Request.Context()), c.Request.URL.Query().Get("peer_id"), after, limit)
	if err != nil {
		httpx.Fail(c, err)
		return
	}
	httpx.Data(c, http.StatusOK, map[string]any{"messages": items})
}
func (h *Handler) WebSocket(c *gin.Context) {
	userID, ok := h.tickets.Consume(c.Request.URL.Query().Get("ticket"))
	if !ok {
		httpx.Fail(c, httpx.Error(http.StatusUnauthorized, "invalid_ws_ticket", "WebSocket ticket is invalid, expired or already used"))
		return
	}
	select {
	case h.slots <- struct{}{}:
		defer func() { <-h.slots }()
	default:
		httpx.Fail(c, httpx.Error(http.StatusServiceUnavailable, "connection_limit", "too many WebSocket connections"))
		return
	}
	conn, err := h.upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}
	wsClient := &client{conn: conn, send: make(chan *impb.Envelope, 64), done: make(chan struct{})}
	if !h.hub.add(userID, wsClient) {
		wsClient.close()
		return
	}
	defer h.hub.remove(userID, wsClient)
	defer wsClient.close()
	go wsClient.writer()
	conn.SetReadLimit(300 << 10)
	_ = conn.SetReadDeadline(time.Now().Add(75 * time.Second))
	conn.SetPongHandler(func(string) error { return conn.SetReadDeadline(time.Now().Add(75 * time.Second)) })
	for {
		kind, data, err := conn.ReadMessage()
		if err != nil {
			return
		}
		if kind != websocket.BinaryMessage {
			h.sendError(wsClient, "invalid_frame", "binary protobuf frames required", "")
			continue
		}
		_ = conn.SetReadDeadline(time.Now().Add(75 * time.Second))
		e := &impb.Envelope{}
		if err := proto.Unmarshal(data, e); err != nil {
			h.sendError(wsClient, "invalid_envelope", "invalid protobuf envelope", "")
			continue
		}
		if e.Type == impb.Envelope_PING {
			wsClient.enqueue(&impb.Envelope{Version: protocolVersion, Type: impb.Envelope_PONG, Timestamp: time.Now().UnixMilli()})
			continue
		}
		ctx, cancel := context.WithTimeout(c.Request.Context(), 10*time.Second)
		saved, err := h.service.SaveMessage(ctx, userID, e)
		cancel()
		if err != nil {
			h.sendError(wsClient, "message_rejected", "message rejected", e.MessageId)
			h.log.Warn("message rejected", "user_id", userID, "message_id", e.MessageId, "error", err)
			continue
		}
		if !saved.Duplicate {
			h.hub.Publish(saved.Envelope.RecipientId, saved.Envelope)
		}
		if !wsClient.enqueue(Ack(saved)) {
			return
		}
	}
}
func (h *Handler) sendError(c *client, code, message, messageID string) {
	payload, _ := proto.Marshal(&impb.ErrorPayload{Code: code, Message: message})
	e := &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_ERROR, MessageId: messageID, Timestamp: time.Now().UnixMilli(), Payload: payload}
	c.enqueue(e)
}
