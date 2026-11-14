package stress

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestTicketAcceptsCreatedEnvelope(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/v1/ws-ticket" || r.Header.Get("Authorization") != "Bearer token" {
			t.Errorf("unexpected ticket request %+v", r)
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`{"data":{"ticket":"one-time","expires_in":60}}`))
	}))
	defer server.Close()
	ticket, err := NewAPIClient(server.URL).Ticket(context.Background(), "token")
	if err != nil {
		t.Fatal(err)
	}
	if ticket != "one-time" {
		t.Fatalf("ticket=%q", ticket)
	}
}
