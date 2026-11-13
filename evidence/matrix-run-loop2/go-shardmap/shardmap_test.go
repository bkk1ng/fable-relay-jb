package shardmap

import (
	"fmt"
	"sync"
	"testing"
)

// --- smoke test ---

func TestGetSetDelete(t *testing.T) {
	m := New[string, int]()

	// missing key
	if _, ok := m.Get("absent"); ok {
		t.Fatal("expected miss for absent key")
	}

	// set and get
	m.Set("x", 42)
	v, ok := m.Get("x")
	if !ok || v != 42 {
		t.Fatalf("want (42, true), got (%d, %v)", v, ok)
	}

	// overwrite
	m.Set("x", 99)
	v, ok = m.Get("x")
	if !ok || v != 99 {
		t.Fatalf("want (99, true) after overwrite, got (%d, %v)", v, ok)
	}

	// delete
	m.Delete("x")
	if _, ok := m.Get("x"); ok {
		t.Fatal("expected miss after delete")
	}

	// delete of absent key is a no-op
	m.Delete("never-existed")
}

func TestLen(t *testing.T) {
	m := New[int, string]()
	const n = 200
	for i := range n {
		m.Set(i, fmt.Sprintf("v%d", i))
	}
	if l := m.Len(); l != n {
		t.Fatalf("want Len %d, got %d", n, l)
	}
	for i := range n / 2 {
		m.Delete(i)
	}
	if l := m.Len(); l != n/2 {
		t.Fatalf("want Len %d after deletes, got %d", n/2, l)
	}
}

func TestRange(t *testing.T) {
	m := New[string, int]()
	want := map[string]int{"a": 1, "b": 2, "c": 3}
	for k, v := range want {
		m.Set(k, v)
	}

	got := make(map[string]int)
	m.Range(func(k string, v int) bool {
		got[k] = v
		return true
	})
	if len(got) != len(want) {
		t.Fatalf("Range collected %d entries, want %d", len(got), len(want))
	}
	for k, wv := range want {
		if got[k] != wv {
			t.Errorf("key %q: want %d, got %d", k, wv, got[k])
		}
	}
}

func TestRangeEarlyStop(t *testing.T) {
	m := New[int, int]()
	for i := range 100 {
		m.Set(i, i)
	}
	count := 0
	m.Range(func(_ int, _ int) bool {
		count++
		return count < 10 // stop after visiting 10 entries
	})
	if count != 10 {
		t.Fatalf("expected early stop at 10, got %d", count)
	}
}

// TestConcurrent hammers all operations from multiple goroutines simultaneously.
func TestConcurrent(t *testing.T) {
	m := New[int, int]()
	const (
		goroutines = 64
		ops        = 1_000
	)

	var wg sync.WaitGroup
	wg.Add(goroutines)
	for g := range goroutines {
		go func(id int) {
			defer wg.Done()
			base := id * ops
			for i := range ops {
				key := base + i
				m.Set(key, key)
				if v, ok := m.Get(key); ok && v != key {
					// value must match what we wrote (another writer can't
					// have the same key because base is unique per goroutine)
					t.Errorf("key %d: got %d, want %d", key, v, key)
				}
				if i%10 == 0 {
					m.Delete(key)
				}
			}
		}(g)
	}
	wg.Wait()

	// Range under no concurrent writers: must not panic and must only
	// return values that equal their keys.
	m.Range(func(k, v int) bool {
		if k != v {
			t.Errorf("Range: key %d has value %d", k, v)
		}
		return true
	})
}

// --- benchmarks ---

func BenchmarkSet(b *testing.B) {
	m := New[int, int]()
	b.ResetTimer()
	for i := range b.N {
		m.Set(i, i)
	}
}

func BenchmarkGet(b *testing.B) {
	m := New[int, int]()
	const size = 1 << 16
	for i := range size {
		m.Set(i, i)
	}
	b.ResetTimer()
	for i := range b.N {
		m.Get(i & (size - 1))
	}
}

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

func BenchmarkGetParallel(b *testing.B) {
	m := New[int, int]()
	const size = 1 << 16
	for i := range size {
		m.Set(i, i)
	}
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			m.Get(i & (size - 1))
			i++
		}
	})
}

func BenchmarkMixedParallel(b *testing.B) {
	// 80% reads, 20% writes — a realistic workload shape.
	m := New[int, int]()
	const size = 1 << 16
	for i := range size {
		m.Set(i, i)
	}
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			k := i & (size - 1)
			if i%5 == 0 {
				m.Set(k, i)
			} else {
				m.Get(k)
			}
			i++
		}
	})
}
