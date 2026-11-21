// Package stress implements the Secure IM real-load stress runner.
package stress

import (
	"fmt"
	"math"
	"time"
)

const (
	DefaultFileBudgetMiB = 500
	DefaultServer        = "http://127.0.0.1:18080"
)

type Config struct {
	Server                string
	EnvFile               string
	ReportRoot            string
	Target                int
	IdlePercent           int
	ChatPercent           int
	FilePercent           int
	ConnectionsPerS       int
	StableDuration        time.Duration
	ChatRate              float64
	FileSize              int64
	ChunkSize             int64
	FileWorkers           int
	FileQueueSize         int
	FileBudgetMiB         int64
	BackendMemoryLimitMiB int64
	DrainTimeout          time.Duration
	RampTimeout           time.Duration
	DockerContainer       string
	RunID                 string
}

func DefaultConfig() Config {
	return Config{Server: DefaultServer, EnvFile: "deploy/.env", ReportRoot: "runtime/stress-reports", Target: 1000,
		IdlePercent: 80, ChatPercent: 18, FilePercent: 2, ConnectionsPerS: 200, StableDuration: 5 * time.Minute,
		ChatRate: 0.05, FileSize: 100 << 20, ChunkSize: 5 << 20, FileWorkers: 96, FileQueueSize: 128,
		FileBudgetMiB: DefaultFileBudgetMiB, BackendMemoryLimitMiB: 3072, DrainTimeout: 5 * time.Minute, RampTimeout: 45 * time.Second, DockerContainer: "secure-im-stress-backend"}
}

func (c Config) Validate() error {
	if c.Target < 1 || c.ConnectionsPerS < 1 || c.FileWorkers < 1 || c.FileQueueSize < 1 {
		return fmt.Errorf("target, connection rate, file workers and queue size must be positive")
	}
	if c.IdlePercent < 0 || c.ChatPercent < 0 || c.FilePercent < 0 || c.IdlePercent+c.ChatPercent+c.FilePercent != 100 {
		return fmt.Errorf("idle/chat/file percentages must be non-negative and total 100")
	}
	if c.StableDuration <= 0 || c.DrainTimeout <= 0 || c.RampTimeout <= 0 || c.ChatRate < 0 || c.FileSize < 1 || c.ChunkSize < 64<<10 {
		return fmt.Errorf("invalid duration, chat rate, file size or chunk size")
	}
	if c.FileBudgetMiB < 1 || c.BackendMemoryLimitMiB < 1 || c.Server == "" || c.ReportRoot == "" {
		return fmt.Errorf("invalid server, report root or file budget")
	}
	return nil
}

type Allocation struct {
	Idle int `json:"idle"`
	Chat int `json:"chat"`
	File int `json:"file"`
}

// Allocate uses largest remainders, then keeps the chat population pairable.
func Allocate(target, idlePct, chatPct, filePct int) (Allocation, error) {
	if target < 0 || idlePct < 0 || chatPct < 0 || filePct < 0 || idlePct+chatPct+filePct != 100 {
		return Allocation{}, fmt.Errorf("invalid target or percentages")
	}
	pcts := []int{idlePct, chatPct, filePct}
	values := make([]int, 3)
	remainders := make([]float64, 3)
	used := 0
	for i, pct := range pcts {
		exact := float64(target*pct) / 100
		values[i] = int(math.Floor(exact))
		remainders[i] = exact - math.Floor(exact)
		used += values[i]
	}
	for left := target - used; left > 0; left-- {
		best := 0
		for i := 1; i < 3; i++ {
			if remainders[i] > remainders[best] {
				best = i
			}
		}
		values[best]++
		remainders[best] = -1
	}
	if values[1]%2 != 0 {
		if values[0] > 0 {
			values[1]++
			values[0]--
		} else {
			values[1]--
			values[0]++
		}
	}
	return Allocation{Idle: values[0], Chat: values[1], File: values[2]}, nil
}
