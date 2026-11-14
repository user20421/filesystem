package stress

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

type APIClient struct {
	Base string
	HTTP *http.Client
}

func NewAPIClient(base string) *APIClient {
	return &APIClient{Base: strings.TrimRight(base, "/"), HTTP: &http.Client{Timeout: 15 * time.Minute, Transport: &http.Transport{MaxIdleConns: 256, MaxIdleConnsPerHost: 128, IdleConnTimeout: 90 * time.Second}}}
}
func (a *APIClient) JSON(ctx context.Context, method, path, token string, input, output any, expected ...int) error {
	var body io.Reader
	if input != nil {
		raw, err := json.Marshal(input)
		if err != nil {
			return err
		}
		body = bytes.NewReader(raw)
	}
	req, err := http.NewRequestWithContext(ctx, method, a.Base+path, body)
	if err != nil {
		return err
	}
	if input != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := a.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	ok := len(expected) == 0 && resp.StatusCode >= 200 && resp.StatusCode < 300
	for _, status := range expected {
		if resp.StatusCode == status {
			ok = true
		}
	}
	if !ok {
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("%s %s: HTTP %d: %s", method, path, resp.StatusCode, strings.TrimSpace(string(raw)))
	}
	if output != nil {
		var envelope struct {
			Data json.RawMessage `json:"data"`
		}
		if err = json.NewDecoder(resp.Body).Decode(&envelope); err != nil {
			return err
		}
		if len(envelope.Data) == 0 {
			return fmt.Errorf("%s: missing data envelope", path)
		}
		return json.Unmarshal(envelope.Data, output)
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	return nil
}
func (a *APIClient) Ticket(ctx context.Context, token string) (string, error) {
	var data struct {
		Ticket string `json:"ticket"`
	}
	if err := a.JSON(ctx, http.MethodPost, "/api/v1/ws-ticket", token, nil, &data, http.StatusCreated); err != nil {
		return "", err
	}
	if data.Ticket == "" {
		return "", fmt.Errorf("empty ws ticket")
	}
	return data.Ticket, nil
}
func (a *APIClient) WebSocket(ctx context.Context, ticket string) (*websocket.Conn, error) {
	base, err := url.Parse(a.Base)
	if err != nil {
		return nil, err
	}
	if base.Scheme == "https" {
		base.Scheme = "wss"
	} else {
		base.Scheme = "ws"
	}
	base.Path = "/ws"
	base.RawQuery = url.Values{"ticket": {ticket}}.Encode()
	dialer := websocket.Dialer{HandshakeTimeout: 15 * time.Second}
	conn, resp, err := dialer.DialContext(ctx, base.String(), nil)
	if resp != nil && resp.Body != nil {
		resp.Body.Close()
	}
	return conn, err
}
func (a *APIClient) Health(ctx context.Context, path string) error {
	return a.JSON(ctx, http.MethodGet, path, "", nil, nil, http.StatusOK)
}
