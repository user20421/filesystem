package backend_test

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"net/http"
	"sort"
	"testing"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

type integrationReadReceipt struct {
	ConversationID string `json:"conversation_id"`
	PeerID         string `json:"peer_id"`
	Cursor         uint64 `json:"cursor"`
	ReadAt         int64  `json:"read_at"`
	Updated        int64  `json:"updated"`
}

type integrationHistoryItem struct {
	Cursor   uint64 `json:"cursor"`
	Envelope string `json:"envelope"`
}

func (c *integrationClient) verifyReadReceipts(suffix string) {
	alice := c.register("readalice_"+suffix, "Read Alice")
	bob := c.register("readbob_"+suffix, "Read Bob")
	mallory := c.register("readmallory_"+suffix, "Read Mallory")
	c.requestJSON(http.MethodGet, "/api/v1/presence", "", nil, http.StatusUnauthorized)
	for _, id := range []string{alice.User.ID, bob.User.ID} {
		if containsString(c.presence(alice.Tokens.AccessToken), id) {
			c.t.Fatalf("user %s online before WebSocket connection", id)
		}
	}

	aliceOne := c.websocket(alice.Tokens.AccessToken)
	defer aliceOne.Close()
	aliceTwo := c.websocket(alice.Tokens.AccessToken)
	defer aliceTwo.Close()
	bobOne := c.websocket(bob.Tokens.AccessToken)
	defer bobOne.Close()
	bobTwo := c.websocket(bob.Tokens.AccessToken)
	defer bobTwo.Close()
	online := c.presence(alice.Tokens.AccessToken)
	if !sort.StringsAreSorted(online) || !containsString(online, alice.User.ID) || !containsString(online, bob.User.ID) {
		c.t.Fatalf("unexpected multi-device presence: %v", online)
	}

	messages := make([]*impb.Envelope, 3)
	cursors := make([]uint64, 3)
	conversationID := ""
	for i := range messages {
		messages[i] = &impb.Envelope{
			Version: 2, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(),
			RecipientId: bob.User.ID, DeviceId: "read-integration-device",
			Timestamp: time.Now().UnixMilli(), Payload: textMessagePayload(c.t, fmt.Sprintf("read batch message %d", i+1)),
		}
		sendEnvelope(c.t, aliceOne, messages[i])
		ack := readEnvelopeType(c.t, aliceOne, impb.Envelope_ACK)
		assertAck(c.t, ack, messages[i].MessageId)
		cursors[i] = parseCursor(c.t, ack.Payload)
		for _, conn := range []*websocket.Conn{bobOne, bobTwo} {
			delivered := readEnvelopeType(c.t, conn, impb.Envelope_MESSAGE)
			if delivered.MessageId != messages[i].MessageId || !bytes.Equal(delivered.Payload, messages[i].Payload) {
				c.t.Fatalf("unexpected delivered message %d: %v", i+1, delivered)
			}
			if conversationID == "" {
				conversationID = delivered.ConversationId
			} else if delivered.ConversationId != conversationID {
				c.t.Fatal("direct messages used different conversations")
			}
		}
	}

	// Missing, third-party, reverse-direction and foreign-peer cursors are all forbidden.
	for _, attempt := range []struct {
		token  string
		peerID string
		cursor uint64
	}{
		{mallory.Tokens.AccessToken, alice.User.ID, cursors[0]},
		{bob.Tokens.AccessToken, mallory.User.ID, cursors[0]},
		{alice.Tokens.AccessToken, bob.User.ID, cursors[1]},
		{bob.Tokens.AccessToken, alice.User.ID, ^uint64(0)},
	} {
		c.requestJSON(http.MethodPost, "/api/v1/messages/read", attempt.token, map[string]any{
			"peer_id": attempt.peerID, "cursor": attempt.cursor,
		}, http.StatusForbidden)
	}

	receipt := c.markRead(bob.Tokens.AccessToken, alice.User.ID, cursors[1])
	if receipt.ConversationID != conversationID || receipt.PeerID != alice.User.ID || receipt.Cursor != cursors[1] || receipt.ReadAt <= 0 || receipt.Updated != 2 {
		c.t.Fatalf("unexpected first read receipt: %+v", receipt)
	}
	connections := []*websocket.Conn{aliceOne, aliceTwo, bobOne, bobTwo}
	assertReadBroadcast(c.t, connections, alice.User.ID, bob.User.ID, receipt)

	repeated := c.markRead(bob.Tokens.AccessToken, alice.User.ID, cursors[1])
	if repeated.ReadAt != receipt.ReadAt || repeated.Updated != 0 || repeated.ConversationID != receipt.ConversationID {
		c.t.Fatalf("non-idempotent read receipt: first=%+v repeated=%+v", receipt, repeated)
	}
	assertReadBroadcast(c.t, connections, alice.User.ID, bob.User.ID, repeated)

	// Removing one device preserves presence; removing the last device clears it.
	_ = bobTwo.Close()
	c.waitPresence(alice.Tokens.AccessToken, bob.User.ID, true)
	_ = bobOne.Close()
	c.waitPresence(alice.Tokens.AccessToken, bob.User.ID, false)
	_ = aliceTwo.Close()
	c.waitPresence(bob.Tokens.AccessToken, alice.User.ID, true)
	_ = aliceOne.Close()
	c.waitPresence(bob.Tokens.AccessToken, alice.User.ID, false)

	// Both participants are offline: history must expose the same persisted read_at.
	for _, history := range [][]integrationHistoryItem{
		c.history(alice.Tokens.AccessToken, bob.User.ID),
		c.history(bob.Tokens.AccessToken, alice.User.ID),
	} {
		if len(history) != 3 {
			c.t.Fatalf("offline history has %d messages, want 3", len(history))
		}
		for i, item := range history {
			if item.Cursor != cursors[i] {
				c.t.Fatalf("history cursor %d = %d, want %d", i, item.Cursor, cursors[i])
			}
			envelope := decodeHistoryEnvelope(c.t, item)
			if envelope.MessageId != messages[i].MessageId {
				c.t.Fatalf("history message %d = %s, want %s", i, envelope.MessageId, messages[i].MessageId)
			}
			if i < 2 && envelope.ReadAt != receipt.ReadAt {
				c.t.Fatalf("message %d read_at = %d, want %d", i+1, envelope.ReadAt, receipt.ReadAt)
			}
			if i == 2 && envelope.ReadAt != 0 {
				c.t.Fatalf("third message unexpectedly read at %d", envelope.ReadAt)
			}
		}
	}
	c.t.Logf("verified READ on four connections, two messages updated idempotently, third unread; read_at=%d", receipt.ReadAt)
}

func (c *integrationClient) markRead(token, peerID string, cursor uint64) integrationReadReceipt {
	body := c.requestJSON(http.MethodPost, "/api/v1/messages/read", token, map[string]any{
		"peer_id": peerID, "cursor": cursor,
	}, http.StatusOK)
	var receipt integrationReadReceipt
	decodeData(c.t, body, &receipt)
	return receipt
}

func (c *integrationClient) presence(token string) []string {
	body := c.requestJSON(http.MethodGet, "/api/v1/presence", token, nil, http.StatusOK)
	var result struct {
		OnlineUserIDs []string `json:"online_user_ids"`
	}
	decodeData(c.t, body, &result)
	return result.OnlineUserIDs
}

func (c *integrationClient) waitPresence(token, userID string, want bool) {
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if containsString(c.presence(token), userID) == want {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	c.t.Fatalf("presence for %s did not become %t", userID, want)
}

func (c *integrationClient) history(token, peerID string) []integrationHistoryItem {
	body := c.requestJSON(http.MethodGet, "/api/v1/messages?peer_id="+peerID+"&after_id=0&limit=20", token, nil, http.StatusOK)
	var result struct {
		Messages []integrationHistoryItem `json:"messages"`
	}
	decodeData(c.t, body, &result)
	return result.Messages
}

func decodeHistoryEnvelope(t *testing.T, item integrationHistoryItem) *impb.Envelope {
	t.Helper()
	binary, err := base64.StdEncoding.DecodeString(item.Envelope)
	if err != nil {
		t.Fatal(err)
	}
	var envelope impb.Envelope
	if err := proto.Unmarshal(binary, &envelope); err != nil {
		t.Fatal(err)
	}
	return &envelope
}

func assertReadBroadcast(t *testing.T, connections []*websocket.Conn, senderID, readerID string, receipt integrationReadReceipt) {
	t.Helper()
	for i, conn := range connections {
		event := readEnvelopeType(t, conn, impb.Envelope_READ)
		if event.Version != 2 || event.SenderId != readerID || event.RecipientId != senderID ||
			event.ConversationId != receipt.ConversationID || event.Cursor != receipt.Cursor ||
			event.ReadAt != receipt.ReadAt || event.Timestamp != receipt.ReadAt {
			t.Fatalf("connection %d received invalid READ: %v", i, event)
		}
	}
}

func readEnvelopeType(t *testing.T, conn *websocket.Conn, want impb.Envelope_Type) *impb.Envelope {
	t.Helper()
	for {
		envelope := readEnvelope(t, conn)
		if envelope.Type == want {
			return envelope
		}
		// READ and keepalive frames are independent of MESSAGE/ACK assertions.
		if envelope.Type != impb.Envelope_READ && envelope.Type != impb.Envelope_PING && envelope.Type != impb.Envelope_PONG {
			t.Fatalf("got envelope type %s while waiting for %s: %v", envelope.Type, want, envelope)
		}
	}
}

func containsString(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}
