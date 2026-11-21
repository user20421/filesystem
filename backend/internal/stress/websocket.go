package stress

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"math/rand"
	"sync"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

const protocolVersion = 2

type WSClient struct {
	user      User
	api       *APIClient
	metrics   *Metrics
	writeMu   sync.Mutex
	conn      *websocket.Conn
	dataError func(error)
}

func NewWSClient(user User, api *APIClient, metrics *Metrics, dataError func(error)) *WSClient {
	return &WSClient{user: user, api: api, metrics: metrics, dataError: dataError}
}

func (c *WSClient) Run(ctx context.Context) {
	random := newRandom(c.user.ID + ":reconnect")
	connectedOnce := false
	for ctx.Err() == nil {
		ticketCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
		ticket, err := c.api.Ticket(ticketCtx, c.user.Token)
		cancel()
		if err == nil {
			var conn *websocket.Conn
			conn, err = c.api.WebSocket(ctx, ticket)
			if err == nil {
				c.setConn(conn)
				if connectedOnce {
					c.metrics.Reconnected()
				}
				connectedOnce = true
				c.readLoop(ctx, conn)
				c.clearConn(conn)
			}
		}
		if err != nil && ctx.Err() == nil {
			c.metrics.ConnectionFailed()
		}
		if ctx.Err() != nil {
			break
		}
		delay := time.Duration(250+random.Intn(750)) * time.Millisecond
		select {
		case <-ctx.Done():
			return
		case <-time.After(delay):
		}
	}
}
func (c *WSClient) setConn(conn *websocket.Conn) {
	c.writeMu.Lock()
	c.conn = conn
	c.metrics.SetOnline(1)
	c.writeMu.Unlock()
}
func (c *WSClient) clearConn(conn *websocket.Conn) {
	c.writeMu.Lock()
	if c.conn == conn {
		c.conn = nil
		c.metrics.SetOnline(-1)
	}
	_ = conn.Close()
	c.writeMu.Unlock()
}
func (c *WSClient) Close() {
	c.writeMu.Lock()
	if c.conn != nil {
		_ = c.conn.Close()
	}
	c.writeMu.Unlock()
}

func (c *WSClient) readLoop(ctx context.Context, conn *websocket.Conn) {
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()
	_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
	for {
		kind, raw, err := conn.ReadMessage()
		if err != nil {
			return
		}
		_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		if kind != websocket.BinaryMessage {
			c.dataError(fmt.Errorf("user %s received non-binary WebSocket frame", c.user.Username))
			continue
		}
		var envelope impb.Envelope
		if err = proto.Unmarshal(raw, &envelope); err != nil {
			c.dataError(err)
			continue
		}
		now := time.Now()
		switch envelope.Type {
		case impb.Envelope_ACK:
			var payload impb.AckPayload
			if err := proto.Unmarshal(envelope.Payload, &payload); err != nil || payload.Cursor == 0 {
				c.dataError(fmt.Errorf("invalid ACK payload for %s", envelope.MessageId))
				continue
			}
			c.metrics.MessageAck(envelope.MessageId, now)
		case impb.Envelope_MESSAGE:
			c.metrics.MessageDelivered(envelope.MessageId, now)
		case impb.Envelope_ERROR:
			var payload impb.ErrorPayload
			if err := proto.Unmarshal(envelope.Payload, &payload); err != nil {
				c.dataError(fmt.Errorf("invalid ERROR payload for %s", envelope.MessageId))
				continue
			}
			c.dataError(fmt.Errorf("server ERROR for %s: %s: %s", envelope.MessageId, payload.Code, payload.Message))
		case impb.Envelope_PING:
			pong := &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_PONG, MessageId: envelope.MessageId}
			_ = c.Send(pong)
		}
	}
}

func (c *WSClient) Send(envelope *impb.Envelope) error {
	raw, err := proto.Marshal(envelope)
	if err != nil {
		return err
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	if c.conn == nil {
		return fmt.Errorf("websocket offline")
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if err = c.conn.WriteMessage(websocket.BinaryMessage, raw); err != nil {
		_ = c.conn.Close()
		return err
	}
	return nil
}
func (c *WSClient) Heartbeat(ctx context.Context) {
	random := newRandom(c.user.ID + ":heartbeat")
	delay := time.Duration(15+random.Intn(26)) * time.Second
	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(delay):
			_ = c.Send(&impb.Envelope{Version: protocolVersion, Type: impb.Envelope_PING, MessageId: uuid.NewString(), DeviceId: "stress-" + c.user.Username})
			delay = time.Duration(15+random.Intn(26)) * time.Second
		}
	}
}
func (c *WSClient) Chat(ctx context.Context, partner User, rate float64) {
	if rate <= 0 {
		return
	}
	random := newRandom(c.user.ID + ":chat")
	for {
		wait := time.Duration(random.ExpFloat64() / rate * float64(time.Second))
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
			id := uuid.NewString()
			now := time.Now()
			payload, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{Text: fmt.Sprintf("stress %s %d", id, now.UnixNano())}})
			if err != nil {
				c.dataError(err)
				continue
			}
			envelope := &impb.Envelope{Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: id, SenderId: c.user.ID, RecipientId: partner.ID, DeviceId: "stress-" + c.user.Username, Timestamp: now.UnixMilli(), Payload: payload}
			// Register before writing: a fast ACK/push can arrive before WriteMessage returns.
			c.metrics.MessageSent(id, now)
			_ = c.Send(envelope)
		}
	}
}
func newRandom(key string) *rand.Rand {
	sum := sha256.Sum256([]byte(key))
	seed := int64(binary.BigEndian.Uint64(sum[:8])) ^ time.Now().UnixNano()
	return rand.New(rand.NewSource(seed))
}

func IsNormalWSClose(err error) bool {
	return errors.Is(err, context.Canceled) || websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway)
}
