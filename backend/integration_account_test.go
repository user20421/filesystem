package backend_test

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	_ "github.com/go-sql-driver/mysql"
	"github.com/google/uuid"
	"github.com/gorilla/websocket"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"secure-im/backend/internal/proto/impb"
)

func TestRealAccountDeletionIntegration(t *testing.T) {
	baseURL := os.Getenv("SECURE_IM_INTEGRATION_URL")
	if baseURL == "" {
		t.Skip("set SECURE_IM_INTEGRATION_URL to run against a live backend")
	}
	dsn := os.Getenv("INTEGRATION_MYSQL_DSN")
	if dsn == "" {
		t.Fatal("INTEGRATION_MYSQL_DSN is required for account deletion verification")
	}
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	if err := db.PingContext(ctx); err != nil {
		t.Fatal(err)
	}

	c := &integrationClient{t: t, baseURL: strings.TrimRight(baseURL, "/"), http: &http.Client{Timeout: 20 * time.Second}}
	suffix := strings.ReplaceAll(uuid.NewString()[:8], "-", "")
	deleted := c.register("delete_"+suffix, "Deleted")
	peer := c.register("peer_"+suffix, "Peer")
	other := c.register("other_"+suffix, "Other")
	deletedWS := c.websocket(deleted.Tokens.AccessToken)
	defer deletedWS.Close()
	deletedSecondWS := c.websocket(deleted.Tokens.AccessToken)
	defer deletedSecondWS.Close()
	peerWS := c.websocket(peer.Tokens.AccessToken)
	defer peerWS.Close()

	bidirectionalMessage(t, deletedWS, peerWS, deleted.User.ID, peer.User.ID, "from deleted")
	bidirectionalMessage(t, peerWS, deletedWS, peer.User.ID, deleted.User.ID, "to deleted")
	unrelatedMessage := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: other.User.ID, DeviceId: "peer-device", Payload: []byte("unrelated")}
	sendEnvelope(t, peerWS, unrelatedMessage)
	assertAck(t, readEnvelope(t, peerWS), unrelatedMessage.MessageId)

	outgoing := c.integrationUploadedFile(deleted, peer, "outgoing.bin")
	incoming := c.integrationUploadedFile(peer, deleted, "incoming.bin")
	unrelated := c.integrationUploadedFile(peer, other, "unrelated.bin")
	c.requestJSON(http.MethodPost, "/api/v1/files/"+unrelated.ID+"/complete", peer.Tokens.AccessToken, map[string]any{}, http.StatusOK)
	for _, id := range []string{outgoing.ID, incoming.ID, unrelated.ID} {
		if got := countObjects(t, "files/"+id+"/"); got != 1 {
			t.Fatalf("file %s has %d objects before deletion, want 1", id, got)
		}
	}
	deletedTicket := c.issueTicket(deleted.Tokens.AccessToken)
	peerTicket := c.issueTicket(peer.Tokens.AccessToken)

	c.requestJSON(http.MethodDelete, "/api/v1/users/me", "", nil, http.StatusUnauthorized)
	c.requestJSON(http.MethodDelete, "/api/v1/users/me", deleted.Tokens.AccessToken, nil, http.StatusNoContent)
	c.requestJSON(http.MethodGet, "/api/v1/users/me", deleted.Tokens.AccessToken, nil, http.StatusUnauthorized)
	c.requestJSON(http.MethodDelete, "/api/v1/users/me", deleted.Tokens.AccessToken, nil, http.StatusUnauthorized)
	c.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": deleted.Tokens.RefreshToken}, http.StatusUnauthorized)
	c.requestJSON(http.MethodPost, "/api/v1/auth/login", "", map[string]any{"username": deleted.User.Username, "password": "integration-password-123"}, http.StatusUnauthorized)
	assertWebSocketClosed(t, deletedWS)
	assertWebSocketClosed(t, deletedSecondWS)
	assertTicketStatus(t, c.baseURL, deletedTicket, http.StatusUnauthorized)
	peerSpareWS := dialIntegrationTicket(t, c.baseURL, peerTicket)
	defer peerSpareWS.Close()

	c.requestJSON(http.MethodGet, "/api/v1/users/me", peer.Tokens.AccessToken, nil, http.StatusOK)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+outgoing.ID, peer.Tokens.AccessToken, nil, http.StatusNotFound)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+incoming.ID, peer.Tokens.AccessToken, nil, http.StatusNotFound)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+unrelated.ID, other.Tokens.AccessToken, nil, http.StatusOK)
	if downloaded := c.download(other.Tokens.AccessToken, unrelated.ID, http.StatusOK); string(downloaded) != "unrelated" {
		t.Fatalf("unrelated file changed: %q", downloaded)
	}
	for _, id := range []string{outgoing.ID, incoming.ID} {
		if got := countObjects(t, "files/"+id+"/"); got != 0 {
			t.Fatalf("deleted file %s still has %d MinIO objects", id, got)
		}
	}
	if got := countObjects(t, "files/"+unrelated.ID+"/"); got != 1 {
		t.Fatalf("unrelated file has %d MinIO objects after deletion, want 1", got)
	}

	afterDelete := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: other.User.ID, DeviceId: "peer-device", Payload: []byte("peer remains active")}
	sendEnvelope(t, peerWS, afterDelete)
	assertAck(t, readEnvelope(t, peerWS), afterDelete.MessageId)
	if got := len(c.history(other.Tokens.AccessToken, peer.User.ID)); got != 2 {
		t.Fatalf("unrelated conversation has %d messages, want 2", got)
	}

	assertDBCount(t, db, `SELECT COUNT(1) FROM users WHERE id=?`, 0, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM refresh_tokens WHERE user_id=?`, 0, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM messages WHERE sender_id=? OR recipient_id=?`, 0, deleted.User.ID, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM conversations WHERE user_low_id=? OR user_high_id=?`, 0, deleted.User.ID, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM files WHERE owner_id=? OR recipient_id=?`, 0, deleted.User.ID, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM file_chunks c JOIN files f ON f.id=c.file_id WHERE f.owner_id=? OR f.recipient_id=?`, 0, deleted.User.ID, deleted.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM users WHERE id IN (?,?)`, 2, peer.User.ID, other.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM messages WHERE (sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)`, 2, peer.User.ID, other.User.ID, other.User.ID, peer.User.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM files WHERE id=?`, 1, unrelated.ID)
	assertDBCount(t, db, `SELECT COUNT(1) FROM file_chunks WHERE file_id=?`, 1, unrelated.ID)
}

func bidirectionalMessage(t *testing.T, from, to *websocket.Conn, senderID, recipientID, payload string) {
	t.Helper()
	message := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: recipientID, DeviceId: "account-integration", Payload: []byte(payload)}
	sendEnvelope(t, from, message)
	assertAck(t, readEnvelope(t, from), message.MessageId)
	delivered := readEnvelope(t, to)
	if delivered.Type != impb.Envelope_MESSAGE || delivered.SenderId != senderID || delivered.MessageId != message.MessageId {
		t.Fatalf("unexpected bidirectional message: %v", delivered)
	}
}

func (c *integrationClient) integrationUploadedFile(owner, recipient integrationAuth, name string) integrationFile {
	plaintext := []byte(strings.TrimSuffix(name, ".bin"))
	digest := fmt.Sprintf("%x", sha256.Sum256(plaintext))
	file := c.initFile(owner, recipient, name, int64(len(plaintext)), digest, 64<<10, http.StatusCreated)
	c.putChunk(owner.Tokens.AccessToken, file.ID, 0, plaintext, http.StatusOK)
	return file
}

func (c *integrationClient) issueTicket(accessToken string) string {
	body := c.requestJSON(http.MethodPost, "/api/v1/ws-ticket", accessToken, map[string]any{}, http.StatusCreated)
	var result struct {
		Ticket string `json:"ticket"`
	}
	decodeData(c.t, body, &result)
	return result.Ticket
}

func assertWebSocketClosed(t *testing.T, conn *websocket.Conn) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			if netErr, ok := err.(interface{ Timeout() bool }); ok && netErr.Timeout() {
				t.Fatal("deleted user's WebSocket remained open")
			}
			return
		}
		// A second device may still have a delivery queued ahead of closure.
	}
}

func integrationWSURL(baseURL, ticket string) string {
	return "ws" + strings.TrimPrefix(baseURL, "http") + "/ws?ticket=" + ticket
}

func assertTicketStatus(t *testing.T, baseURL, ticket string, want int) {
	t.Helper()
	conn, response, err := websocket.DefaultDialer.Dial(integrationWSURL(baseURL, ticket), nil)
	if conn != nil {
		conn.Close()
	}
	if response != nil {
		response.Body.Close()
	}
	if err == nil || response == nil || response.StatusCode != want {
		t.Fatalf("revoked ticket dial error=%v response=%v, want HTTP %d", err, response, want)
	}
}

func dialIntegrationTicket(t *testing.T, baseURL, ticket string) *websocket.Conn {
	t.Helper()
	conn, response, err := websocket.DefaultDialer.Dial(integrationWSURL(baseURL, ticket), nil)
	if err != nil {
		if response != nil {
			t.Fatalf("other user's ticket failed with HTTP %d: %v", response.StatusCode, err)
		}
		t.Fatalf("other user's ticket failed: %v", err)
	}
	return conn
}

func countObjects(t *testing.T, prefix string) int {
	t.Helper()
	client, err := minio.New(os.Getenv("INTEGRATION_MINIO_ENDPOINT"), &minio.Options{
		Creds: credentials.NewStaticV4(os.Getenv("INTEGRATION_MINIO_ACCESS_KEY"), os.Getenv("INTEGRATION_MINIO_SECRET_KEY"), ""),
	})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	count := 0
	for object := range client.ListObjects(ctx, os.Getenv("INTEGRATION_MINIO_BUCKET"), minio.ListObjectsOptions{Prefix: prefix, Recursive: true}) {
		if object.Err != nil {
			t.Fatal(object.Err)
		}
		count++
	}
	return count
}

func assertDBCount(t *testing.T, db *sql.DB, query string, want int, args ...any) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	var got int
	if err := db.QueryRowContext(ctx, query, args...).Scan(&got); err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("database count = %d, want %d; query=%s args=%v", got, want, query, args)
	}
}
