package auth

import "testing"

func testParams() ArgonParams {
	return ArgonParams{Memory: 8 * 1024, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
}
func TestPasswordHashAndVerify(t *testing.T) {
	hash, err := HashPassword("a-secure-password", testParams())
	if err != nil {
		t.Fatal(err)
	}
	ok, err := VerifyPassword(hash, "a-secure-password")
	if err != nil || !ok {
		t.Fatalf("verification failed: %v %v", ok, err)
	}
	ok, err = VerifyPassword(hash, "wrong-password")
	if err != nil || ok {
		t.Fatalf("wrong password accepted: %v %v", ok, err)
	}
	other, _ := HashPassword("a-secure-password", testParams())
	if other == hash {
		t.Fatal("password salts are not random")
	}
}
func TestPasswordValidation(t *testing.T) {
	if _, err := HashPassword("short", testParams()); err == nil {
		t.Fatal("five-byte password accepted")
	}
	if _, err := HashPassword("123456", testParams()); err != nil {
		t.Fatalf("six-byte password rejected: %v", err)
	}
	if err := validateRegistration("mixedCase", "User", "123456"); err != nil {
		t.Fatalf("valid registration rejected: %v", err)
	}
	if _, err := HashPassword(string(make([]byte, 128)), testParams()); err != nil {
		t.Fatalf("128-byte password rejected: %v", err)
	}
	if _, err := HashPassword(string(make([]byte, 129)), testParams()); err == nil {
		t.Fatal("129-byte password accepted")
	}
	if _, err := HashPassword("密ab", testParams()); err == nil {
		t.Fatal("five-byte UTF-8 password accepted")
	}
	if _, err := VerifyPassword("broken", "a-secure-password"); err == nil {
		t.Fatal("malformed hash accepted")
	}
}
