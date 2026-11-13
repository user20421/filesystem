package account

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

type fakeDeleter struct{ err error }

func (f fakeDeleter) Delete(context.Context, string) error { return f.err }

func TestDeleteMeResponses(t *testing.T) {
	for _, test := range []struct {
		name string
		err  error
		want int
	}{
		{name: "success", want: http.StatusNoContent},
		{name: "retryable failure", err: retryable(), want: http.StatusServiceUnavailable},
	} {
		t.Run(test.name, func(t *testing.T) {
			recorder := httptest.NewRecorder()
			request := httptest.NewRequest(http.MethodDelete, "/api/v1/users/me", nil)
			NewHandler(fakeDeleter{err: test.err}).DeleteMe(recorder, request)
			if recorder.Code != test.want {
				t.Fatalf("status = %d, want %d: %s", recorder.Code, test.want, recorder.Body.String())
			}
			if test.err == nil && recorder.Body.Len() != 0 {
				t.Fatalf("204 response has body %q", recorder.Body.String())
			}
		})
	}
}
