package filetransfer

import (
	"context"
	"testing"

	"github.com/google/uuid"
)

func TestMinIOListRejectsNonContentPrefixes(t *testing.T) {
	store := &MinIOStore{}
	for _, prefix := range []string{"", "contents/", "contents/not-a-uuid/", "other/" + uuid.NewString() + "/", "contents/" + uuid.NewString()} {
		if _, err := store.List(context.Background(), prefix); err == nil {
			t.Fatalf("List accepted prefix %q", prefix)
		}
	}
	if !validContentPrefix("contents/" + uuid.NewString() + "/") {
		t.Fatal("canonical content prefix was rejected")
	}
}
