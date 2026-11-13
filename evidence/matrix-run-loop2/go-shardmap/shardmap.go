// Package shardmap provides a generic concurrent map sharded across 32 buckets.
// Each shard carries its own RWMutex so reads in different shards never block
// each other and writes contend only within their shard.
package shardmap

import (
	"fmt"
	"hash/fnv"
	"sync"
)

const numShards = 32

type shard[K comparable, V any] struct {
	mu sync.RWMutex
	m  map[K]V
}

// ShardMap is a goroutine-safe map[K]V backed by 32 independent shards.
type ShardMap[K comparable, V any] struct {
	shards [numShards]shard[K, V]
}

// New returns an initialised *ShardMap ready for use.
func New[K comparable, V any]() *ShardMap[K, V] {
	sm := &ShardMap[K, V]{}
	for i := range sm.shards {
		sm.shards[i].m = make(map[K]V)
	}
	return sm
}

// shardFor derives a shard index from key using FNV-1a 32-bit hashing.
// fmt.Fprintf converts the generic key to bytes without unsafe; for
// high-throughput scenarios with string or integer keys, a type switch
// on the key would be faster but would require a narrower type constraint.
func (sm *ShardMap[K, V]) shardFor(key K) *shard[K, V] {
	h := fnv.New32a()
	fmt.Fprintf(h, "%v", key) //nolint:errcheck // hash.Hash32 never returns an error
	return &sm.shards[h.Sum32()%numShards]
}

// Get returns the value stored under key and whether the key was present.
func (sm *ShardMap[K, V]) Get(key K) (V, bool) {
	s := sm.shardFor(key)
	s.mu.RLock()
	v, ok := s.m[key]
	s.mu.RUnlock()
	return v, ok
}

// Set stores value under key, overwriting any existing entry.
func (sm *ShardMap[K, V]) Set(key K, value V) {
	s := sm.shardFor(key)
	s.mu.Lock()
	s.m[key] = value
	s.mu.Unlock()
}

// Delete removes key from the map. It is a no-op when key is absent.
func (sm *ShardMap[K, V]) Delete(key K) {
	s := sm.shardFor(key)
	s.mu.Lock()
	delete(s.m, key)
	s.mu.Unlock()
}

// Range calls fn for every key-value pair in the map. Returning false from fn
// stops iteration immediately. Each shard is locked for the duration of its
// own iteration, so Range provides per-shard consistency: concurrent writes to
// other shards may become visible as iteration advances across shard boundaries.
func (sm *ShardMap[K, V]) Range(fn func(K, V) bool) {
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		stop := false
		for k, v := range s.m {
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
// Because each shard is locked individually, the value is a consistent
// sum at the moment each shard was sampled, not an atomic snapshot.
func (sm *ShardMap[K, V]) Len() int {
	n := 0
	for i := range sm.shards {
		s := &sm.shards[i]
		s.mu.RLock()
		n += len(s.m)
		s.mu.RUnlock()
	}
	return n
}
