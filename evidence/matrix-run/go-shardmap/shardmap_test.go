package shardmap

import (
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
)

// TestConcurrentSmoke hammers the map from many goroutines doing mixed
// Set/Get/Delete/Range/Len operations in parallel. It asserts no data race
// trips, every written key is readable back, and Len reports the expected
// count once the writers settle.
func TestConcurrentSmoke(t *testing.T) {
	const writers = 8
	const readers = 8
	const keysPerWriter = 2000

	sm := New[int, int]()

	var wg sync.WaitGroup
	wg.Add(writers + readers)

	// writers: each owns a disjoint key range and repeatedly sets/gets/deletes
	for w := 0; w < writers; w++ {
		w := w
		go func() {
			defer wg.Done()
			base := w * keysPerWriter
			for i := 0; i < keysPerWriter; i++ {
				k := base + i
				sm.Set(k, k*10)
				if v, ok := sm.Get(k); !ok || v != k*10 {
					t.Errorf("writer %d: key %d missing or wrong: got %v ok=%v", w, k, v, ok)
					return
				}
				sm.Delete(k)
				sm.Set(k, k*10+1) // resurrect
			}
		}()
	}

	// readers: read random keys and Range over the map, just exercising locks
	for r := 0; r < readers; r++ {
		go func() {
			defer wg.Done()
			for i := 0; i < keysPerWriter; i++ {
				// read a key that some writer owns
				_, _ = sm.Get(i % (writers * keysPerWriter))
				// walk the whole map; stop early sometimes
				count := 0
				sm.Range(func(k, v int) bool {
					count++
					return count < 50 // bail early on some iterations
				})
				_ = sm.Len()
			}
		}()
	}

	wg.Wait()

	// after all writers finish, every writer's last action was Set(k, k*10+1),
	// so every key in every writer's range should be present with that value.
	const totalKeys = writers * keysPerWriter
	for k := 0; k < totalKeys; k++ {
		v, ok := sm.Get(k)
		if !ok {
			t.Errorf("post-check: key %d missing", k)
			continue
		}
		if v != k*10+1 {
			t.Errorf("post-check: key %d = %d, want %d", k, v, k*10+1)
		}
	}

	if got := sm.Len(); got != totalKeys {
		t.Errorf("Len = %d, want %d", got, totalKeys)
	}
}

// TestShardDistribution checks that keys spread across shards rather than
// piling into one, which would defeat the point of sharding.
func TestShardDistribution(t *testing.T) {
	sm := New[int, string]()
	for i := 0; i < 1000; i++ {
		sm.Set(i, fmt.Sprintf("v%d", i))
	}

	counts := make([]int, numShards)
	for i := range sm.shards {
		sm.shards[i].mu.RLock()
		counts[i] = len(sm.shards[i].m)
		sm.shards[i].mu.RUnlock()
	}

	used := 0
	for _, c := range counts {
		if c > 0 {
			used++
		}
	}
	// 1000 keys over 32 shards should touch at least 20 shards with FNV-32a.
	if used < 20 {
		t.Errorf("poor distribution: only %d/%d shards used", used, numShards)
	}
}

// TestRangeStopsEarly verifies Range honours a false return from fn.
func TestRangeStopsEarly(t *testing.T) {
	sm := New[int, int]()
	for i := 0; i < 100; i++ {
		sm.Set(i, i)
	}

	seen := 0
	sm.Range(func(k, v int) bool {
		seen++
		return seen < 5
	})

	if seen != 5 {
		t.Errorf("Range visited %d items, want 5 before stopping", seen)
	}
}

// BenchmarkSet measures single-key Set throughput under a tight loop.
func BenchmarkSet(b *testing.B) {
	sm := New[int, int]()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		sm.Set(i, i)
	}
}

// BenchmarkGet measures single-key Get throughput after pre-populating.
func BenchmarkGet(b *testing.B) {
	sm := New[int, int]()
	for i := 0; i < 10000; i++ {
		sm.Set(i, i)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_, _ = sm.Get(i % 10000)
	}
}

// BenchmarkSetParallel runs Set from multiple goroutines to exercise
// contention across shards.
func BenchmarkSetParallel(b *testing.B) {
	sm := New[int, int]()
	b.ReportAllocs()
	var counter atomic.Int64
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			k := counter.Add(1)
			sm.Set(int(k), int(k))
		}
	})
}

// BenchmarkGetParallel runs Get from multiple goroutines against a
// pre-populated map.
func BenchmarkGetParallel(b *testing.B) {
	sm := New[int, int]()
	for i := 0; i < 10000; i++ {
		sm.Set(i, i)
	}
	var counter atomic.Int64
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			k := counter.Add(1) % 10000
			_, _ = sm.Get(int(k))
		}
	})
}

// BenchmarkConcurrentMixed simulates real usage: a mix of Get, Set, and
// Delete from many goroutines, reported per operation.
func BenchmarkConcurrentMixed(b *testing.B) {
	sm := New[int, int]()
	for i := 0; i < 10000; i++ {
		sm.Set(i, i)
	}
	var counter atomic.Int64
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			k := int(counter.Add(1)) % 10000
			switch k % 3 {
			case 0:
				_, _ = sm.Get(k)
			case 1:
				sm.Set(k, k)
			case 2:
				sm.Delete(k)
			}
		}
	})
}
