// Package apperror 定义与 HTTP、WebSocket 等传输协议无关的业务错误。
// Kind 只表达错误类别，具体传输状态由各自边界统一映射。
package apperror

// Kind 是业务错误的语义类别，不包含任何 HTTP 状态码。
type Kind uint8

const (
	KindUnknown Kind = iota
	KindBadRequest
	KindUnauthorized
	KindForbidden
	KindNotFound
	KindConflict
	KindTooManyRequests
	KindServiceUnavailable
)

// Error 携带稳定的业务 code 和可安全展示给调用方的 message。
type Error struct {
	Kind    Kind
	Code    string
	Message string
}

func (e *Error) Error() string { return e.Message }

// New 创建一个业务错误；调用方不得在此附加传输协议状态。
func New(kind Kind, code, message string) *Error {
	return &Error{Kind: kind, Code: code, Message: message}
}
