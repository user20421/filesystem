package chat

import (
	"testing"

	"google.golang.org/protobuf/proto"
	"secure-im/backend/internal/proto/impb"
)

func TestValidateMessagePayload(t *testing.T) {
	text, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{Text: "hello"}})
	if err != nil {
		t.Fatal(err)
	}
	file, err := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_File{File: &impb.FileNotification{
		FileId: "file-1", Filename: "hello.txt", Size: 5,
	}}})
	if err != nil {
		t.Fatal(err)
	}
	valid := func(payload []byte) *impb.Envelope {
		return &impb.Envelope{
			Version: protocolVersion, Type: impb.Envelope_MESSAGE, MessageId: "message-1",
			DeviceId: "desktop", SenderId: "sender", Payload: payload,
		}
	}
	for name, payload := range map[string][]byte{"text": text, "file": file} {
		t.Run(name, func(t *testing.T) {
			if err := validateMessage(valid(payload), "sender"); err != nil {
				t.Fatalf("valid payload rejected: %v", err)
			}
		})
	}

	emptyText, _ := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_Text{}})
	missingFileID, _ := proto.Marshal(&impb.MessagePayload{Content: &impb.MessagePayload_File{File: &impb.FileNotification{
		Filename: "hello.txt", Size: 5,
	}}})
	emptyBody, _ := proto.Marshal(&impb.MessagePayload{})
	for name, payload := range map[string][]byte{
		"plain text":      []byte("hello"),
		"legacy JSON":     []byte(`{"type":"text","text":"hello"}`),
		"invalid wire":    {0xff},
		"empty body":      emptyBody,
		"empty text":      emptyText,
		"missing file id": missingFileID,
	} {
		t.Run(name, func(t *testing.T) {
			if err := validateMessage(valid(payload), "sender"); err == nil {
				t.Fatal("invalid payload accepted")
			}
		})
	}

	legacy := valid(text)
	legacy.Version = 1
	if err := validateMessage(legacy, "sender"); err == nil {
		t.Fatal("version 1 envelope accepted")
	}
}

func TestAckUsesProtobufPayload(t *testing.T) {
	ack := Ack(Saved{Envelope: &impb.Envelope{MessageId: "message-1"}, Cursor: 42})
	if ack.Version != protocolVersion || ack.Type != impb.Envelope_ACK || ack.Cursor != 0 {
		t.Fatalf("unexpected ACK envelope: %v", ack)
	}
	var payload impb.AckPayload
	if err := proto.Unmarshal(ack.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Cursor != 42 {
		t.Fatalf("ACK cursor = %d, want 42", payload.Cursor)
	}
}

func TestErrorUsesProtobufPayload(t *testing.T) {
	c := &client{send: make(chan *impb.Envelope, 1), done: make(chan struct{})}
	(&Handler{}).sendError(c, "invalid_payload", "message rejected")
	envelope := <-c.send
	if envelope.Version != protocolVersion || envelope.Type != impb.Envelope_ERROR {
		t.Fatalf("unexpected ERROR envelope: %v", envelope)
	}
	var payload impb.ErrorPayload
	if err := proto.Unmarshal(envelope.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Code != "invalid_payload" || payload.Message != "message rejected" {
		t.Fatalf("unexpected ERROR payload: %v", &payload)
	}
}
