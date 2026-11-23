package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"secure-im/backend/internal/stress"
)

// main 启动真实压测工具，默认读取隔离环境并生成连接、消息、文件及资源报告。
func main() {
	cfg := stress.DefaultConfig()
	defaults := cfg
	var mix string
	var fileMiB, hiddenFileMiB float64
	var duration time.Duration
	var hiddenFileBytes int64
	flags := flag.NewFlagSet("stress-runner", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	flags.StringVar(&cfg.Server, "server", defaults.Server, "backend base URL")
	flags.StringVar(&cfg.EnvFile, "env-file", defaults.EnvFile, "deployment env file")
	flags.StringVar(&cfg.ReportRoot, "report-root", defaults.ReportRoot, "report output root")
	flags.IntVar(&cfg.Target, "target", defaults.Target, "total WebSocket connections/users")
	flags.StringVar(&mix, "mix", "80/18/2", "idle/chat/file percentages")
	flags.IntVar(&cfg.ConnectionsPerS, "connection-rate", defaults.ConnectionsPerS, "connections launched per second")
	flags.DurationVar(&cfg.StableDuration, "stable-duration", defaults.StableDuration, "full stable load duration")
	flags.Float64Var(&cfg.ChatRate, "chat-rate", defaults.ChatRate, "messages/second/chat user")
	flags.Float64Var(&fileMiB, "file-size-mib", 100, "synthetic file size in MiB")
	flags.Float64Var(&hiddenFileMiB, "file-mib", 0, "")
	flags.Int64Var(&cfg.ChunkSize, "chunk-size", defaults.ChunkSize, "upload chunk size in bytes")
	flags.IntVar(&cfg.FileWorkers, "file-workers", defaults.FileWorkers, "global file transfer workers")
	flags.IntVar(&cfg.FileQueueSize, "file-queue-size", defaults.FileQueueSize, "bounded file job queue capacity")
	flags.Int64Var(&cfg.FileBudgetMiB, "file-budget-mib", defaults.FileBudgetMiB, "synthetic file buffer budget in MiB")
	flags.DurationVar(&cfg.RampTimeout, "ramp-timeout", defaults.RampTimeout, "wait for 99% online after ramp")
	flags.DurationVar(&cfg.DrainTimeout, "drain-timeout", defaults.DrainTimeout, "file queue drain timeout")
	flags.StringVar(&cfg.DockerContainer, "docker-container", defaults.DockerContainer, "backend docker container name")
	flags.DurationVar(&duration, "duration", 0, "")
	flags.Int64Var(&hiddenFileBytes, "file-size-bytes", 0, "")
	flags.Usage = func() {
		fmt.Fprintln(flags.Output(), "Secure IM real-load stress runner")
		fmt.Fprintln(flags.Output(), "Public flags: --server --env-file --report-root --target --mix --connection-rate --stable-duration --chat-rate --file-size-mib --chunk-size --file-workers --file-queue-size --file-budget-mib --ramp-timeout --drain-timeout --docker-container")
	}
	if err := flags.Parse(os.Args[1:]); err != nil {
		os.Exit(2)
	}
	var err error
	cfg.IdlePercent, cfg.ChatPercent, cfg.FilePercent, err = parseMix(mix)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	if duration > 0 {
		cfg.StableDuration = duration
	}
	if hiddenFileMiB > 0 {
		fileMiB = hiddenFileMiB
	}
	cfg.FileSize = int64(fileMiB * float64(1<<20))
	if hiddenFileBytes > 0 {
		cfg.FileSize = hiddenFileBytes
	}
	if err = cfg.Validate(); err != nil {
		fmt.Fprintln(os.Stderr, "invalid configuration:", err)
		os.Exit(2)
	}
	cfg.EnvFile = projectPath(cfg.EnvFile)
	cfg.ReportRoot = projectPath(cfg.ReportRoot)
	cfg.RunID = stress.RunID()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	final := stress.NewRunner(cfg, os.Stdout).Run(ctx)
	_ = json.NewEncoder(os.Stdout).Encode(final)
	if final.Status == "failure" {
		os.Exit(1)
	}
}

func parseMix(value string) (int, int, int, error) {
	parts := strings.Split(value, "/")
	if len(parts) != 3 {
		return 0, 0, 0, fmt.Errorf("--mix must be idle/chat/file")
	}
	values := [3]int{}
	for i := range parts {
		n, err := strconv.Atoi(parts[i])
		if err != nil {
			return 0, 0, 0, fmt.Errorf("invalid --mix: %w", err)
		}
		values[i] = n
	}
	if values[0]+values[1]+values[2] != 100 {
		return 0, 0, 0, fmt.Errorf("--mix percentages must total 100")
	}
	return values[0], values[1], values[2], nil
}
func projectPath(path string) string {
	if filepath.IsAbs(path) {
		return path
	}
	if _, err := os.Stat("go.mod"); err == nil {
		if _, err = os.Stat(filepath.Join("..", "deploy")); err == nil {
			return filepath.Join("..", path)
		}
	}
	return path
}
