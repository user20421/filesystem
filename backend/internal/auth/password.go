package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"fmt"
	"strconv"
	"strings"

	"golang.org/x/crypto/argon2"
)

// ArgonParams 定义 Argon2id 的计算参数：内存单位 KiB、迭代次数、并行度、盐与派生密钥字节数。
type ArgonParams struct {
	Memory, Iterations    uint32
	Parallelism           uint8
	SaltLength, KeyLength uint32
}

// DefaultArgonParams 是本地开发使用的默认密码哈希参数，生产环境应结合硬件与限流策略评估。
var DefaultArgonParams = ArgonParams{Memory: 64 * 1024, Iterations: 3, Parallelism: 2, SaltLength: 16, KeyLength: 32}

// HashPassword 使用随机盐生成 PHC 格式的 Argon2id 哈希，数据库不保存明文密码。
func HashPassword(password string, p ArgonParams) (string, error) {
	if len(password) < 6 || len(password) > 128 {
		return "", fmt.Errorf("password must be 6 to 128 bytes")
	}
	salt := make([]byte, p.SaltLength)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key := argon2.IDKey([]byte(password), salt, p.Iterations, p.Memory, p.Parallelism, p.KeyLength)
	b64 := base64.RawStdEncoding
	return fmt.Sprintf("$argon2id$v=19$m=%d,t=%d,p=%d$%s$%s", p.Memory, p.Iterations, p.Parallelism, b64.EncodeToString(salt), b64.EncodeToString(key)), nil
}

// VerifyPassword 从哈希中恢复参数并用常量时间比较校验密码，拒绝不支持的编码格式。
func VerifyPassword(encoded, password string) (bool, error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "argon2id" || parts[2] != "v=19" {
		return false, fmt.Errorf("unsupported password hash")
	}
	values := strings.Split(parts[3], ",")
	if len(values) != 3 || !strings.HasPrefix(values[0], "m=") || !strings.HasPrefix(values[1], "t=") || !strings.HasPrefix(values[2], "p=") {
		return false, fmt.Errorf("invalid password parameters")
	}
	memory, err1 := strconv.ParseUint(values[0][2:], 10, 32)
	iterations, err2 := strconv.ParseUint(values[1][2:], 10, 32)
	parallelism, err3 := strconv.ParseUint(values[2][2:], 10, 8)
	if err1 != nil || err2 != nil || err3 != nil {
		return false, fmt.Errorf("invalid password parameters")
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return false, err
	}
	expected, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(expected) == 0 {
		return false, fmt.Errorf("invalid password hash")
	}
	key := argon2.IDKey([]byte(password), salt, uint32(iterations), uint32(memory), uint8(parallelism), uint32(len(expected)))
	return subtle.ConstantTimeCompare(key, expected) == 1, nil
}
