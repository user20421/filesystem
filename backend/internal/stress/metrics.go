package stress

import (
	"math"
	"sort"
	"sync"
	"time"
)

type Metrics struct {
	mu                                                                    sync.Mutex
	runID                                                                 string
	started                                                               time.Time
	online, connectionFailed, reconnects                                  int64
	messagesSent, messagesDelivered, messageDuplicates                    int64
	filesQueued, filesActive, filesCompleted, filesFailed, bytesCompleted int64
	healthFailures, stableSamples, stableOnlineSum, stableOnlineMin       int64
	ackLatency, deliveryLatency, fileQueueLatency                         []float64
	fileCompletionLatency                                                 []float64
	pendingAck, pendingDelivery                                           map[string]time.Time
	delivered                                                             map[string]struct{}
	backendBudgetUsedMiB, backendBudgetHighMiB, backendBudgetLimitMiB     *float64
	backendRSSMiB, backendCPUPercent                                      *float64
	backendGoHeapMiB                                                      *float64
	backendGoroutines                                                     *int
	backendChunkSlotsUsed, backendChunkSlotsLimit                         *int64
	configuredBudgetLimitMiB                                              float64
}

func NewMetrics(runID string, fileBudgetMiB int64) *Metrics {
	return &Metrics{runID: runID, started: time.Now(), configuredBudgetLimitMiB: float64(fileBudgetMiB), pendingAck: make(map[string]time.Time),
		pendingDelivery: make(map[string]time.Time), delivered: make(map[string]struct{})}
}

func Percentile(values []float64, percentile float64) float64 {
	if len(values) == 0 {
		return 0
	}
	copyOfValues := append([]float64(nil), values...)
	sort.Float64s(copyOfValues)
	rank := int(math.Ceil(percentile * float64(len(copyOfValues))))
	if rank < 1 {
		rank = 1
	}
	if rank > len(copyOfValues) {
		rank = len(copyOfValues)
	}
	return copyOfValues[rank-1]
}

func (m *Metrics) SetOnline(delta int64) { m.mu.Lock(); m.online += delta; m.mu.Unlock() }
func (m *Metrics) Online() int64         { m.mu.Lock(); defer m.mu.Unlock(); return m.online }
func (m *Metrics) PendingMessages() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.pendingAck) + len(m.pendingDelivery)
}
func (m *Metrics) ConnectionFailed() { m.mu.Lock(); m.connectionFailed++; m.mu.Unlock() }
func (m *Metrics) Reconnected()      { m.mu.Lock(); m.reconnects++; m.mu.Unlock() }
func (m *Metrics) MessageSent(id string, at time.Time) {
	m.mu.Lock()
	m.messagesSent++
	m.pendingAck[id] = at
	m.pendingDelivery[id] = at
	m.mu.Unlock()
}
func (m *Metrics) MessageAck(id string, at time.Time) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if sent, ok := m.pendingAck[id]; ok {
		m.ackLatency = append(m.ackLatency, ms(at.Sub(sent)))
		delete(m.pendingAck, id)
	}
}
func (m *Metrics) MessageDelivered(id string, at time.Time) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, exists := m.delivered[id]; exists {
		m.messageDuplicates++
		return
	}
	m.delivered[id] = struct{}{}
	m.messagesDelivered++
	if sent, ok := m.pendingDelivery[id]; ok {
		m.deliveryLatency = append(m.deliveryLatency, ms(at.Sub(sent)))
		delete(m.pendingDelivery, id)
	}
}
func (m *Metrics) FileQueued(delta int64) { m.mu.Lock(); m.filesQueued += delta; m.mu.Unlock() }
func (m *Metrics) FileActive(delta int64) { m.mu.Lock(); m.filesActive += delta; m.mu.Unlock() }
func (m *Metrics) FileCompleted(bytes int64) {
	m.mu.Lock()
	m.filesCompleted++
	m.bytesCompleted += bytes
	m.mu.Unlock()
}
func (m *Metrics) FileFailed() { m.mu.Lock(); m.filesFailed++; m.mu.Unlock() }
func (m *Metrics) FileQueueWait(d time.Duration) {
	m.mu.Lock()
	m.fileQueueLatency = append(m.fileQueueLatency, ms(d))
	m.mu.Unlock()
}

// FileCompletion 记录单文件端到端完成耗时：从任务入队到下载 SHA-256 校验通过，包含排队等待。
func (m *Metrics) FileCompletion(d time.Duration) {
	m.mu.Lock()
	m.fileCompletionLatency = append(m.fileCompletionLatency, ms(d))
	m.mu.Unlock()
}

func mean(values []float64) float64 {
	if len(values) == 0 {
		return 0
	}
	sum := float64(0)
	for _, v := range values {
		sum += v
	}
	return sum / float64(len(values))
}
func (m *Metrics) HealthFailed() { m.mu.Lock(); m.healthFailures++; m.mu.Unlock() }
func (m *Metrics) BackendRuntime(heapMiB *float64, goroutines *int) {
	m.mu.Lock()
	m.backendGoHeapMiB, m.backendGoroutines = clone(heapMiB), cloneInt(goroutines)
	m.mu.Unlock()
}
func (m *Metrics) BackendStats(rss, cpu *float64) {
	m.mu.Lock()
	m.backendRSSMiB, m.backendCPUPercent = clone(rss), clone(cpu)
	m.mu.Unlock()
}
func (m *Metrics) BackendBudget(usedBytes, limitBytes *int64) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if usedBytes == nil || limitBytes == nil {
		m.backendBudgetUsedMiB, m.backendBudgetLimitMiB = nil, nil
		return
	}
	used, limit := float64(*usedBytes)/(1<<20), float64(*limitBytes)/(1<<20)
	m.backendBudgetUsedMiB, m.backendBudgetLimitMiB = &used, &limit
	if m.backendBudgetHighMiB == nil || used > *m.backendBudgetHighMiB {
		m.backendBudgetHighMiB = clone(&used)
	}
}
func (m *Metrics) BackendChunkSlots(used, limit *int64) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if used == nil || limit == nil {
		m.backendChunkSlotsUsed, m.backendChunkSlotsLimit = nil, nil
		return
	}
	m.backendChunkSlotsUsed, m.backendChunkSlotsLimit = cloneInt64(used), cloneInt64(limit)
}
func (m *Metrics) StableSample() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.stableSamples++
	m.stableOnlineSum += m.online
	if m.stableSamples == 1 || m.online < m.stableOnlineMin {
		m.stableOnlineMin = m.online
	}
}
func clone(v *float64) *float64 {
	if v == nil {
		return nil
	}
	c := *v
	return &c
}
func cloneInt(v *int) *int {
	if v == nil {
		return nil
	}
	c := *v
	return &c
}
func cloneInt64(v *int64) *int64 {
	if v == nil {
		return nil
	}
	c := *v
	return &c
}
func value(v *float64) float64 {
	if v == nil {
		return 0
	}
	return *v
}
func ms(d time.Duration) float64 { return float64(d) / float64(time.Millisecond) }

type Progress struct {
	Type                string    `json:"type"`
	RunID               string    `json:"run_id"`
	Phase               string    `json:"phase"`
	Target              int       `json:"target"`
	Online              int64     `json:"online"`
	ConnectionFailed    int64     `json:"connection_failed"`
	Reconnects          int64     `json:"reconnects"`
	MessagesSent        int64     `json:"messages_sent"`
	MessagesDelivered   int64     `json:"messages_delivered"`
	MessageDuplicates   int64     `json:"message_duplicates"`
	MessageLost         int64     `json:"message_lost"`
	AckP95MS            float64   `json:"ack_p95_ms"`
	AckP99MS            float64   `json:"ack_p99_ms"`
	DeliveryP95MS       float64   `json:"delivery_p95_ms"`
	FilesQueued         int64     `json:"files_queued"`
	FilesActive         int64     `json:"files_active"`
	FilesCompleted      int64     `json:"files_completed"`
	FilesFailed         int64     `json:"files_failed"`
	BytesCompleted      int64     `json:"bytes_completed"`
	FileMiBPerSec       float64   `json:"file_mib_per_sec"`
	FileQueueP95MS      float64   `json:"file_queue_p95_ms"`
	BackendRSSMiB       *float64  `json:"backend_rss_mib"`
	BackendCPUPercent   *float64  `json:"backend_cpu_percent"`
	GoHeapMiB           *float64  `json:"go_heap_mib"`
	Goroutines          *int      `json:"goroutines"`
	FileBudgetUsedMiB   *float64  `json:"file_budget_used_mib"`
	FileBudgetLimitMiB  *float64  `json:"file_budget_limit_mib"`
	FileChunkSlotsUsed  *int64    `json:"file_chunk_slots_used"`
	FileChunkSlotsLimit *int64    `json:"file_chunk_slots_limit"`
	Timestamp           time.Time `json:"timestamp"`
}

type Totals struct {
	MessagesSent, MessagesDelivered, MessageDuplicates, MessageLost int64
	AckP95MS, AckP99MS, DeliveryP95MS                               float64
	FilesCompleted, FilesFailed, BytesCompleted                     int64
	FileQueueP95MS                                                  float64
	FileQueueAvgMS, FileCompletionAvgMS                             float64
	ConnectionFailed, Reconnects, HealthFailures                    int64
	StableSamples, StableOnlineMin                                  int64
	StableOnlineAverage                                             float64
	FileBudgetHighMiB                                               float64
	FileBudgetLimitMiB                                              float64
	FileBudgetKnown                                                 bool
}

func (m *Metrics) Snapshot(target int, phase string, throughput float64) Progress {
	m.mu.Lock()
	defer m.mu.Unlock()
	lost := m.messagesSent - m.messagesDelivered
	if lost < 0 {
		lost = 0
	}
	return Progress{Type: "progress", RunID: m.runID, Phase: phase, Target: target, Online: m.online,
		ConnectionFailed: m.connectionFailed, Reconnects: m.reconnects, MessagesSent: m.messagesSent,
		MessagesDelivered: m.messagesDelivered, MessageDuplicates: m.messageDuplicates, MessageLost: lost,
		AckP95MS: Percentile(m.ackLatency, .95), AckP99MS: Percentile(m.ackLatency, .99), DeliveryP95MS: Percentile(m.deliveryLatency, .95),
		FilesQueued: m.filesQueued, FilesActive: m.filesActive, FilesCompleted: m.filesCompleted, FilesFailed: m.filesFailed,
		BytesCompleted: m.bytesCompleted, FileMiBPerSec: throughput, FileQueueP95MS: Percentile(m.fileQueueLatency, .95),
		BackendRSSMiB: clone(m.backendRSSMiB), BackendCPUPercent: clone(m.backendCPUPercent), GoHeapMiB: clone(m.backendGoHeapMiB), Goroutines: cloneInt(m.backendGoroutines),
		FileBudgetUsedMiB: clone(m.backendBudgetUsedMiB), FileBudgetLimitMiB: clone(m.backendBudgetLimitMiB),
		FileChunkSlotsUsed: cloneInt64(m.backendChunkSlotsUsed), FileChunkSlotsLimit: cloneInt64(m.backendChunkSlotsLimit), Timestamp: time.Now().UTC()}
}

func (m *Metrics) Totals() Totals {
	m.mu.Lock()
	defer m.mu.Unlock()
	lost := m.messagesSent - m.messagesDelivered
	if lost < 0 {
		lost = 0
	}
	average := float64(0)
	if m.stableSamples > 0 {
		average = float64(m.stableOnlineSum) / float64(m.stableSamples)
	}
	budgetLimit := value(m.backendBudgetLimitMiB)
	if budgetLimit <= 0 {
		budgetLimit = m.configuredBudgetLimitMiB
	}
	return Totals{MessagesSent: m.messagesSent, MessagesDelivered: m.messagesDelivered, MessageDuplicates: m.messageDuplicates, MessageLost: lost,
		AckP95MS: Percentile(m.ackLatency, .95), AckP99MS: Percentile(m.ackLatency, .99), DeliveryP95MS: Percentile(m.deliveryLatency, .95),
		FilesCompleted: m.filesCompleted, FilesFailed: m.filesFailed, BytesCompleted: m.bytesCompleted, FileQueueP95MS: Percentile(m.fileQueueLatency, .95),
		FileQueueAvgMS: mean(m.fileQueueLatency), FileCompletionAvgMS: mean(m.fileCompletionLatency),
		ConnectionFailed: m.connectionFailed, Reconnects: m.reconnects, HealthFailures: m.healthFailures, StableSamples: m.stableSamples,
		StableOnlineMin: m.stableOnlineMin, StableOnlineAverage: average, FileBudgetHighMiB: value(m.backendBudgetHighMiB), FileBudgetLimitMiB: budgetLimit, FileBudgetKnown: m.backendBudgetHighMiB != nil}
}
