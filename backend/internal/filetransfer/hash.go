package filetransfer

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
)

func SHA256(data []byte) string { sum := sha256.Sum256(data); return hex.EncodeToString(sum[:]) }
func hashesEqual(a, b string) bool {
	return len(a) == len(b) && subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}
