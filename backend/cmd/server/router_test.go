package main

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/chat"
	"secure-im/backend/internal/filetransfer"
	"secure-im/backend/internal/httpx"
	"secure-im/backend/internal/proto/impb"
)

type routerContextKey struct{}

type routerErrorBody struct {
	Error struct {
		Code      string `json:"code"`
		RequestID string `json:"request_id"`
	} `json:"error"`
}

func decodeRouterError(t *testing.T, response *httptest.ResponseRecorder) routerErrorBody {
	t.Helper()
	var body routerErrorBody
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode error response %q: %v", response.Body.String(), err)
	}
	return body
}

func TestRouterGetHeadPathValueContextAndRequestID(t *testing.T) {
	routes := newRouter(nil, nil, func(c *gin.Context) {
		ctx := context.WithValue(c.Request.Context(), routerContextKey{}, "preserved")
		c.Request = c.Request.WithContext(ctx)
		c.Next()
	}, httpx.RequestContext())
	get(routes.engine, "/items/:file_id", func(c *gin.Context) {
		contextValue, _ := c.Request.Context().Value(routerContextKey{}).(string)
		httpx.Data(c, http.StatusOK, map[string]string{
			"method":        c.Request.Method,
			"path_value":    c.Param("file_id"),
			"request_id":    httpx.RequestID(c.Request.Context()),
			"context_value": contextValue,
		})
	})
	server := httptest.NewServer(routes)
	defer server.Close()

	request, err := http.NewRequest(http.MethodGet, server.URL+"/items/file-123", nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("X-Request-ID", "router-request-id")
	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var body struct {
		Data map[string]string `json:"data"`
	}
	if err := json.NewDecoder(response.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusOK || response.Header.Get("X-Request-ID") != "router-request-id" {
		t.Fatalf("GET status=%d request_id=%q", response.StatusCode, response.Header.Get("X-Request-ID"))
	}
	if response.Header.Get("X-Content-Type-Options") != "nosniff" {
		t.Fatalf("X-Content-Type-Options=%q", response.Header.Get("X-Content-Type-Options"))
	}
	want := map[string]string{"method": http.MethodGet, "path_value": "file-123", "request_id": "router-request-id", "context_value": "preserved"}
	for key, value := range want {
		if body.Data[key] != value {
			t.Errorf("GET data[%q]=%q, want %q", key, body.Data[key], value)
		}
	}

	head, err := server.Client().Do(mustRequest(t, http.MethodHead, server.URL+"/items/file-123"))
	if err != nil {
		t.Fatal(err)
	}
	defer head.Body.Close()
	headBody, err := io.ReadAll(head.Body)
	if err != nil {
		t.Fatal(err)
	}
	if head.StatusCode != http.StatusOK || len(headBody) != 0 || head.Header.Get("Content-Type") != "application/json; charset=utf-8" {
		t.Fatalf("HEAD status=%d body=%q content_type=%q", head.StatusCode, headBody, head.Header.Get("Content-Type"))
	}
}

func mustRequest(t *testing.T, method, url string) *http.Request {
	t.Helper()
	request, err := http.NewRequest(method, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	return request
}

func TestRouterJSONNotFound(t *testing.T) {
	routes := newRouter(nil, nil, httpx.RequestContext())
	get(routes.engine, "/health", func(c *gin.Context) {
		httpx.Data(c, http.StatusOK, map[string]string{"status": "ok"})
	})
	for _, test := range []struct {
		name   string
		method string
		path   string
	}{
		{name: "unknown path", method: http.MethodGet, path: "/missing"},
		{name: "wrong method", method: http.MethodPost, path: "/health"},
		{name: "trailing slash", method: http.MethodGet, path: "/health/"},
	} {
		t.Run(test.name, func(t *testing.T) {
			request := httptest.NewRequest(test.method, test.path, nil)
			request.Header.Set("X-Request-ID", "missing-route-request")
			response := httptest.NewRecorder()
			routes.ServeHTTP(response, request)
			if response.Code != http.StatusNotFound {
				t.Fatalf("status=%d, want 404", response.Code)
			}
			if response.Header().Get("Content-Type") != "application/json; charset=utf-8" {
				t.Fatalf("Content-Type=%q", response.Header().Get("Content-Type"))
			}
			if !strings.HasSuffix(response.Body.String(), "\n") {
				t.Fatalf("JSON response has no trailing newline: %q", response.Body.String())
			}
			body := decodeRouterError(t, response)
			if body.Error.Code != "not_found" || body.Error.RequestID != "missing-route-request" {
				t.Fatalf("error body=%+v", body.Error)
			}
		})
	}
}

func TestRouterJSONDecodeBoundaries(t *testing.T) {
	routes := newRouter(nil, nil, httpx.RequestContext())
	routes.engine.POST("/decode", func(c *gin.Context) {
		var input struct {
			Name string `json:"name"`
		}
		if err := httpx.Decode(c, &input); err != nil {
			httpx.Fail(c, err)
			return
		}
		httpx.Data(c, http.StatusCreated, input)
	})
	for _, test := range []struct {
		name       string
		body       string
		wantStatus int
		wantCode   string
	}{
		{name: "valid", body: `{"name":"alice"}`, wantStatus: http.StatusCreated},
		{name: "empty", body: ``, wantStatus: http.StatusBadRequest, wantCode: "invalid_json"},
		{name: "malformed", body: `{"name":`, wantStatus: http.StatusBadRequest, wantCode: "invalid_json"},
		{name: "unknown field", body: `{"name":"alice","extra":true}`, wantStatus: http.StatusBadRequest, wantCode: "invalid_json"},
		{name: "multiple values", body: `{"name":"alice"}{"name":"bob"}`, wantStatus: http.StatusBadRequest, wantCode: "invalid_json"},
		{name: "over one MiB", body: `{"name":"` + strings.Repeat("x", 1<<20) + `"}`, wantStatus: http.StatusBadRequest, wantCode: "invalid_json"},
	} {
		t.Run(test.name, func(t *testing.T) {
			response := httptest.NewRecorder()
			routes.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/decode", strings.NewReader(test.body)))
			if response.Code != test.wantStatus {
				t.Fatalf("status=%d body=%s, want %d", response.Code, response.Body.String(), test.wantStatus)
			}
			if !strings.HasSuffix(response.Body.String(), "\n") {
				t.Fatalf("JSON response has no trailing newline: %q", response.Body.String())
			}
			if test.wantCode != "" {
				if body := decodeRouterError(t, response); body.Error.Code != test.wantCode {
					t.Fatalf("error code=%q, want %q", body.Error.Code, test.wantCode)
				}
			}
		})
	}
}

func TestRouterRejectsOversizedChunk(t *testing.T) {
	routes := newRouter(nil, nil, httpx.RequestContext())
	files := filetransfer.NewHandler(nil, slog.New(slog.NewTextHandler(io.Discard, nil)), 4, nil)
	routes.engine.PUT("/api/v1/files/:file_id/chunks/:index", files.Upload)
	response := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPut, "/api/v1/files/file-123/chunks/0", strings.NewReader("12345"))
	routes.ServeHTTP(response, request)
	if response.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status=%d body=%s, want 413", response.Code, response.Body.String())
	}
	if body := decodeRouterError(t, response); body.Error.Code != "chunk_too_large" {
		t.Fatalf("error code=%q, want chunk_too_large", body.Error.Code)
	}
}

type routerDeadlineRecorder struct {
	*httptest.ResponseRecorder
	deadlines []time.Time
}

func (w *routerDeadlineRecorder) SetWriteDeadline(deadline time.Time) error {
	w.deadlines = append(w.deadlines, deadline)
	return nil
}

func TestRouterResponseControllerDeadline(t *testing.T) {
	routes := newRouter(nil, nil)
	get(routes.engine, "/download", func(c *gin.Context) {
		controller := http.NewResponseController(c.Writer)
		if err := controller.SetWriteDeadline(time.Time{}); err != nil {
			t.Errorf("clear deadline: %v", err)
			return
		}
		if err := controller.SetWriteDeadline(time.Now().Add(30 * time.Second)); err != nil {
			t.Errorf("set rolling deadline: %v", err)
			return
		}
		c.Writer.Header().Set("Content-Disposition", "attachment; filename*=UTF-8''file.bin")
		_, _ = c.Writer.Write([]byte("ok"))
	})
	response := &routerDeadlineRecorder{ResponseRecorder: httptest.NewRecorder()}
	routes.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/download", nil))
	if response.Code != http.StatusOK || response.Body.String() != "ok" {
		t.Fatalf("status=%d body=%q", response.Code, response.Body.String())
	}
	if len(response.deadlines) != 2 || !response.deadlines[0].IsZero() || response.deadlines[1].IsZero() {
		t.Fatalf("deadlines=%v, want clear then rolling deadline", response.deadlines)
	}
	if got := response.Header().Get("Content-Disposition"); got != "attachment; filename*=UTF-8''file.bin" {
		t.Fatalf("Content-Disposition=%q", got)
	}
}

func TestRouterWebSocketMethodlessCompatibility(t *testing.T) {
	for _, method := range []string{http.MethodGet, http.MethodPost, "BREW"} {
		t.Run(method, func(t *testing.T) {
			tickets := chat.NewTicketStore(time.Minute)
			chatHTTP := chat.NewHandler(chat.NewService(nil), tickets, chat.NewHub(), slog.New(slog.NewTextHandler(io.Discard, nil)), 1)
			routes := newRouter(nil, chatHTTP.WebSocket, httpx.RequestContext())
			response := httptest.NewRecorder()
			routes.ServeHTTP(response, httptest.NewRequest(method, "/ws?ticket=invalid", nil))
			if response.Code != http.StatusUnauthorized {
				t.Fatalf("status=%d body=%s, want 401", response.Code, response.Body.String())
			}
			if body := decodeRouterError(t, response); body.Error.Code != "invalid_ws_ticket" {
				t.Fatalf("error code=%q, want invalid_ws_ticket", body.Error.Code)
			}
		})
	}
}

func TestRouterWebSocketHandshake(t *testing.T) {
	tickets := chat.NewTicketStore(time.Minute)
	hub := chat.NewHub()
	chatHTTP := chat.NewHandler(chat.NewService(nil), tickets, hub, slog.New(slog.NewTextHandler(io.Discard, nil)), 1)
	routes := newRouter(nil, chatHTTP.WebSocket, httpx.RequestContext())
	server := httptest.NewServer(routes)
	defer server.Close()
	defer hub.CloseAll()
	ticket, err := tickets.Issue("router-user")
	if err != nil {
		t.Fatal(err)
	}
	url := "ws" + strings.TrimPrefix(server.URL, "http") + "/ws?ticket=" + ticket
	connection, response, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatalf("WebSocket handshake failed: %v", err)
	}
	defer connection.Close()
	if response.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("handshake status=%d, want 101", response.StatusCode)
	}
	if err := connection.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if err := connection.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatal(err)
	}
	ping, err := proto.Marshal(&impb.Envelope{Version: 2, Type: impb.Envelope_PING})
	if err != nil {
		t.Fatal(err)
	}
	if err := connection.WriteMessage(websocket.BinaryMessage, ping); err != nil {
		t.Fatal(err)
	}
	kind, data, err := connection.ReadMessage()
	if err != nil {
		t.Fatal(err)
	}
	if kind != websocket.BinaryMessage {
		t.Fatalf("frame kind=%d, want binary", kind)
	}
	var pong impb.Envelope
	if err := proto.Unmarshal(data, &pong); err != nil {
		t.Fatal(err)
	}
	if pong.Type != impb.Envelope_PONG {
		t.Fatalf("frame type=%s, want PONG", pong.Type)
	}
}
