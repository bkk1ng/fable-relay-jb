// Package shardmap provides a generic sharded concurrent map.
// Go 1.22+, standard library only.
package shardmap

import (
	"hash/fnv"
	"sync"
	"testing"
)

const numShards = 32

// shard is one bucket of the map, protected by its own RWMutex.
type shard[K comparable, V any] struct {
	mu    sync.RWMutex
	items map[K]V
}

// ShardMap is a generic concurrent map sharded across [numShards] buckets.
// The zero value is not usable; create one with New.
type ShardMap[K comparable, V any] struct {
	shards [numShards]*shard[K, V]
	hasher func(K) uint64
}

// New returns a ready-to-use ShardMap.
// hasher must be safe to call concurrently and must be deterministic.
// Pass nil to use the built-in FNV-based hasher (only works for
// string, []byte, integer, and float key types).
func New[K comparable, V any](hasher func(K) uint64) *ShardMap[K, V] {
	m := &ShardMap[K, V]{hasher: hasher}
	if m.hasher == nil {
		m.hasher = defaultHasher[K]
	}
	for i := range m.shards {
		m.shards[i] = &shard[K, V]{items: make(map[K]V)}
	}
	return m
}

// shardFor returns the shard responsible for key k.
func (m *ShardMap[K, V]) shardFor(k K) *shard[K, V] {
	h := m.hasher(k)
	return m.shards[h%numShards]
}

// Get returns the value stored for k and whether it was found.
func (m *ShardMap[K, V]) Get(k K) (V, bool) {
	s := m.shardFor(k)
	s.mu.RLock()
	v, ok := s.items[k]
	s.mu.RUnlock()
	return v, ok
}

// Set stores v under k, overwriting any existing value.
func (m *ShardMap[K, V]) Set(k K, v V) {
	s := m.shardFor(k)
	s.mu.Lock()
	s.items[k] = v
	s.mu.Unlock()
}

// Delete removes k from the map. It is a no-op if k is not present.
func (m *ShardMap[K, V]) Delete(k K) {
	s := m.shardFor(k)
	s.mu.Lock()
	delete(s.items, k)
	s.mu.Unlock()
}

// Range calls fn for every key-value pair in the map.
// The iteration order is not defined. fn must not call any ShardMap
// method on the same map (it holds a shard lock during the call).
// Returning false from fn stops iteration.
func (m *ShardMap[K, V]) Range(fn func(k K, v V) bool) {
	for _, s := range m.shards {
		s.mu.RLock()
		for k, v := range s.items {
			if !fn(k, v) {
				s.mu.RUnlock()
				return
			}
		}
		s.mu.RUnlock()
	}
}

// Len returns the total number of entries across all shards.
func (m *ShardMap[K, V]) Len() int {
	total := 0
	for _, s := range m.shards {
		s.mu.RLock()
		total += len(s.items)
		s.mu.RUnlock()
	}
	return total
}

// ---------------------------------------------------------------------------
// Default FNV hasher
// ---------------------------------------------------------------------------

// defaultHasher produces an FNV-1a 64-bit hash for the common key types
// that can be sensibly serialised. For custom or composite key types you
// should supply your own hasher to New.
func defaultHasher[K comparable](k K) uint64 {
	h := fnv.New64a()

	// Use fmt-free serialisation for the types we can handle cheaply.
	switch v := any(k).(type) {
	case string:
		_, _ = h.Write([]byte(v))
	case []byte:
		_, _ = h.Write(v)
	case int:
		h.Write(intToBytes(uint64(v), 8))
	case int8:
		h.Write(intToBytes(uint64(v), 1))
	case int16:
		h.Write(intToBytes(uint64(v), 2))
	case int32:
		h.Write(intToBytes(uint64(v), 4))
	case int64:
		h.Write(intToBytes(uint64(v), 8))
	case uint:
		h.Write(intToBytes(uint64(v), 8))
	case uint8:
		h.Write(intToBytes(uint64(v), 1))
	case uint16:
		h.Write(intToBytes(uint64(v), 2))
	case uint32:
		h.Write(intToBytes(uint64(v), 4))
	case uint64:
		h.Write(intToBytes(v, 8))
	default:
		// Fallback: hash the string representation of the interface value.
		// This is correct but slower; callers with hot paths should supply
		// a type-specific hasher.
		s := any(k).(interface{ String() string })
		_, _ = h.Write([]byte(s.String()))
	}
	return h.Sum64()
}

// intToBytes converts an integer to a little-endian byte slice of length n.
func intToBytes(v uint64, n int) []byte {
	b := make([]byte, n)
	for i := range n {
		b[i] = byte(v >> (i * 8))
	}
	return b
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------

// BenchmarkShardMap exercises Set and Get under parallel load.
// Run with: go test -bench=BenchmarkShardMap -benchmem
func BenchmarkShardMap(b *testing.B) {
	m := New[string, int](nil)

	// Pre-populate so Get has something to find.
	for i := range 1000 {
		m.Set(itoa(i), i)
	}

	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			k := itoa(i % 1000)
			if i%2 == 0 {
				m.Set(k, i)
			} else {
				m.Get(k)
			}
			i++
		}
	})
}

// ---------------------------------------------------------------------------
// Concurrent smoke test
// ---------------------------------------------------------------------------

// TestShardMapConcurrent spawns many goroutines that write and read
// concurrently, then verifies every written key is readable.
func TestShardMapConcurrent(t *testing.T) {
	const (
		goroutines = 64
		keys       = 500
	)

	m := New[string, int](nil)
	var wg sync.WaitGroup

	// Writers
	for g := range goroutines {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for i := range keys {
				m.Set(itoa(id*keys+i), id*keys+i)
			}
		}(g)
	}

	// Concurrent readers (racing against writers intentionally)
	for range goroutines / 2 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			m.Range(func(_ string, _ int) bool { return true })
		}()
	}

	wg.Wait()

	// Verify all written keys are present with correct values.
	total := goroutines * keys
	if got := m.Len(); got != total {
		t.Fatalf("Len() = %d, want %d", got, total)
	}

	for g := range goroutines {
		for i := range keys {
			want := g*keys + i
			k := itoa(want)
			v, ok := m.Get(k)
			if !ok {
				t.Errorf("key %q missing", k)
				continue
			}
			if v != want {
				t.Errorf("key %q: got %d, want %d", k, v, want)
			}
		}
	}

	// Spot-check Delete
	m.Delete(itoa(0))
	if _, ok := m.Get(itoa(0)); ok {
		t.Error("key 0 still present after Delete")
	}
	if got := m.Len(); got != total-1 {
		t.Fatalf("Len() after delete = %d, want %d", got, total-1)
	}
}

// itoa is a zero-dependency integer-to-string helper.
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	buf := make([]byte, 0, 20)
	for n > 0 {
		buf = append(buf, byte('0'+n%10))
		n /= 10
	}
	if neg {
		buf = append(buf, '-')
	}
	// reverse
	for i, j := 0, len(buf)-1; i < j; i, j = i+1, j-1 {
		buf[i], buf[j] = buf[j], buf[i]
	}
	return string(buf)
}
