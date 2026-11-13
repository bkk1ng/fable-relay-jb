use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A generic fixed-capacity LRU cache.
///
/// Freshness order is maintained by a [`VecDeque<K>`] (front = MRU, back = LRU)
/// alongside a [`HashMap<K, V>`] for O(1) value lookup.  Key operations that
/// touch the order deque are O(n) due to linear search; for a production LRU
/// prefer an intrusive doubly-linked list.
pub struct LruCache<K, V> {
    capacity: usize,
    /// Front = most-recently used; back = least-recently used.
    order: VecDeque<K>,
    map: HashMap<K, V>,
}

impl<K, V> LruCache<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Creates a new cache with the given `capacity`.
    ///
    /// A capacity of 0 is valid; every [`put`](Self::put) call is a no-op.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            order: VecDeque::with_capacity(capacity),
            map: HashMap::with_capacity(capacity),
        }
    }

    /// Returns the number of entries currently held.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns `true` when the cache holds no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Looks up `key`, promotes it to MRU position, and returns a reference
    /// to its value, or `None` if the key is absent.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if self.map.contains_key(key) {
            Self::move_to_front(&mut self.order, key);
            self.map.get(key)
        } else {
            None
        }
    }

    /// Returns a reference to the value for `key` without changing freshness
    /// order, or `None` if the key is absent.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Inserts or updates `key → value`.
    ///
    /// * **Update**: replaces the stored value and promotes the key to MRU.
    /// * **Insert**: if the cache is at capacity, the LRU entry is evicted
    ///   first; then the new entry is placed at MRU.
    /// * **Zero capacity**: the call is a no-op; nothing is stored.
    pub fn put(&mut self, key: K, value: V) {
        if self.capacity == 0 {
            return;
        }

        if self.map.contains_key(&key) {
            // Update in place and re-promote.
            self.map.insert(key.clone(), value);
            Self::move_to_front(&mut self.order, &key);
        } else {
            // Evict the LRU entry if we are at capacity.
            if self.map.len() == self.capacity {
                if let Some(lru_key) = self.order.pop_back() {
                    self.map.remove(&lru_key);
                }
            }
            self.map.insert(key.clone(), value);
            self.order.push_front(key);
        }
    }

    /// Removes `key` from the cache, returning its value if it was present.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        // Remove from the value store first so we can return the value.
        let value = self.map.remove(key)?;
        // Keep the order deque consistent.
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        Some(value)
    }

    /// Removes all entries, resetting the cache to an empty state.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    // ── private helpers ──────────────────────────────────────────────────────

    /// Moves `key` to the front (MRU) of `order`.  If the key is already at
    /// the front, this is a no-op.
    fn move_to_front(order: &mut VecDeque<K>, key: &K) {
        if order.front().map_or(false, |f| f == key) {
            return; // already MRU — nothing to do
        }
        if let Some(pos) = order.iter().position(|k| k == key) {
            order.remove(pos);
        }
        order.push_front(key.clone());
    }
}

// ── tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::LruCache;

    // ── construction ─────────────────────────────────────────────────────────

    #[test]
    fn new_cache_is_empty() {
        let c: LruCache<i32, i32> = LruCache::new(4);
        assert!(c.is_empty());
        assert_eq!(c.len(), 0);
    }

    // ── get / peek ────────────────────────────────────────────────────────────

    #[test]
    fn get_missing_key_returns_none() {
        let mut c: LruCache<&str, i32> = LruCache::new(4);
        assert_eq!(c.get(&"absent"), None);
    }

    #[test]
    fn get_present_key_returns_value() {
        let mut c = LruCache::new(4);
        c.put(1u32, "hello");
        assert_eq!(c.get(&1), Some(&"hello"));
    }

    #[test]
    fn peek_does_not_affect_eviction_order() {
        let mut c = LruCache::new(2);
        c.put(1u32, "a");
        c.put(2u32, "b");
        // peek key 1 (currently LRU) — must not promote it
        assert_eq!(c.peek(&1), Some(&"a"));
        // inserting a third entry should evict 1, the true LRU
        c.put(3u32, "c");
        assert_eq!(c.peek(&1), None, "key 1 should have been evicted");
        assert_eq!(c.peek(&2), Some(&"b"));
        assert_eq!(c.peek(&3), Some(&"c"));
    }

    // ── eviction order ────────────────────────────────────────────────────────

    #[test]
    fn evicts_lru_on_overflow() {
        let mut c = LruCache::new(3);
        c.put(1i32, "one");
        c.put(2i32, "two");
        c.put(3i32, "three");
        // promote 1 so the LRU becomes 2
        c.get(&1);
        c.put(4i32, "four");
        assert_eq!(c.get(&2), None, "key 2 should be evicted");
        assert!(c.get(&1).is_some());
        assert!(c.get(&3).is_some());
        assert!(c.get(&4).is_some());
    }

    #[test]
    fn get_promotes_to_mru() {
        let mut c = LruCache::new(3);
        c.put("a", 1i32);
        c.put("b", 2i32);
        c.put("c", 3i32);
        // "a" is currently LRU; promote it
        c.get(&"a");
        // next evictee should now be "b"
        c.put("d", 4i32);
        assert_eq!(c.peek(&"b"), None, "b should be evicted");
        assert!(c.peek(&"a").is_some());
    }

    #[test]
    fn insertion_order_drives_eviction_without_access() {
        let mut c = LruCache::new(2);
        c.put(10u32, 'x');
        c.put(20u32, 'y');
        // 10 is LRU; evict it on the third insert
        c.put(30u32, 'z');
        assert_eq!(c.peek(&10), None);
        assert_eq!(c.peek(&20), Some(&'y'));
        assert_eq!(c.peek(&30), Some(&'z'));
    }

    // ── overwrite ─────────────────────────────────────────────────────────────

    #[test]
    fn overwrite_updates_value_and_promotes_key() {
        let mut c = LruCache::new(3);
        c.put(1i32, "first");
        c.put(2i32, "second");
        c.put(3i32, "third");
        // overwrite key 1 (currently LRU) → becomes MRU
        c.put(1i32, "updated");
        assert_eq!(c.get(&1), Some(&"updated"));
        // inserting a fourth entry should evict 2, now the LRU
        c.put(4i32, "fourth");
        assert_eq!(c.peek(&2), None, "key 2 should be evicted");
        assert!(c.peek(&1).is_some());
    }

    #[test]
    fn overwrite_does_not_grow_cache() {
        let mut c = LruCache::new(2);
        c.put("k", 1i32);
        c.put("k", 2i32);
        assert_eq!(c.len(), 1);
        assert_eq!(c.get(&"k"), Some(&2));
    }

    // ── remove ────────────────────────────────────────────────────────────────

    #[test]
    fn remove_existing_key_returns_value() {
        let mut c = LruCache::new(4);
        c.put(5i32, "five");
        assert_eq!(c.remove(&5), Some("five"));
        assert_eq!(c.len(), 0);
        assert_eq!(c.peek(&5), None);
    }

    #[test]
    fn remove_absent_key_returns_none() {
        let mut c: LruCache<i32, &str> = LruCache::new(4);
        assert_eq!(c.remove(&99), None);
    }

    #[test]
    fn remove_middle_entry_keeps_rest_and_eviction_still_works() {
        let mut c = LruCache::new(4);
        c.put(1u32, 'a');
        c.put(2u32, 'b');
        c.put(3u32, 'c');
        c.remove(&2);
        assert_eq!(c.len(), 2);
        // fill up and confirm eviction still fires correctly
        c.put(4u32, 'd');
        c.put(5u32, 'e'); // evicts 1 (LRU after removes)
        assert_eq!(c.peek(&1), None);
        assert!(c.peek(&3).is_some());
    }

    // ── len / clear ───────────────────────────────────────────────────────────

    #[test]
    fn len_tracks_entries() {
        let mut c = LruCache::new(5);
        assert_eq!(c.len(), 0);
        c.put(1i32, 1i32);
        assert_eq!(c.len(), 1);
        c.put(2i32, 2i32);
        assert_eq!(c.len(), 2);
        c.remove(&1);
        assert_eq!(c.len(), 1);
    }

    #[test]
    fn clear_empties_cache() {
        let mut c = LruCache::new(4);
        c.put(1i32, "a");
        c.put(2i32, "b");
        c.clear();
        assert!(c.is_empty());
        assert_eq!(c.len(), 0);
        assert_eq!(c.peek(&1), None);
        assert_eq!(c.peek(&2), None);
    }

    #[test]
    fn cleared_cache_accepts_new_entries() {
        let mut c = LruCache::new(2);
        c.put(1i32, 100i32);
        c.put(2i32, 200i32);
        c.clear();
        c.put(3i32, 300i32);
        assert_eq!(c.len(), 1);
        assert_eq!(c.get(&3), Some(&300));
    }

    // ── zero-capacity ─────────────────────────────────────────────────────────

    #[test]
    fn zero_capacity_never_stores() {
        let mut c: LruCache<i32, i32> = LruCache::new(0);
        c.put(1, 42);
        assert!(c.is_empty());
        assert_eq!(c.get(&1), None);
        assert_eq!(c.peek(&1), None);
    }

    #[test]
    fn zero_capacity_remove_returns_none() {
        let mut c: LruCache<i32, i32> = LruCache::new(0);
        assert_eq!(c.remove(&1), None);
    }

    #[test]
    fn zero_capacity_len_stays_zero() {
        let mut c: LruCache<&str, u64> = LruCache::new(0);
        c.put("x", 1);
        c.put("y", 2);
        assert_eq!(c.len(), 0);
        assert!(c.is_empty());
    }

    // ── capacity boundary ─────────────────────────────────────────────────────

    #[test]
    fn len_never_exceeds_capacity() {
        let cap = 4usize;
        let mut c = LruCache::new(cap);
        for i in 0..20u32 {
            c.put(i, i * 2);
        }
        assert_eq!(c.len(), cap);
    }

    #[test]
    fn single_capacity_always_holds_last_put() {
        let mut c = LruCache::new(1);
        c.put(1i32, "first");
        c.put(2i32, "second");
        assert_eq!(c.peek(&1), None);
        assert_eq!(c.peek(&2), Some(&"second"));
    }
}
