// Package shardmap provides a generic sharded concurrent map.
//
// ShardMap[K, V] distributes keys across 32 independent shards, each
// protected by its own sync.RWMutex, to reduce lock contention under
// concurrent workloads.
package shardmap

import (
	"hash/fnv"
	"sync"
	"unsafe"
)

const numShards = 32

// shard is one bucket of the map.
type shard[K comparable, V any] struct {
	mu sync.RWMutex
	m  map[K]V
}

// ShardMap is a generic, goroutine-safe map sharded across 32 buckets.
// The zero value is not usable; create one with New.
type ShardMap[K comparable, V any] struct {
	shards [numShards]shard[K, V]
}

// New returns an initialised ShardMap ready for concurrent use.
func New[K comparable, V any]() *ShardMap[K, V] {
	sm := &ShardMap[K, V]{}
	for i := range sm.shards {
		sm.shards[i].m = make(map[K]V)
	}
	return sm
}

// shardIndex hashes key to a shard index in [0, numShards).
// It serialises the key's memory representation through FNV-1a, which is
// fast and has good distribution for small keys.
func (sm *ShardMap[K, V]) shardIndex(key K) uint32 {
	h := fnv.New32a()

	// Obtain a stable byte view of any comparable key without reflection.
	// unsafe.Slice over the key's memory is portable for value types;
	// it covers strings, integers, structs, and arrays.
	size := unsafe.Sizeof(key)
	if size > 0 {
		ptr := unsafe.Pointer(&key)
		b := unsafe.Slice((*byte)(ptr), size)
		_, _ = h.Write(b)
	}
	return h.Sum32() % numShards
}

// Set stores value under key, overwriting any previous value.
func (sm *ShardMap[K, V]) Set(key K, value V) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.Lock()
	s.m[key] = value
	s.mu.Unlock()
}

// Get returns the value for key and whether it was present.
func (sm *ShardMap[K, V]) Get(key K) (V, bool) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.RLock()
	v, ok := s.m[key]
	s.mu.RUnlock()
	return v, ok
}

// Delete removes the entry for key. It is a no-op if key is absent.
func (sm *ShardMap[K, V]) Delete(key K) {
	s := &sm.shards[sm.shardIndex(key)]
	s.mu.Lock()
	delete(s.m, key)
	s.mu.Unlock()
}

// Len returns the total number of entries across all shards.
// Each shard is locked briefly in turn; the result is a consistent
// snapshot only if no mutations occur during the call.
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

// Range calls fn for every key-value pair in the map.
// Iteration order is undefined. If fn returns false, Range stops early.
// Each shard is locked for the duration of its iteration; other shards
// remain accessible while one is being walked.
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
