package chat

import (
	"context"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"time"

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

func NewHandler(service *Service, tickets *TicketStore, hub *Hub, log *slog.Logger, maxConnections ...int) *Handler {
	limit := 1000
	if len(maxConnections) > 0 && maxConnections[0] > 0 {
		limit = maxConnections[0]
	}
	h := &Handler{service: service, tickets: tickets, hub: hub, log: log, slots: make(chan struct{}, limit)}
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
func (h *Handler) Ticket(w http.ResponseWriter, r *http.Request) {
	ticket, err := h.tickets.Issue(auth.UserID(r.Context()))
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 201, map[string]any{"ticket": ticket, "expires_in": int64(h.tickets.ttl.Seconds())})
}
func (h *Handler) Presence(w http.ResponseWriter, r *http.Request) {
	httpx.Data(w, 200, map[string]any{"online_user_ids": h.hub.OnlineUsers()})
}

func (h *Handler) MarkRead(w http.ResponseWriter, r *http.Request) {
	var input struct {
		PeerID string `json:"peer_id"`
		Cursor uint64 `json:"cursor"`
	}
	if err := httpx.Decode(w, r, &input); err != nil {
		httpx.Fail(w, r, err)
		return
	}
	readerID := auth.UserID(r.Context())
	receipt, err := h.service.MarkRead(r.Context(), readerID, input.PeerID, input.Cursor)
	if err != nil {
		httpx.Fail(w, r, err)
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
	httpx.Data(w, 200, receipt)
}

func (h *Handler) History(w http.ResponseWriter, r *http.Request) {
	after, err := strconv.ParseUint(r.URL.Query().Get("after_id"), 10, 64)
	if r.URL.Query().Get("after_id") == "" {
		after = 0
	} else if err != nil {
		httpx.Fail(w, r, httpx.Error(400, "invalid_cursor", "after_id must be an unsigned integer"))
		return
	}
	limit := 200
	if raw := r.URL.Query().Get("limit"); raw != "" {
		limit, err = strconv.Atoi(raw)
		if err != nil || limit < 1 || limit > 500 {
			httpx.Fail(w, r, httpx.Error(400, "invalid_limit", "limit must be between 1 and 500"))
			return
		}
	}
	items, err := h.service.History(r.Context(), auth.UserID(r.Context()), r.URL.Query().Get("peer_id"), after, limit)
	if err != nil {
		httpx.Fail(w, r, err)
		return
	}
	httpx.Data(w, 200, map[string]any{"messages": items})
}
func (h *Handler) WebSocket(w http.ResponseWriter, r *http.Request) {
	userID, ok := h.tickets.Consume(r.URL.Query().Get("ticket"))
	if !ok {
		httpx.Fail(w, r, httpx.Error(401, "invalid_ws_ticket", "WebSocket ticket is invalid, expired or already used"))
		return
	}
	select {
	case h.slots <- struct{}{}:
		defer func() { <-h.slots }()
	default:
		httpx.Fail(w, r, httpx.Error(503, "connection_limit", "too many WebSocket connections"))
		return
	}
	conn, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	c := &client{conn: conn, send: make(chan *impb.Envelope, 64), done: make(chan struct{})}
	if !h.hub.add(userID, c) {
		c.close()
		return
	}
	defer h.hub.remove(userID, c)
	defer c.close()
	go c.writer()
	conn.SetReadLimit(300 << 10)
	_ = conn.SetReadDeadline(time.Now().Add(75 * time.Second))
	conn.SetPongHandler(func(string) error { return conn.SetReadDeadline(time.Now().Add(75 * time.Second)) })
	for {
		kind, data, err := conn.ReadMessage()
		if err != nil {
			return
		}
		if kind != websocket.BinaryMessage {
			h.sendError(c, "invalid_frame", "binary protobuf frames required")
			continue
		}
		_ = conn.SetReadDeadline(time.Now().Add(75 * time.Second))
		e := &impb.Envelope{}
		if err := proto.Unmarshal(data, e); err != nil {
			h.sendError(c, "invalid_envelope", "invalid protobuf envelope")
			continue
		}
		if e.Type == impb.Envelope_PING {
			c.enqueue(&impb.Envelope{Version: protocolVersion, Type: impb.Envelope_PONG, Timestamp: time.Now().UnixMilli()})
			continue
		}
		ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
		saved, err := h.service.SaveMessage(ctx, userID, e)
		cancel()
		if err != nil {
			h.sendError(c, "message_rejected", "message rejected")
			h.log.Warn("message rejected", "user_id", userID, "error", err)
			continue
		}
		if !saved.Duplicate {
			h.hub.Publish(saved.Envelope.RecipientId, saved.Envelope)
		}
		if !c.enqueue(Ack(saved)) {
			return
		}
	}
}
func (h *Handler) sendError(c *client, code, message string) {
	payload, _ := proto.Marshal(&impb.ErrorPayload{Code: code, Message: message})
	c.enqueue(&impb.Envelope{Version: protocolVersion, Type: impb.Envelope_ERROR, Timestamp: time.Now().UnixMilli(), Payload: payload})
}
