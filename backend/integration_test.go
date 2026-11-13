package backend_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

type integrationUser struct {
	ID          string `json:"id"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
}
type integrationTokens struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	TokenType    string `json:"token_type"`
	ExpiresIn    int64  `json:"expires_in"`
}
type integrationAuth struct {
	User   integrationUser   `json:"user"`
	Tokens integrationTokens `json:"tokens"`
}
type integrationFile struct {
	ID          string `json:"id"`
	OwnerID     string `json:"owner_id"`
	RecipientID string `json:"recipient_id"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	ChunkSize   int64  `json:"chunk_size"`
	ChunkCount  int    `json:"chunk_count"`
	Status      string `json:"status"`
}
type integrationClient struct {
	t       *testing.T
	baseURL string
	http    *http.Client
}

func TestRealAPIIntegration(t *testing.T) {
	baseURL := os.Getenv("SECURE_IM_INTEGRATION_URL")
	if baseURL == "" {
		t.Skip("set SECURE_IM_INTEGRATION_URL to run against a live backend")
	}
	c := &integrationClient{t: t, baseURL: strings.TrimRight(baseURL, "/"), http: &http.Client{Timeout: 20 * time.Second}}
	suffix := strings.ReplaceAll(uuid.NewString()[:8], "-", "")
	alice := c.register("aLiCe_"+suffix, "Alice")
	bob := c.register("bOb_"+suffix, "Bob")
	mallory := c.register("mAlLoRy_"+suffix, "Mallory")
	weak := c.registerWithPassword("wEaK_"+suffix, "Weak", "123456")
	weakLogin := c.login("wEaK_"+suffix, "123456")
	if weakLogin.User.Username != weak.User.Username {
		t.Fatalf("mixed-case login returned username %q, want %q", weakLogin.User.Username, weak.User.Username)
	}
	badPassword := c.requestJSON(http.MethodPost, "/api/v1/auth/register", "", map[string]any{"username": "short_" + suffix, "display_name": "Short", "password": "12345"}, http.StatusBadRequest)
	if !strings.Contains(string(badPassword), "password must be 6 to 128 bytes") {
		t.Fatalf("unexpected password validation error: %s", badPassword)
	}
	c.loginAndRefresh("AlIcE_"+suffix, alice.Tokens.RefreshToken)
	users := c.requestJSON(http.MethodGet, "/api/v1/users", alice.Tokens.AccessToken, nil, http.StatusOK)
	var userList struct {
		Users []integrationUser `json:"users"`
	}
	decodeData(t, users, &userList)
	if len(userList.Users) < 4 {
		t.Fatalf("expected at least four users, got %d", len(userList.Users))
	}
	for _, user := range userList.Users {
		if user.Username != strings.ToUpper(user.Username) {
			t.Fatalf("user list contains non-uppercase username %q", user.Username)
		}
	}
	t.Logf("auth response uses data.user and data.tokens; users response uses data.users (%d total)", len(userList.Users))

	aliceWS := c.websocket(alice.Tokens.AccessToken)
	defer aliceWS.Close()
	bobWS := c.websocket(bob.Tokens.AccessToken)
	defer bobWS.Close()
	bobObserver := c.websocket(bob.Tokens.AccessToken)
	first := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: bob.User.ID, DeviceId: "alice-integration-device", Timestamp: time.Now().UnixMilli(), Payload: []byte("hello Bob from Alice")}
	sendEnvelope(t, aliceWS, first)
	ack := readEnvelope(t, aliceWS)
	assertAck(t, ack, first.MessageId)
	firstCursor := parseCursor(t, ack.Payload)
	delivered := readEnvelopeType(t, bobWS, impb.Envelope_MESSAGE)
	observerDelivered := readEnvelopeType(t, bobObserver, impb.Envelope_MESSAGE)
	if observerDelivered.MessageId != first.MessageId {
		t.Fatalf("second device did not receive message: %v", observerDelivered)
	}
	if delivered.Type != impb.Envelope_MESSAGE || delivered.MessageId != first.MessageId || !bytes.Equal(delivered.Payload, first.Payload) || delivered.SenderId != alice.User.ID || delivered.ConversationId == "" {
		t.Fatalf("unexpected delivered envelope: %v", delivered)
	}
	sendEnvelope(t, aliceWS, first)
	duplicateAck := readEnvelope(t, aliceWS)
	assertAck(t, duplicateAck, first.MessageId)
	if got := parseCursor(t, duplicateAck.Payload); got != firstCursor {
		t.Fatalf("idempotent ACK cursor changed: %d != %d", got, firstCursor)
	}
	assertNoMessage(t, bobObserver)
	_ = bobObserver.Close()
	reply := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: alice.User.ID, DeviceId: "bob-integration-device", Payload: []byte("hello Alice from Bob")}
	sendEnvelope(t, bobWS, reply)
	assertAck(t, readEnvelope(t, bobWS), reply.MessageId)
	if got := readEnvelopeType(t, aliceWS, impb.Envelope_MESSAGE); got.MessageId != reply.MessageId || !bytes.Equal(got.Payload, reply.Payload) {
		t.Fatalf("unexpected reverse message: %v", got)
	}
	offline := &impb.Envelope{Version: 1, Type: impb.Envelope_MESSAGE, MessageId: uuid.NewString(), RecipientId: mallory.User.ID, DeviceId: "alice-integration-device", Payload: []byte("offline message for Mallory")}
	sendEnvelope(t, aliceWS, offline)
	assertAck(t, readEnvelope(t, aliceWS), offline.MessageId)
	historyBody := c.requestJSON(http.MethodGet, "/api/v1/messages?peer_id="+alice.User.ID+"&after_id=0&limit=20", mallory.Tokens.AccessToken, nil, http.StatusOK)
	var history struct {
		Messages []struct {
			Cursor   uint64 `json:"cursor"`
			Envelope string `json:"envelope"`
		} `json:"messages"`
	}
	decodeData(t, historyBody, &history)
	if len(history.Messages) != 1 {
		t.Fatalf("expected one offline history item, got %d", len(history.Messages))
	}
	historyBytes, err := base64.StdEncoding.DecodeString(history.Messages[0].Envelope)
	if err != nil {
		t.Fatal(err)
	}
	var historyEnvelope impb.Envelope
	if err := proto.Unmarshal(historyBytes, &historyEnvelope); err != nil {
		t.Fatal(err)
	}
	if historyEnvelope.MessageId != offline.MessageId || !bytes.Equal(historyEnvelope.Payload, offline.Payload) || history.Messages[0].Cursor == 0 {
		t.Fatalf("unexpected history item: cursor=%d envelope=%v", history.Messages[0].Cursor, &historyEnvelope)
	}
	t.Logf("history response uses data.messages[].cursor and base64 data.messages[].envelope; cursor=%d", history.Messages[0].Cursor)

	c.verifyReadReceipts(suffix)
	c.verifyFileTransfer(alice, bob, mallory)
	c.verifyActiveFileLimit(alice, bob)
}

func (c *integrationClient) register(username, displayName string) integrationAuth {
	return c.registerWithPassword(username, displayName, "integration-password-123")
}
func (c *integrationClient) registerWithPassword(username, displayName, password string) integrationAuth {
	body := c.requestJSON(http.MethodPost, "/api/v1/auth/register", "", map[string]any{"username": username, "display_name": displayName, "password": password}, http.StatusCreated)
	var result integrationAuth
	decodeData(c.t, body, &result)
	if result.User.ID == "" || result.Tokens.AccessToken == "" || result.Tokens.RefreshToken == "" {
		c.t.Fatalf("incomplete auth result: %s", body)
	}
	if result.User.Username != strings.ToUpper(username) {
		c.t.Fatalf("registration username = %q, want %q", result.User.Username, strings.ToUpper(username))
	}
	return result
}
func (c *integrationClient) login(username, password string) integrationAuth {
	body := c.requestJSON(http.MethodPost, "/api/v1/auth/login", "", map[string]any{"username": username, "password": password}, http.StatusOK)
	var result integrationAuth
	decodeData(c.t, body, &result)
	return result
}
func (c *integrationClient) loginAndRefresh(username, oldRefresh string) {
	login := c.login(username, "integration-password-123")
	body := c.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": login.Tokens.RefreshToken}, http.StatusOK)
	var refreshed integrationAuth
	decodeData(c.t, body, &refreshed)
	if refreshed.Tokens.RefreshToken == login.Tokens.RefreshToken {
		c.t.Fatal("refresh token was not rotated")
	}
	c.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": login.Tokens.RefreshToken}, http.StatusUnauthorized)
	body = c.requestJSON(http.MethodPost, "/api/v1/auth/refresh", "", map[string]any{"refresh_token": oldRefresh}, http.StatusOK)
	decodeData(c.t, body, &refreshed)
}
func (c *integrationClient) requestJSON(method, path, token string, input any, wantStatus int) []byte {
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			c.t.Fatal(err)
		}
		body = bytes.NewReader(encoded)
	}
	req, err := http.NewRequest(method, c.baseURL+path, body)
	if err != nil {
		c.t.Fatal(err)
	}
	if input != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := c.http.Do(req)
	if err != nil {
		c.t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 2<<20))
	if err != nil {
		c.t.Fatal(err)
	}
	if resp.StatusCode != wantStatus {
		c.t.Fatalf("%s %s: got status %d, want %d: %s", method, path, resp.StatusCode, wantStatus, data)
	}
	return data
}
func decodeData(t *testing.T, body []byte, output any) {
	t.Helper()
	var envelope struct {
		Data json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(body, &envelope); err != nil || len(envelope.Data) == 0 {
		t.Fatalf("invalid data envelope: %s (%v)", body, err)
	}
	if err := json.Unmarshal(envelope.Data, output); err != nil {
		t.Fatalf("invalid data payload: %s (%v)", body, err)
	}
}
func (c *integrationClient) websocket(accessToken string) *websocket.Conn {
	body := c.requestJSON(http.MethodPost, "/api/v1/ws-ticket", accessToken, map[string]any{}, http.StatusCreated)
	var ticket struct {
		Ticket    string `json:"ticket"`
		ExpiresIn int64  `json:"expires_in"`
	}
	decodeData(c.t, body, &ticket)
	wsURL := "ws" + strings.TrimPrefix(c.baseURL, "http") + "/ws?ticket=" + ticket.Ticket
	conn, resp, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		if resp != nil {
			c.t.Fatalf("WebSocket dial failed: %v (status %d)", err, resp.StatusCode)
		}
		c.t.Fatal(err)
	}
	return conn
}
func sendEnvelope(t *testing.T, conn *websocket.Conn, envelope *impb.Envelope) {
	t.Helper()
	binary, err := proto.Marshal(envelope)
	if err != nil {
		t.Fatal(err)
	}
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, binary); err != nil {
		t.Fatal(err)
	}
}
func readEnvelope(t *testing.T, conn *websocket.Conn) *impb.Envelope {
	t.Helper()
	if err := conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	kind, binary, err := conn.ReadMessage()
	if err != nil {
		t.Fatal(err)
	}
	if kind != websocket.BinaryMessage {
		t.Fatalf("unexpected WebSocket frame type %d", kind)
	}
	var envelope impb.Envelope
	if err := proto.Unmarshal(binary, &envelope); err != nil {
		t.Fatal(err)
	}
	return &envelope
}
func assertAck(t *testing.T, envelope *impb.Envelope, messageID string) {
	t.Helper()
	if envelope.Type != impb.Envelope_ACK || envelope.MessageId != messageID {
		t.Fatalf("expected ACK for %s, got %v", messageID, envelope)
	}
}
func parseCursor(t *testing.T, payload []byte) uint64 {
	t.Helper()
	cursor, err := strconv.ParseUint(string(payload), 10, 64)
	if err != nil || cursor == 0 {
		t.Fatalf("invalid ACK cursor %q: %v", payload, err)
	}
	return cursor
}
func assertNoMessage(t *testing.T, conn *websocket.Conn) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	kind, binary, err := conn.ReadMessage()
	if err != nil {
		if netError, ok := err.(interface{ Timeout() bool }); ok && netError.Timeout() {
			return
		}
		t.Fatalf("unexpected WebSocket error while checking idempotency: %v", err)
	}
	t.Fatalf("duplicate or unexpected frame arrived: type=%d bytes=%x", kind, binary)
}

func (c *integrationClient) initFile(owner, recipient integrationAuth, name string, size int64, digest string, chunkSize int64, wantStatus int) integrationFile {
	body := c.requestJSON(http.MethodPost, "/api/v1/files/init", owner.Tokens.AccessToken, map[string]any{"filename": name, "size": size, "sha256": digest, "recipient_id": recipient.User.ID, "chunk_size": chunkSize}, wantStatus)
	if wantStatus != http.StatusCreated {
		return integrationFile{}
	}
	var file integrationFile
	decodeData(c.t, body, &file)
	if file.ID == "" || file.ChunkCount == 0 {
		c.t.Fatalf("invalid file init response: %s", body)
	}
	return file
}
func (c *integrationClient) putChunk(token, fileID string, index int, chunk []byte, wantStatus int) {
	req, err := http.NewRequest(http.MethodPut, fmt.Sprintf("%s/api/v1/files/%s/chunks/%d", c.baseURL, fileID, index), bytes.NewReader(chunk))
	if err != nil {
		c.t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/octet-stream")
	sum := sha256.Sum256(chunk)
	req.Header.Set("X-Chunk-SHA256", hex.EncodeToString(sum[:]))
	resp, err := c.http.Do(req)
	if err != nil {
		c.t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != wantStatus {
		c.t.Fatalf("upload chunk %d: got status %d, want %d: %s", index, resp.StatusCode, wantStatus, body)
	}
}
func (c *integrationClient) verifyFileTransfer(alice, bob, mallory integrationAuth) {
	plaintext := make([]byte, 150003)
	for i := range plaintext {
		plaintext[i] = byte((i*31 + 17) % 251)
	}
	sum := sha256.Sum256(plaintext)
	file := c.initFile(alice, bob, "integration-payload.bin", int64(len(plaintext)), hex.EncodeToString(sum[:]), 64<<10, http.StatusCreated)
	if file.ChunkCount != 3 {
		c.t.Fatalf("expected three chunks, got %d", file.ChunkCount)
	}
	c.t.Logf("file init requires filename/size/sha256/recipient_id/chunk_size and returns data as file metadata; id=%s chunks=%d", file.ID, file.ChunkCount)
	for index, offset := 0, 0; offset < len(plaintext); index++ {
		end := min(offset+int(file.ChunkSize), len(plaintext))
		c.putChunk(alice.Tokens.AccessToken, file.ID, index, plaintext[offset:end], http.StatusOK)
		offset = end
	}
	statusBody := c.requestJSON(http.MethodGet, "/api/v1/files/"+file.ID, bob.Tokens.AccessToken, nil, http.StatusOK)
	var status struct {
		File           integrationFile `json:"file"`
		UploadedChunks []int           `json:"uploaded_chunks"`
	}
	decodeData(c.t, statusBody, &status)
	if len(status.UploadedChunks) != 3 || status.File.Status != "uploading" {
		c.t.Fatalf("unexpected file status: %s", statusBody)
	}
	c.t.Logf("file status response has data.file and sibling data.uploaded_chunks: %v", status.UploadedChunks)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+file.ID, mallory.Tokens.AccessToken, nil, http.StatusForbidden)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+file.ID+"/download", mallory.Tokens.AccessToken, nil, http.StatusForbidden)
	c.putChunk(mallory.Tokens.AccessToken, file.ID, 0, plaintext[:file.ChunkSize], http.StatusForbidden)
	c.requestJSON(http.MethodPost, "/api/v1/files/"+file.ID+"/complete", bob.Tokens.AccessToken, map[string]any{}, http.StatusForbidden)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+file.ID+"/download", bob.Tokens.AccessToken, nil, http.StatusConflict)
	completedBody := c.requestJSON(http.MethodPost, "/api/v1/files/"+file.ID+"/complete", alice.Tokens.AccessToken, map[string]any{}, http.StatusOK)
	var completed integrationFile
	decodeData(c.t, completedBody, &completed)
	if completed.Status != "completed" {
		c.t.Fatalf("file not completed: %s", completedBody)
	}
	downloaded := c.download(bob.Tokens.AccessToken, file.ID, http.StatusOK)
	if !bytes.Equal(downloaded, plaintext) {
		c.t.Fatal("downloaded plaintext differs from uploaded content")
	}
	downloadSum := sha256.Sum256(downloaded)
	if hex.EncodeToString(downloadSum[:]) != file.SHA256 {
		c.t.Fatal("downloaded SHA256 differs from initialized SHA256")
	}
	c.verifyCiphertext(file, plaintext)
	c.requestJSON(http.MethodDelete, "/api/v1/files/"+file.ID, alice.Tokens.AccessToken, nil, http.StatusNoContent)
	c.requestJSON(http.MethodGet, "/api/v1/files/"+file.ID, bob.Tokens.AccessToken, nil, http.StatusNotFound)
}
func (c *integrationClient) download(token, fileID string, wantStatus int) []byte {
	req, err := http.NewRequest(http.MethodGet, c.baseURL+"/api/v1/files/"+fileID+"/download", nil)
	if err != nil {
		c.t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := c.http.Do(req)
	if err != nil {
		c.t.Fatal(err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 10<<20))
	if err != nil {
		c.t.Fatal(err)
	}
	if resp.StatusCode != wantStatus {
		c.t.Fatalf("download: got status %d, want %d: %s", resp.StatusCode, wantStatus, body)
	}
	return body
}
func (c *integrationClient) verifyCiphertext(file integrationFile, plaintext []byte) {
	endpoint := os.Getenv("INTEGRATION_MINIO_ENDPOINT")
	accessKey := os.Getenv("INTEGRATION_MINIO_ACCESS_KEY")
	secretKey := os.Getenv("INTEGRATION_MINIO_SECRET_KEY")
	bucket := os.Getenv("INTEGRATION_MINIO_BUCKET")
	if endpoint == "" || accessKey == "" || secretKey == "" || bucket == "" {
		c.t.Fatal("INTEGRATION_MINIO_ENDPOINT/ACCESS_KEY/SECRET_KEY/BUCKET are required for ciphertext verification")
	}
	client, err := minio.New(endpoint, &minio.Options{Creds: credentials.NewStaticV4(accessKey, secretKey, ""), Secure: false})
	if err != nil {
		c.t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	objects := client.ListObjects(ctx, bucket, minio.ListObjectsOptions{Prefix: "files/" + file.ID + "/", Recursive: true})
	count := 0
	for info := range objects {
		if info.Err != nil {
			c.t.Fatal(info.Err)
		}
		object, err := client.GetObject(ctx, bucket, info.Key, minio.GetObjectOptions{})
		if err != nil {
			c.t.Fatal(err)
		}
		ciphertext, err := io.ReadAll(io.LimitReader(object, int64(file.ChunkSize+17)))
		object.Close()
		if err != nil {
			c.t.Fatal(err)
		}
		if bytes.Contains(ciphertext, plaintext[:64]) || bytes.Contains(ciphertext, plaintext[64<<10:64<<10+64]) {
			c.t.Fatalf("MinIO object %s contains recognizable plaintext", info.Key)
		}
		if len(ciphertext) <= 16 {
			c.t.Fatalf("MinIO object %s is too short", info.Key)
		}
		count++
	}
	if count != file.ChunkCount {
		c.t.Fatalf("found %d encrypted objects, expected %d", count, file.ChunkCount)
	}
	c.t.Logf("verified %d MinIO objects are ciphertext, not plaintext", count)
}
func (c *integrationClient) verifyActiveFileLimit(alice, bob integrationAuth) {
	sum := sha256.Sum256([]byte{1})
	files := make([]integrationFile, 0, 3)
	defer func() {
		for _, file := range files {
			c.requestJSON(http.MethodDelete, "/api/v1/files/"+file.ID, alice.Tokens.AccessToken, nil, http.StatusNoContent)
		}
	}()
	for i := 0; i < 3; i++ {
		files = append(files, c.initFile(alice, bob, fmt.Sprintf("active-%d.bin", i), 1, hex.EncodeToString(sum[:]), 64<<10, http.StatusCreated))
	}
	c.initFile(alice, bob, "active-overflow.bin", 1, hex.EncodeToString(sum[:]), 64<<10, http.StatusTooManyRequests)
	c.t.Log("verified default active file limit: three accepted, fourth rejected with HTTP 429")
}
