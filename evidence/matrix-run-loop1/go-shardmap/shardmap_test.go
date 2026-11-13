package shardmap

import (
	"fmt"
	"sync"
	"testing"
)

// --- smoke test -----------------------------------------------------------

// TestConcurrentSetGet hammers the map from many goroutines and verifies
// every written value is readable back.
func TestConcurrentSetGet(t *testing.T) {
	const goroutines = 64
	const keysPerGoroutine = 500

	m := New[string, int]()
	var wg sync.WaitGroup

	// Concurrent writes.
	wg.Add(goroutines)
	for g := 0; g < goroutines; g++ {
		g := g
		go func() {
			defer wg.Done()
			for i := 0; i < keysPerGoroutine; i++ {
				key := fmt.Sprintf("g%d-k%d", g, i)
				m.Set(key, g*keysPerGoroutine+i)
			}
		}()
	}
	wg.Wait()

	expectedLen := goroutines * keysPerGoroutine
	if got := m.Len(); got != expectedLen {
		t.Fatalf("Len() = %d, want %d", got, expectedLen)
	}

	// Concurrent reads — every key must be present with the correct value.
	wg.Add(goroutines)
	for g := 0; g < goroutines; g++ {
		g := g
		go func() {
			defer wg.Done()
			for i := 0; i < keysPerGoroutine; i++ {
				key := fmt.Sprintf("g%d-k%d", g, i)
				want := g*keysPerGoroutine + i
				got, ok := m.Get(key)
				if !ok {
					t.Errorf("key %q not found", key)
					return
				}
				if got != want {
					t.Errorf("key %q: got %d, want %d", key, got, want)
				}
			}
		}()
	}
	wg.Wait()
}

// TestDelete verifies that Delete removes entries and that Get reports them
// absent afterwards.
func TestDelete(t *testing.T) {
	m := New[int, string]()

	for i := 0; i < 100; i++ {
		m.Set(i, fmt.Sprintf("v%d", i))
	}
	for i := 0; i < 100; i += 2 {
		m.Delete(i)
	}

	wantLen := 50
	if got := m.Len(); got != wantLen {
		t.Fatalf("Len() after deletes = %d, want %d", got, wantLen)
	}

	for i := 0; i < 100; i++ {
		_, ok := m.Get(i)
		if i%2 == 0 && ok {
			t.Errorf("key %d should have been deleted but is still present", i)
		}
		if i%2 != 0 && !ok {
			t.Errorf("key %d should be present but is missing", i)
		}
	}
}

// TestRange verifies that Range visits every entry exactly once.
func TestRange(t *testing.T) {
	m := New[int, int]()
	const n = 200

	for i := 0; i < n; i++ {
		m.Set(i, i*2)
	}

	seen := make(map[int]int, n)
	m.Range(func(k, v int) bool {
		seen[k] = v
		return true
	})

	if len(seen) != n {
		t.Fatalf("Range visited %d entries, want %d", len(seen), n)
	}
	for i := 0; i < n; i++ {
		if seen[i] != i*2 {
			t.Errorf("key %d: got value %d, want %d", i, seen[i], i*2)
		}
	}
}

// TestRangeEarlyExit verifies that returning false from Range stops iteration.
func TestRangeEarlyExit(t *testing.T) {
	m := New[int, int]()
	for i := 0; i < 100; i++ {
		m.Set(i, i)
	}

	count := 0
	m.Range(func(k, v int) bool {
		count++
		return count < 10 // stop after 10
	})

	if count != 10 {
		t.Fatalf("Range with early exit visited %d entries, want 10", count)
	}
}

// TestMissingKey verifies that Get on an absent key returns the zero value
// and ok == false.
func TestMissingKey(t *testing.T) {
	m := New[string, int]()
	v, ok := m.Get("nope")
	if ok {
		t.Fatalf("expected ok=false for missing key, got value %d", v)
	}
	if v != 0 {
		t.Fatalf("expected zero value for missing key, got %d", v)
	}
}

// TestOverwrite verifies that Set replaces an existing value.
func TestOverwrite(t *testing.T) {
	m := New[string, string]()
	m.Set("key", "first")
	m.Set("key", "second")

	got, ok := m.Get("key")
	if !ok {
		t.Fatal("key not found after overwrite")
	}
	if got != "second" {
		t.Fatalf("got %q, want %q", got, "second")
	}
	if l := m.Len(); l != 1 {
		t.Fatalf("Len() = %d after overwrite, want 1", l)
	}
}

// --- benchmarks -----------------------------------------------------------

// BenchmarkSet measures single-key Set throughput.
func BenchmarkSet(b *testing.B) {
	m := New[int, int]()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		m.Set(i, i)
	}
}

// BenchmarkGet measures single-key Get throughput on a pre-populated map.
func BenchmarkGet(b *testing.B) {
	m := New[int, int]()
	for i := 0; i < 1_000_000; i++ {
		m.Set(i, i)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		m.Get(i % 1_000_000)
	}
}

// BenchmarkSetParallel measures concurrent Set throughput across all shards.
func BenchmarkSetParallel(b *testing.B) {
	m := New[int, int]()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			m.Set(i, i)
			i++
		}
	})
}

// BenchmarkGetParallel measures concurrent read-heavy throughput.
func BenchmarkGetParallel(b *testing.B) {
	m := New[int, int]()
	for i := 0; i < 1_000_000; i++ {
		m.Set(i, i)
	}
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			m.Get(i % 1_000_000)
			i++
		}
	})
}

// BenchmarkMixed measures a 90% read / 10% write workload in parallel.
func BenchmarkMixed(b *testing.B) {
	m := New[int, int]()
	for i := 0; i < 1_000_000; i++ {
		m.Set(i, i)
	}
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			if i%10 == 0 {
				m.Set(i%1_000_000, i)
			} else {
				m.Get(i % 1_000_000)
			}
			i++
		}
	})
}
