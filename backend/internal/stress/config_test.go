package stress

import "testing"

func TestDefaultConfigUsesFiveMiBChunks(t *testing.T) {
	cfg := DefaultConfig()
	if cfg.ChunkSize != 5<<20 {
		t.Fatalf("ChunkSize = %d, want 5 MiB", cfg.ChunkSize)
	}
	if err := cfg.Validate(); err != nil {
		t.Fatalf("default config is invalid: %v", err)
	}
}
