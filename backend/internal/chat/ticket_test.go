package chat

import (
	"testing"
	"time"
)

func TestRevokeUserLeavesOtherTicketsValid(t *testing.T) {
	s := NewTicketStore(time.Minute)
	aliceOne, _ := s.Issue("alice")
	aliceTwo, _ := s.Issue("alice")
	bob, _ := s.Issue("bob")
	s.RevokeUser("alice")
	for _, raw := range []string{aliceOne, aliceTwo} {
		if _, ok := s.Consume(raw); ok {
			t.Fatal("revoked user's ticket was accepted")
		}
	}
	if id, ok := s.Consume(bob); !ok || id != "bob" {
		t.Fatal("other user's ticket was revoked")
	}
}

func TestTicketIsSingleUseAndExpires(t *testing.T) {
	now := time.Now()
	s := NewTicketStore(time.Minute)
	s.now = func() time.Time { return now }
	raw, err := s.Issue("user-1")
	if err != nil {
		t.Fatal(err)
	}
	if id, ok := s.Consume(raw); !ok || id != "user-1" {
		t.Fatal("ticket not accepted")
	}
	if _, ok := s.Consume(raw); ok {
		t.Fatal("ticket accepted twice")
	}
	raw, _ = s.Issue("user-2")
	now = now.Add(2 * time.Minute)
	if _, ok := s.Consume(raw); ok {
		t.Fatal("expired ticket accepted")
	}
}
