// Package shardmap provides a generic sharded concurrent map.
// Go 1.22+, standard library only.
package shardmap

import (
	"hash/fnv"
	"fmt"
	"sync"
	"testing"
)

const numShards = 32

// shard holds a portion of the map's key space.
type shard[K comparable, V any] struct {
	mu sync.RWMutex
	m  map[K]V
}

// ShardMap is a generic concurrent map sharded by key hash to reduce lock
// contention. K must be comparable; V can be any type.
type ShardMap[K comparable, V any] struct {
	shards [numShards]shard[K, V]
}

// New returns an initialised ShardMap ready for use.
func New[K comparable, V any]() *ShardMap[K, V] {
	sm := &ShardMap[K, V]{}
	for i := range sm.shards {
		sm.shards[i].m = make(map[K]V)
	}
	return sm
}

// shardIndex returns the shard index for the given key by FNV-hashing its
// fmt.Sprint representation. This is allocation-heavy for hot paths; callers
// that need maximum throughput should use a type-specialised implementation.
func (sm *ShardMap[K, V]) shardIndex(key K) uint32 {
	h := fnv.New32a()
	// fmt.Fprintf into the hash avoids a full Sprint allocation on the fast path
	// for types whose String() doesn't allocate.
	fmt.Fprintf(h, "%v", key)
	return h.Sum32() % numShards
}

// Get returns the value stored for key and whether it was found.
func (sm *ShardMap[K, V]) Get(key K) (V, bool) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.RLock()
	v, ok := s.m[key]
	s.mu.RUnlock()
	return v, ok
}

// Set stores value under key, creating or overwriting the entry.
func (sm *ShardMap[K, V]) Set(key K, value V) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.Lock()
	s.m[key] = value
	s.mu.Unlock()
}

// Delete removes the entry for key. It is a no-op if key is not present.
func (sm *ShardMap[K, V]) Delete(key K) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.Lock()
	delete(s.m, key)
	s.mu.Unlock()
}

// Len returns the total number of entries across all shards.
// Each shard is locked briefly in turn; the count is a consistent snapshot
// of each shard but not an atomic snapshot of the whole map.
func (sm *ShardMap[K, V]) Len() int {
	total := 0
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		total += len(s.m)
		s.mu.RUnlock()
	}
	return total
}

// Range calls fn for every key-value pair in the map. Iteration order is
// undefined. If fn returns false, Range stops early. Each shard is locked
// for the duration of its own iteration, so other goroutines may observe
// partial updates across shards during a Range call.
func (sm *ShardMap[K, V]) Range(fn func(key K, value V) bool) {
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		cont := true
		for k, v := range s.m {
			if !fn(k, v) {
				cont = false
				break
			}
		}
		s.mu.RUnlock()
		if !cont {
			return
		}
	}
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

// BenchmarkShardMapSet measures Set throughput with integer keys.
func BenchmarkShardMapSet(b *testing.B) {
	sm := New[int, int]()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			sm.Set(i, i)
			i++
		}
	})
}

// BenchmarkShardMapGet measures Get throughput on a pre-populated map.
func BenchmarkShardMapGet(b *testing.B) {
	sm := New[int, int]()
	for i := range 1000 {
		sm.Set(i, i)
	}
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			sm.Get(i % 1000)
			i++
		}
	})
}

// BenchmarkShardMapMixed runs concurrent reads and writes at a 4:1 ratio.
func BenchmarkShardMapMixed(b *testing.B) {
	sm := New[int, int]()
	for i := range 1000 {
		sm.Set(i, i)
	}
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			if i%5 == 0 {
				sm.Set(i%1000, i)
			} else {
				sm.Get(i % 1000)
			}
			i++
		}
	})
}

// ---------------------------------------------------------------------------
// Smoke test
// ---------------------------------------------------------------------------

// TestShardMapConcurrent runs a concurrent smoke test: N goroutines each set
// a disjoint key range, then all keys are verified present with correct values.
func TestShardMapConcurrent(t *testing.T) {
	const (
		goroutines = 64
		perRoutine = 1000
	)

	sm := New[int, int]()
	var wg sync.WaitGroup
	wg.Add(goroutines)

	for g := range goroutines {
		g := g
		go func() {
			defer wg.Done()
			base := g * perRoutine
			for i := range perRoutine {
				sm.Set(base+i, base+i)
			}
		}()
	}
	wg.Wait()

	// Verify total count.
	want := goroutines * perRoutine
	if got := sm.Len(); got != want {
		t.Fatalf("Len() = %d, want %d", got, want)
	}

	// Verify every key holds the expected value.
	for g := range goroutines {
		base := g * perRoutine
		for i := range perRoutine {
			key := base + i
			v, ok := sm.Get(key)
			if !ok {
				t.Errorf("key %d not found", key)
				continue
			}
			if v != key {
				t.Errorf("key %d: got %d, want %d", key, v, key)
			}
		}
	}

	// Verify Range visits every entry exactly once.
	seen := make(map[int]int, want)
	sm.Range(func(k, v int) bool {
		seen[k] = v
		return true
	})
	if len(seen) != want {
		t.Fatalf("Range visited %d entries, want %d", len(seen), want)
	}

	// Delete all keys from one goroutine's range and confirm removal.
	const deleteGoroutine = 0
	for i := range perRoutine {
		sm.Delete(deleteGoroutine*perRoutine + i)
	}
	wantAfter := want - perRoutine
	if got := sm.Len(); got != wantAfter {
		t.Fatalf("Len() after delete = %d, want %d", got, wantAfter)
	}
}
