package stress

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

type debugSample struct {
	rss, cpu, heapMiB       *float64
	goroutines              *int
	budgetUsed, budgetLimit *int64
}

type Telemetry struct {
	api            *APIClient
	metrics        *Metrics
	container      string
	mu             sync.Mutex
	rssMax, cpuMax *float64
}

func NewTelemetry(api *APIClient, metrics *Metrics, container string) *Telemetry {
	return &Telemetry{api: api, metrics: metrics, container: container}
}
func (t *Telemetry) Run(ctx context.Context) {
	defer func() {
		t.metrics.BackendStats(nil, nil)
		t.metrics.BackendRuntime(nil, nil)
		t.metrics.BackendBudget(nil, nil)
	}()
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	t.sample(ctx)
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			t.sample(ctx)
		}
	}
}
func (t *Telemetry) sample(parent context.Context) {
	ctx, cancel := context.WithTimeout(parent, 2500*time.Millisecond)
	defer cancel()
	if err := t.api.Health(ctx, "/ready"); err != nil && parent.Err() == nil {
		t.metrics.HealthFailed()
	}
	debug := t.debugStats(ctx)
	rss, cpu := debug.rss, debug.cpu
	t.metrics.BackendBudget(debug.budgetUsed, debug.budgetLimit)
	t.metrics.BackendRuntime(debug.heapMiB, debug.goroutines)
	if dockerRSS, dockerCPU, err := dockerStats(ctx, t.container); err == nil {
		if dockerRSS != nil {
			rss = dockerRSS
		}
		if dockerCPU != nil {
			cpu = dockerCPU
		}
	}
	t.metrics.BackendStats(rss, cpu)
	t.mu.Lock()
	defer t.mu.Unlock()
	if rss != nil && (t.rssMax == nil || *rss > *t.rssMax) {
		t.rssMax = clone(rss)
	}
	if cpu != nil && (t.cpuMax == nil || *cpu > *t.cpuMax) {
		t.cpuMax = clone(cpu)
	}
}
func (t *Telemetry) Max() (*float64, *float64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return clone(t.rssMax), clone(t.cpuMax)
}
func (t *Telemetry) debugStats(ctx context.Context) debugSample {
	var result debugSample
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, t.api.Base+"/debug/stats", nil)
	if err != nil {
		return result
	}
	resp, err := t.api.HTTP.Do(req)
	if err != nil {
		return result
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return result
	}
	var body map[string]any
	if json.NewDecoder(resp.Body).Decode(&body) != nil {
		return result
	}
	if data, ok := body["data"].(map[string]any); ok {
		body = data
	}
	if budget, ok := body["file_memory_budget"].(map[string]any); ok {
		result.budgetUsed, result.budgetLimit = integerField(budget, "used"), integerField(budget, "limit")
	}
	if goStats, ok := body["go"].(map[string]any); ok {
		if heap := numberField(goStats, "heap_alloc_bytes"); heap != nil {
			value := *heap / (1 << 20)
			result.heapMiB = &value
		}
		if count := integerField(goStats, "goroutines"); count != nil {
			value := int(*count)
			result.goroutines = &value
		}
	}
	result.rss = numberField(body, "backend_rss_mib", "rss_mib", "process_rss_mib")
	result.cpu = numberField(body, "backend_cpu_percent", "cpu_percent", "process_cpu_percent")
	return result
}
func integerField(body map[string]any, key string) *int64 {
	value, ok := body[key]
	if !ok {
		return nil
	}
	var parsed int64
	switch v := value.(type) {
	case float64:
		parsed = int64(v)
	case string:
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil {
			return nil
		}
		parsed = n
	default:
		return nil
	}
	return &parsed
}
func numberField(body map[string]any, keys ...string) *float64 {
	for _, key := range keys {
		if value, ok := body[key]; ok {
			switch v := value.(type) {
			case float64:
				return &v
			case string:
				parsed, err := strconv.ParseFloat(v, 64)
				if err == nil {
					return &parsed
				}
			}
		}
	}
	return nil
}
func dockerStats(ctx context.Context, container string) (*float64, *float64, error) {
	cmd := exec.CommandContext(ctx, "docker", "stats", "--no-stream", "--format", "{{json .}}", container)
	output, err := cmd.Output()
	if err != nil {
		return nil, nil, err
	}
	var sample struct {
		CPUPerc  string `json:"CPUPerc"`
		MemUsage string `json:"MemUsage"`
	}
	if err = json.Unmarshal([]byte(strings.TrimSpace(string(output))), &sample); err != nil {
		return nil, nil, err
	}
	cpu, err := strconv.ParseFloat(strings.TrimSuffix(sample.CPUPerc, "%"), 64)
	if err != nil {
		return nil, nil, err
	}
	parts := strings.SplitN(sample.MemUsage, "/", 2)
	rss, err := parseMiB(strings.TrimSpace(parts[0]))
	if err != nil {
		return nil, nil, err
	}
	return &rss, &cpu, nil
}
func parseMiB(value string) (float64, error) {
	units := []struct {
		suffix     string
		multiplier float64
	}{{"GiB", 1024}, {"GB", 1000.0 / 1.048576}, {"MiB", 1}, {"MB", 1000.0 / 1048.576}, {"KiB", 1.0 / 1024}, {"kB", 1.0 / 1024}, {"B", 1.0 / (1024 * 1024)}}
	for _, unit := range units {
		if strings.HasSuffix(value, unit.suffix) {
			n, err := strconv.ParseFloat(strings.TrimSpace(strings.TrimSuffix(value, unit.suffix)), 64)
			return n * unit.multiplier, err
		}
	}
	return 0, fmt.Errorf("unknown docker memory unit in %q", value)
}
