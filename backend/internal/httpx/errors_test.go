package httpx

import (
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/apperror"
)

func TestBusinessErrorMappings(t *testing.T) {
	tests := []struct {
		name string
		kind apperror.Kind
		want int
	}{
		{"bad request", apperror.KindBadRequest, http.StatusBadRequest},
		{"unauthorized", apperror.KindUnauthorized, http.StatusUnauthorized},
		{"forbidden", apperror.KindForbidden, http.StatusForbidden},
		{"not found", apperror.KindNotFound, http.StatusNotFound},
		{"conflict", apperror.KindConflict, http.StatusConflict},
		{"too many requests", apperror.KindTooManyRequests, http.StatusTooManyRequests},
		{"service unavailable", apperror.KindServiceUnavailable, http.StatusServiceUnavailable},
	}
	gin.SetMode(gin.TestMode)
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			recorder := httptest.NewRecorder()
			ctx, _ := gin.CreateTestContext(recorder)
			ctx.Request = httptest.NewRequest(http.MethodGet, "/", nil)
			err := fmt.Errorf("operation: %w", apperror.New(test.kind, "stable_code", "stable message"))
			Fail(ctx, err)
			if recorder.Code != test.want {
				t.Fatalf("status = %d, want %d", recorder.Code, test.want)
			}
			wantBody := "{\"error\":{\"code\":\"stable_code\",\"message\":\"stable message\",\"request_id\":\"\"}}\n"
			if recorder.Body.String() != wantBody {
				t.Fatalf("body = %q, want %q", recorder.Body.String(), wantBody)
			}
		})
	}
}

func TestUnknownBusinessErrorKindIsInternal(t *testing.T) {
	recorder := httptest.NewRecorder()
	ctx, _ := gin.CreateTestContext(recorder)
	ctx.Request = httptest.NewRequest(http.MethodGet, "/", nil)
	Fail(ctx, apperror.New(apperror.KindUnknown, "must_not_leak", "must not leak"))
	if recorder.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", recorder.Code)
	}
	var api *APIError
	if errors.As(apperror.New(apperror.KindBadRequest, "code", "message"), &api) {
		t.Fatal("business error must remain distinct from transport error")
	}
}
