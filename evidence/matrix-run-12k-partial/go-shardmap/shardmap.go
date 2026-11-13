// Package shardmap provides a generic sharded concurrent map.
//
// The benchmark and smoke-test functions at the bottom of this file use the
// testing package. To run them, either rename this file to shardmap_test.go
// (changing the package line to `package shardmap_test` and adjusting imports)
// or create a thin _test.go wrapper that calls them directly.
package shardmap

import (
	"encoding/binary"
	"fmt"
	"hash/fnv"
	"sync"
	"testing"
)

const numShards = 32

// shard is one independent bucket: its own map and its own lock.
type shard[K comparable, V any] struct {
	mu   sync.RWMutex
	data map[K]V
}

// ShardMap is a generic concurrent map distributed across numShards shards.
// Each shard is guarded by an independent RWMutex, so reads on different
// shards never contend with each other.
type ShardMap[K comparable, V any] struct {
	shards [numShards]shard[K, V]
}

// New allocates and initialises a ShardMap.
func New[K comparable, V any]() *ShardMap[K, V] {
	sm := &ShardMap[K, V]{}
	for i := range sm.shards {
		sm.shards[i].data = make(map[K]V)
	}
	return sm
}

// hashKey returns a 32-bit FNV-1a hash of key.
// A type switch handles the most common key types with direct byte conversion;
// all others fall back to fmt.Fprintf, which is correct for any comparable.
func hashKey[K comparable](key K) uint32 {
	h := fnv.New32a()
	switch k := any(key).(type) {
	case string:
		h.Write([]byte(k))
	case int:
		var buf [8]byte
		binary.LittleEndian.PutUint64(buf[:], uint64(k))
		h.Write(buf[:])
	case int32:
		var buf [4]byte
		binary.LittleEndian.PutUint32(buf[:], uint32(k))
		h.Write(buf[:])
	case int64:
		var buf [8]byte
		binary.LittleEndian.PutUint64(buf[:], uint64(k))
		h.Write(buf[:])
	case uint:
		var buf [8]byte
		binary.LittleEndian.PutUint64(buf[:], uint64(k))
		h.Write(buf[:])
	case uint32:
		var buf [4]byte
		binary.LittleEndian.PutUint32(buf[:], k)
		h.Write(buf[:])
	case uint64:
		var buf [8]byte
		binary.LittleEndian.PutUint64(buf[:], k)
		h.Write(buf[:])
	default:
		fmt.Fprintf(h, "%v", key) // hash/fnv implements io.Writer
	}
	return h.Sum32()
}

// pick returns the shard responsible for key.
func (sm *ShardMap[K, V]) pick(key K) *shard[K, V] {
	return &sm.shards[hashKey(key)%numShards]
}

// Get returns the value stored for key and whether it was present.
func (sm *ShardMap[K, V]) Get(key K) (V, bool) {
	s := sm.pick(key)
	s.mu.RLock()
	v, ok := s.data[key]
	s.mu.RUnlock()
	return v, ok
}

// Set stores value under key, overwriting any previous value.
func (sm *ShardMap[K, V]) Set(key K, value V) {
	s := sm.pick(key)
	s.mu.Lock()
	s.data[key] = value
	s.mu.Unlock()
}

// Delete removes key from the map. It is a no-op if the key is absent.
func (sm *ShardMap[K, V]) Delete(key K) {
	s := sm.pick(key)
	s.mu.Lock()
	delete(s.data, key)
	s.mu.Unlock()
}

// Range calls fn for every key/value pair in the map in an unspecified order.
// If fn returns false, iteration stops immediately (the remaining shards are
// skipped). Do not call Set, Delete, or Range on the same map from within fn:
// it will deadlock.
func (sm *ShardMap[K, V]) Range(fn func(K, V) bool) {
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		stop := false
		for k, v := range s.data {
			if !fn(k, v) {
				stop = true
				break
			}
		}
		s.mu.RUnlock()
		if stop {
			return
		}
	}
}

// Len returns the total number of entries across all shards.
// Because each shard is locked independently, Len is not an atomic snapshot.
func (sm *ShardMap[K, V]) Len() int {
	n := 0
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		n += len(s.data)
		s.mu.RUnlock()
	}
	return n
}

// ---------------------------------------------------------------------------
// Concurrent smoke test
//
// Wire this from a _test.go file:
//
//	func TestShardMapConcurrent(t *testing.T) { shardmap.SmokeTestConcurrent(t) }
// ---------------------------------------------------------------------------

// SmokeTestConcurrent runs a concurrent correctness check on ShardMap[int,int].
// 64 goroutines each write ops keys, read them back, then delete half.
func SmokeTestConcurrent(t *testing.T) {
	t.Helper()
	const (
		goroutines = 64
		ops        = 1000
	)

	sm := New[int, int]()
	var wg sync.WaitGroup
	wg.Add(goroutines)

	for g := 0; g < goroutines; g++ {
		g := g
		go func() {
			defer wg.Done()
			base := g * ops

			for i := 0; i < ops; i++ {
				sm.Set(base+i, base+i)
			}
			for i := 0; i < ops; i++ {
				v, ok := sm.Get(base + i)
				if !ok {
					t.Errorf("Get(%d): key missing after Set", base+i)
					continue
				}
				if v != base+i {
					t.Errorf("Get(%d) = %d, want %d", base+i, v, base+i)
				}
			}
			for i := 0; i < ops/2; i++ {
				sm.Delete(base + i)
			}
		}()
	}
	wg.Wait()

	// Each goroutine kept ops - ops/2 entries.
	wantLen := goroutines * (ops - ops/2)
	if got := sm.Len(); got != wantLen {
		t.Errorf("Len() = %d, want %d", got, wantLen)
	}

	// Range must visit every surviving key exactly once.
	seen := make(map[int]int, wantLen)
	sm.Range(func(k, v int) bool {
		seen[k]++
		return true
	})
	if len(seen) != wantLen {
		t.Errorf("Range visited %d distinct keys, want %d", len(seen), wantLen)
	}
	for k, count := range seen {
		if count != 1 {
			t.Errorf("Range visited key %d %d times, want 1", k, count)
		}
	}
}

// ---------------------------------------------------------------------------
// Benchmarks
//
// Wire from a _test.go file:
//
//	func BenchmarkShardMapSet(b *testing.B)   { shardmap.BenchSet(b) }
//	func BenchmarkShardMapGet(b *testing.B)   { shardmap.BenchGet(b) }
//	func BenchmarkShardMapMixed(b *testing.B) { shardmap.BenchMixed(b) }
// ---------------------------------------------------------------------------

// BenchSet benchmarks concurrent Set throughput.
func BenchSet(b *testing.B) {
	b.Helper()
	sm := New[int, int]()
	b.ReportAllocs()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			sm.Set(i, i)
			i++
		}
	})
}

// BenchGet benchmarks concurrent Get throughput after a warm-up fill.
func BenchGet(b *testing.B) {
	b.Helper()
	const fill = 1 << 14 // 16 384 entries — spreads across all 32 shards
	sm := New[int, int]()
	for i := 0; i < fill; i++ {
		sm.Set(i, i)
	}
	b.ReportAllocs()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			sm.Get(i & (fill - 1))
			i++
		}
	})
}

// BenchMixed benchmarks a 70 % read / 20 % write / 10 % delete mix.
func BenchMixed(b *testing.B) {
	b.Helper()
	const fill = 1 << 14
	sm := New[int, int]()
	for i := 0; i < fill; i++ {
		sm.Set(i, i)
	}
	b.ReportAllocs()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			k := i & (fill - 1)
			switch i % 10 {
			case 0, 1: // 20 % writes
				sm.Set(k, i)
			case 2: // 10 % deletes
				sm.Delete(k)
			default: // 70 % reads
				sm.Get(k)
			}
			i++
		}
	})
}
