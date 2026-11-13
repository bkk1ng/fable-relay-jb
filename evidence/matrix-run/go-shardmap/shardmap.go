// Package shardmap provides a generic concurrent map sharded across 32 buckets,
// each guarded by its own sync.RWMutex. Keys are routed to a shard by hashing
// their fmt representation with FNV-32a.
package shardmap

import (
	"fmt"
	"hash/fnv"
	"sync"
)

const numShards = 32

// shard is one of the 32 buckets inside a ShardMap.
type shard[K comparable, V any] struct {
	mu sync.RWMutex
	m  map[K]V
}

// ShardMap is a generic concurrent hash map with 32 internal shards.
// All exported methods are safe for concurrent use.
type ShardMap[K comparable, V any] struct {
	shards [numShards]shard[K, V]
}

// New allocates and initialises a ShardMap ready for use.
func New[K comparable, V any]() *ShardMap[K, V] {
	sm := new(ShardMap[K, V])
	for i := range sm.shards {
		sm.shards[i].m = make(map[K]V)
	}
	return sm
}

// shardOf returns the shard index for key.
// The key is formatted with %v and fed to FNV-32a; collisions across
// distinct comparable values are therefore possible but rare in practice.
func (sm *ShardMap[K, V]) shardOf(key K) uint32 {
	h := fnv.New32a()
	fmt.Fprintf(h, "%v", key) // fnv.Hash32 never returns a non-nil error
	return h.Sum32() % numShards
}

// Get returns the value stored under key and whether it was present.
func (sm *ShardMap[K, V]) Get(key K) (V, bool) {
	s := &sm.shards[sm.shardOf(key)]
	s.mu.RLock()
	v, ok := s.m[key]
	s.mu.RUnlock()
	return v, ok
}

// Set stores value under key, replacing any previous entry.
func (sm *ShardMap[K, V]) Set(key K, value V) {
	s := &sm.shards[sm.shardOf(key)]
	s.mu.Lock()
	s.m[key] = value
	s.mu.Unlock()
}

// Delete removes key from the map. It is a no-op if key is absent.
func (sm *ShardMap[K, V]) Delete(key K) {
	s := &sm.shards[sm.shardOf(key)]
	s.mu.Lock()
	delete(s.m, key)
	s.mu.Unlock()
}

// Range calls fn for every key-value pair in the map in an unspecified order.
// If fn returns false, iteration stops immediately.
//
// fn is invoked while the current shard's read lock is held. It must not call
// Set, Delete, or Range on the same ShardMap; calling Get on the same map is
// safe only when the target key lives in a different shard.
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

// Len returns the total number of entries across all shards.
// It acquires each shard's read lock in turn, so under heavy concurrent
// writes the result reflects an approximate rather than fully atomic count.
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
