package stress

import (
	"testing"
	"time"
)

func TestAllocate(t *testing.T) {
	tests := []struct {
		target, idle, chat, file int
		want                     Allocation
	}{
		{1000, 80, 18, 2, Allocation{Idle: 800, Chat: 180, File: 20}},
		{10, 80, 18, 2, Allocation{Idle: 8, Chat: 2, File: 0}},
		{3, 33, 34, 33, Allocation{Idle: 0, Chat: 2, File: 1}},
		{1, 0, 100, 0, Allocation{Idle: 1, Chat: 0, File: 0}},
		{7, 80, 18, 2, Allocation{Idle: 5, Chat: 2, File: 0}},
	}
	for _, test := range tests {
		got, err := Allocate(test.target, test.idle, test.chat, test.file)
		if err != nil {
			t.Fatal(err)
		}
		if got != test.want {
			t.Errorf("Allocate(%d,%d,%d,%d)=%+v want %+v", test.target, test.idle, test.chat, test.file, got, test.want)
		}
		if got.Chat%2 != 0 || got.Idle+got.Chat+got.File != test.target {
			t.Errorf("invalid allocation %+v", got)
		}
	}
	if _, err := Allocate(10, 80, 10, 0); err == nil {
		t.Fatal("expected invalid mix error")
	}
}
func TestPercentileAndMessageMetrics(t *testing.T) {
	values := make([]float64, 100)
	for i := range values {
		values[i] = float64(i + 1)
	}
	if got := Percentile(values, .95); got != 95 {
		t.Fatalf("p95=%v", got)
	}
	if got := Percentile(values, .99); got != 99 {
		t.Fatalf("p99=%v", got)
	}
	if got := Percentile(nil, .95); got != 0 {
		t.Fatalf("empty=%v", got)
	}
	m := NewMetrics("test", 500)
	start := time.Now()
	m.MessageSent("a", start)
	m.MessageSent("lost", start)
	m.MessageAck("a", start.Add(120*time.Millisecond))
	m.MessageDelivered("a", start.Add(250*time.Millisecond))
	m.MessageDelivered("a", start.Add(300*time.Millisecond))
	totals := m.Totals()
	if totals.AckP95MS != 120 || totals.DeliveryP95MS != 250 || totals.MessageLost != 1 || totals.MessageDuplicates != 1 || totals.MessagesDelivered != 1 {
		t.Fatalf("unexpected totals %+v", totals)
	}
}

func TestTotalsUseConfiguredFileBudgetWhenReportedLimitIsZero(t *testing.T) {
	metrics := NewMetrics("budget", 500)
	used, zero := int64(10<<20), int64(0)
	metrics.BackendBudget(&used, &zero)
	totals := metrics.Totals()
	if !totals.FileBudgetKnown || totals.FileBudgetHighMiB != 10 || totals.FileBudgetLimitMiB != 500 {
		t.Fatalf("budget totals = %+v", totals)
	}
}
