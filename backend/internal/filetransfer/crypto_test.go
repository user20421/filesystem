package filetransfer

import (
	"bytes"
	"testing"
)

func TestDEKWrapAndChunkEncryption(t *testing.T) {
	kek := bytes.Repeat([]byte{7}, 32)
	dek, wrapped, wrapNonce, err := GenerateWrappedDEK(kek)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(dek, wrapped) || len(dek) != 32 {
		t.Fatal("DEK was not wrapped")
	}
	unwrapped, err := UnwrapDEK(kek, wrapped, wrapNonce)
	if err != nil || !bytes.Equal(dek, unwrapped) {
		t.Fatal("DEK unwrap failed")
	}
	plain := []byte("secret chunk contents")
	ciphertext, nonce, err := EncryptChunk(dek, plain)
	if err != nil {
		t.Fatal(err)
	}
	opened, err := DecryptChunk(unwrapped, ciphertext, nonce)
	if err != nil || !bytes.Equal(plain, opened) {
		t.Fatal("chunk round trip failed")
	}
	ciphertext[0] ^= 1
	if _, err := DecryptChunk(dek, ciphertext, nonce); err == nil {
		t.Fatal("tampered ciphertext accepted")
	}
	other, _, _, _ := GenerateWrappedDEK(kek)
	if _, err := DecryptChunk(other, opened, nonce); err == nil {
		t.Fatal("plaintext accepted as ciphertext")
	}
}
func TestPermissionAndChunkState(t *testing.T) {
	f := File{OwnerID: "owner", RecipientID: "recipient", Size: 10, ChunkSize: 4, ChunkCount: 3, Status: "uploading"}
	if err := Authorize(f, "recipient", false); err != nil {
		t.Fatal(err)
	}
	if err := Authorize(f, "recipient", true); err == nil {
		t.Fatal("recipient can perform owner operation")
	}
	if err := Authorize(f, "stranger", false); err == nil {
		t.Fatal("stranger authorized")
	}
	for index, want := range []int64{4, 4, 2} {
		got, err := expectedChunkSize(f, index)
		if err != nil || got != want {
			t.Fatalf("chunk %d: %d %v", index, got, err)
		}
	}
	if _, err := expectedChunkSize(f, 3); err == nil {
		t.Fatal("invalid index accepted")
	}
}
