package filetransfer

import "testing"

func TestAccountDeletionAuthorizesRecipientOnlyForSharedDelete(t *testing.T) {
	file := File{OwnerID: "owner", RecipientID: "recipient"}
	if err := Authorize(file, "recipient", false); err != nil {
		t.Fatalf("recipient could not use account deletion: %v", err)
	}
	if err := Authorize(file, "recipient", true); err == nil {
		t.Fatal("recipient unexpectedly passed owner-only deletion")
	}
	if err := Authorize(file, "stranger", false); err == nil {
		t.Fatal("unrelated user passed participant authorization")
	}
}
