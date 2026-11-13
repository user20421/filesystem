package filetransfer

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"io"
)

func newGCM(key []byte) (cipher.AEAD, error) {
	if len(key) != 32 {
		return nil, fmt.Errorf("AES-256 key must be 32 bytes")
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}
func GenerateWrappedDEK(kek []byte) (dek, wrapped, nonce []byte, err error) {
	dek = make([]byte, 32)
	if _, err = io.ReadFull(rand.Reader, dek); err != nil {
		return
	}
	gcm, err := newGCM(kek)
	if err != nil {
		return
	}
	nonce = make([]byte, gcm.NonceSize())
	if _, err = io.ReadFull(rand.Reader, nonce); err != nil {
		return
	}
	wrapped = gcm.Seal(nil, nonce, dek, nil)
	return
}
func UnwrapDEK(kek, wrapped, nonce []byte) ([]byte, error) {
	gcm, err := newGCM(kek)
	if err != nil {
		return nil, err
	}
	if len(nonce) != gcm.NonceSize() {
		return nil, fmt.Errorf("invalid DEK nonce")
	}
	return gcm.Open(nil, nonce, wrapped, nil)
}
func EncryptChunk(dek, plaintext []byte) (ciphertext, nonce []byte, err error) {
	gcm, err := newGCM(dek)
	if err != nil {
		return
	}
	nonce = make([]byte, gcm.NonceSize())
	if _, err = io.ReadFull(rand.Reader, nonce); err != nil {
		return
	}
	ciphertext = gcm.Seal(nil, nonce, plaintext, nil)
	return
}
func DecryptChunk(dek, ciphertext, nonce []byte) ([]byte, error) {
	gcm, err := newGCM(dek)
	if err != nil {
		return nil, err
	}
	if len(nonce) != gcm.NonceSize() {
		return nil, fmt.Errorf("invalid chunk nonce")
	}
	return gcm.Open(nil, nonce, ciphertext, nil)
}
func SHA256(data []byte) string { sum := sha256.Sum256(data); return hex.EncodeToString(sum[:]) }
func hashesEqual(a, b string) bool {
	return len(a) == len(b) && subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}
