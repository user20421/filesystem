package chat

import (
	"reflect"
	"testing"
)

func TestHubPresenceTracksConnections(t *testing.T) {
	hub := NewHub()
	aliceOne := &client{}
	aliceTwo := &client{}
	bob := &client{}
	if hub.IsOnline("alice") || len(hub.OnlineUsers()) != 0 {
		t.Fatal("empty hub reported online users")
	}
	if !hub.add("alice", aliceOne) || !hub.add("alice", aliceTwo) || !hub.add("bob", bob) {
		t.Fatal("failed to add connections")
	}
	if got, want := hub.OnlineUsers(), []string{"alice", "bob"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("online users = %v, want %v", got, want)
	}
	hub.remove("alice", aliceOne)
	if !hub.IsOnline("alice") {
		t.Fatal("user went offline while one connection remained")
	}
	hub.remove("alice", aliceTwo)
	if hub.IsOnline("alice") {
		t.Fatal("user remained online after last connection was removed")
	}
	if got, want := hub.OnlineUsers(), []string{"bob"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("online users = %v, want %v", got, want)
	}
}

func TestHubCloseUserOnlyClosesThatUser(t *testing.T) {
	hub := NewHub()
	aliceOne := &client{done: make(chan struct{})}
	aliceTwo := &client{done: make(chan struct{})}
	bob := &client{done: make(chan struct{})}
	hub.add("alice", aliceOne)
	hub.add("alice", aliceTwo)
	hub.add("bob", bob)
	hub.CloseUser("alice")
	for _, connection := range []*client{aliceOne, aliceTwo} {
		select {
		case <-connection.done:
		default:
			t.Fatal("deleted user's connection remained open")
		}
	}
	select {
	case <-bob.done:
		t.Fatal("other user's connection was closed")
	default:
	}
	if hub.IsOnline("alice") || !hub.IsOnline("bob") {
		t.Fatal("CloseUser changed the wrong presence entries")
	}
}

func TestHubPresenceConcurrentAccess(t *testing.T) {
	hub := NewHub()
	connection := &client{}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for i := 0; i < 1000; i++ {
			hub.add("alice", connection)
			hub.IsOnline("alice")
			hub.remove("alice", connection)
		}
	}()
	for i := 0; i < 1000; i++ {
		_ = hub.OnlineUsers()
	}
	<-done
}
