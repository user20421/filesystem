package account

import (
	"context"
	"errors"
	"reflect"
	"testing"

	"secure-im/backend/internal/httpx"
)

type fakeRepo struct {
	ids        []string
	listErr    error
	deleteErr  error
	deleteCall int
	steps      *[]string
}

func (r *fakeRepo) FileIDs(context.Context, string) ([]string, error) {
	return r.ids, r.listErr
}
func (r *fakeRepo) DeleteUser(context.Context, string) error {
	r.deleteCall++
	if r.steps != nil {
		*r.steps = append(*r.steps, "database")
	}
	return r.deleteErr
}

type fakeFiles struct {
	calls []string
	fail  string
	steps *[]string
}

func (f *fakeFiles) DeleteForAccount(_ context.Context, id, _ string) error {
	f.calls = append(f.calls, id)
	if f.steps != nil {
		*f.steps = append(*f.steps, "file:"+id)
	}
	if id == f.fail {
		return errors.New("storage unavailable")
	}
	return nil
}

func TestDeleteOrderingAndPostCommitRevocation(t *testing.T) {
	steps := []string{}
	repo := &fakeRepo{ids: []string{"sent", "received"}, steps: &steps}
	files := &fakeFiles{steps: &steps}
	service := &Service{repo: repo, files: files,
		closeUser:     func(string) { steps = append(steps, "close") },
		revokeTickets: func(string) { steps = append(steps, "tickets") },
	}
	if err := service.Delete(context.Background(), "user"); err != nil {
		t.Fatal(err)
	}
	want := []string{"file:sent", "file:received", "database", "close", "tickets"}
	if !reflect.DeepEqual(steps, want) {
		t.Fatalf("deletion steps = %v, want %v", steps, want)
	}
}

func TestDeleteFailureIsRetryableAndPreservesAccount(t *testing.T) {
	repo := &fakeRepo{ids: []string{"sent", "received"}}
	files := &fakeFiles{fail: "received"}
	closed, revoked := false, false
	service := &Service{repo: repo, files: files,
		closeUser:     func(string) { closed = true },
		revokeTickets: func(string) { revoked = true },
	}
	err := service.Delete(context.Background(), "user")
	var apiErr *httpx.APIError
	if !errors.As(err, &apiErr) || apiErr.Status != 503 || apiErr.Code != "account_deletion_failed" {
		t.Fatalf("error = %#v, want retryable 503", err)
	}
	if repo.deleteCall != 0 || closed || revoked {
		t.Fatal("account or sessions were removed after file deletion failed")
	}

	files.fail = ""
	if err := service.Delete(context.Background(), "user"); err != nil {
		t.Fatalf("retry failed: %v", err)
	}
	if repo.deleteCall != 1 || !closed || !revoked {
		t.Fatal("retry did not finish account and session deletion")
	}
	if !reflect.DeepEqual(files.calls, []string{"sent", "received", "sent", "received"}) {
		t.Fatalf("retry did not safely revisit files: %v", files.calls)
	}
}
