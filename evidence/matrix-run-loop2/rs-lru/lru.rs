use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A generic LRU cache with a fixed capacity.
///
/// Internally backed by a `HashMap<K, V>` for O(1) lookup and a `VecDeque<K>`
/// that tracks insertion/access order: index 0 is the least-recently-used key,
/// the back is the most-recently-used.
///
/// # Type parameters
/// - `K`: key type — must be `Eq + Hash + Clone`
/// - `V`: value type — no extra bounds required
pub struct LRU<K, V> {
    capacity: usize,
    map: HashMap<K, V>,
    order: VecDeque<K>, // front = LRU, back = MRU
}

impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Creates a new cache with the given capacity.
    ///
    /// A capacity of 0 is valid; every `put` immediately discards the entry.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            map: HashMap::new(),
            order: VecDeque::new(),
        }
    }

    /// Returns the maximum number of entries the cache can hold.
    pub fn capacity(&self) -> usize {
        self.capacity
    }

    /// Returns the current number of entries in the cache.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns `true` if the cache contains no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Looks up `key` and returns a reference to its value, **updating** its
    /// recency so it becomes the most-recently-used entry.
    ///
    /// Returns `None` if the key is not present.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if self.map.contains_key(key) {
            self.move_to_back(key);
            self.map.get(key)
        } else {
            None
        }
    }

    /// Looks up `key` and returns a reference to its value **without** updating
    /// recency.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Inserts or updates `key` → `value`.
    ///
    /// - If the key already exists its value is updated and it becomes the
    ///   most-recently-used entry.
    /// - If the cache is at capacity the least-recently-used entry is evicted
    ///   first.
    /// - If capacity is 0 the entry is never stored.
    pub fn put(&mut self, key: K, value: V) {
        if self.capacity == 0 {
            return;
        }

        if self.map.contains_key(&key) {
            // Update value in place and promote to MRU.
            self.map.insert(key.clone(), value);
            self.move_to_back(&key);
            return;
        }

        // Evict LRU if we are at capacity.
        if self.map.len() == self.capacity {
            if let Some(lru_key) = self.order.pop_front() {
                self.map.remove(&lru_key);
            }
        }

        self.order.push_back(key.clone());
        self.map.insert(key, value);
    }

    /// Removes `key` from the cache and returns its value, or `None` if absent.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
            self.map.remove(key)
        } else {
            None
        }
    }

    /// Removes all entries from the cache.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    // ── internal helpers ────────────────────────────────────────────────────

    /// Moves `key` to the back (MRU position) of the order deque.
    fn move_to_back(&mut self, key: &K) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
            self.order.push_back(key.clone());
        }
    }
}

// ── tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::LRU;

    // Helper: collect the internal order vec (LRU → MRU) for assertions.
    // We test observable behaviour through the public API rather than peeking
    // at internals wherever possible, but eviction order needs some ordering
    // witness.
    fn drain_order<K: Eq + std::hash::Hash + Clone, V: Clone>(
        cache: &mut LRU<K, V>,
        keys: &[K],
    ) -> Vec<Option<V>> {
        keys.iter().map(|k| cache.peek(k).cloned()).collect()
    }

    // ── basic put / get ───────────────────────────────────────────────────────

    #[test]
    fn get_existing_key() {
        let mut c: LRU<&str, i32> = LRU::new(3);
        c.put("a", 1);
        assert_eq!(c.get(&"a"), Some(&1));
    }

    #[test]
    fn get_missing_key_returns_none() {
        let mut c: LRU<&str, i32> = LRU::new(3);
        assert_eq!(c.get(&"z"), None);
    }

    // ── eviction order ────────────────────────────────────────────────────────

    #[test]
    fn evicts_least_recently_used() {
        let mut c: LRU<&str, i32> = LRU::new(2);
        c.put("a", 1); // order: [a]
        c.put("b", 2); // order: [a, b]
        // Access "a" so "b" becomes the LRU.
        c.get(&"a");   // order: [b, a]
        c.put("c", 3); // "b" evicted → order: [a, c]

        assert_eq!(c.peek(&"a"), Some(&1));
        assert_eq!(c.peek(&"b"), None, "b should have been evicted");
        assert_eq!(c.peek(&"c"), Some(&3));
    }

    #[test]
    fn evicts_in_fifo_order_when_no_access() {
        let mut c: LRU<u32, u32> = LRU::new(3);
        c.put(1, 10);
        c.put(2, 20);
        c.put(3, 30);
        // No accesses — insertion order determines LRU.
        c.put(4, 40); // 1 evicted
        c.put(5, 50); // 2 evicted

        assert_eq!(c.peek(&1), None);
        assert_eq!(c.peek(&2), None);
        assert_eq!(c.peek(&3), Some(&30));
        assert_eq!(c.peek(&4), Some(&40));
        assert_eq!(c.peek(&5), Some(&50));
    }

    #[test]
    fn get_promotes_key_preventing_its_eviction() {
        let mut c: LRU<u32, u32> = LRU::new(3);
        c.put(1, 1);
        c.put(2, 2);
        c.put(3, 3);
        // Access key 1 so it moves to MRU; LRU becomes 2.
        c.get(&1);
        c.put(4, 4); // should evict 2
        assert_eq!(c.peek(&1), Some(&1), "1 should survive");
        assert_eq!(c.peek(&2), None, "2 should be evicted");
        assert_eq!(c.peek(&3), Some(&3));
        assert_eq!(c.peek(&4), Some(&4));
    }

    // ── overwrite ────────────────────────────────────────────────────────────

    #[test]
    fn overwrite_updates_value() {
        let mut c: LRU<&str, i32> = LRU::new(3);
        c.put("x", 1);
        c.put("x", 99);
        assert_eq!(c.get(&"x"), Some(&99));
        assert_eq!(c.len(), 1, "overwrite must not create a duplicate entry");
    }

    #[test]
    fn overwrite_promotes_to_mru() {
        // If we overwrite the LRU key it should become MRU and not be the
        // next one evicted.
        let mut c: LRU<u32, u32> = LRU::new(2);
        c.put(1, 10); // LRU
        c.put(2, 20); // MRU
        c.put(1, 11); // overwrite LRU → it becomes MRU
        c.put(3, 30); // should evict 2 now
        assert_eq!(c.peek(&1), Some(&11), "1 should survive as it was overwritten last");
        assert_eq!(c.peek(&2), None, "2 should be evicted");
        assert_eq!(c.peek(&3), Some(&30));
    }

    // ── zero capacity ─────────────────────────────────────────────────────────

    #[test]
    fn zero_capacity_stores_nothing() {
        let mut c: LRU<&str, i32> = LRU::new(0);
        c.put("a", 1);
        c.put("b", 2);
        assert_eq!(c.len(), 0);
        assert_eq!(c.get(&"a"), None);
    }

    // ── remove ────────────────────────────────────────────────────────────────

    #[test]
    fn remove_existing_key() {
        let mut c: LRU<&str, i32> = LRU::new(3);
        c.put("a", 1);
        c.put("b", 2);
        assert_eq!(c.remove(&"a"), Some(1));
        assert_eq!(c.len(), 1);
        assert_eq!(c.peek(&"a"), None);
    }

    #[test]
    fn remove_missing_key_returns_none() {
        let mut c: LRU<&str, i32> = LRU::new(3);
        assert_eq!(c.remove(&"z"), None);
    }

    #[test]
    fn remove_then_put_respects_capacity() {
        let mut c: LRU<u32, u32> = LRU::new(2);
        c.put(1, 1);
        c.put(2, 2);
        c.remove(&1); // frees a slot
        c.put(3, 3);  // should not evict 2
        assert_eq!(c.peek(&2), Some(&2));
        assert_eq!(c.peek(&3), Some(&3));
        assert_eq!(c.len(), 2);
    }

    // ── peek ──────────────────────────────────────────────────────────────────

    #[test]
    fn peek_does_not_affect_eviction_order() {
        let mut c: LRU<u32, u32> = LRU::new(2);
        c.put(1, 1); // LRU
        c.put(2, 2); // MRU
        // Peek at 1 — should NOT promote it.
        c.peek(&1);
        c.put(3, 3); // 1 should still be evicted
        assert_eq!(c.peek(&1), None, "peek must not update recency");
        assert_eq!(c.peek(&2), Some(&2));
        assert_eq!(c.peek(&3), Some(&3));
    }

    // ── len / clear ───────────────────────────────────────────────────────────

    #[test]
    fn len_tracks_entries() {
        let mut c: LRU<u32, u32> = LRU::new(5);
        assert_eq!(c.len(), 0);
        c.put(1, 1);
        assert_eq!(c.len(), 1);
        c.put(2, 2);
        assert_eq!(c.len(), 2);
        c.remove(&1);
        assert_eq!(c.len(), 1);
    }

    #[test]
    fn clear_empties_the_cache() {
        let mut c: LRU<u32, u32> = LRU::new(3);
        c.put(1, 1);
        c.put(2, 2);
        c.clear();
        assert_eq!(c.len(), 0);
        assert!(c.is_empty());
        // Capacity should still work after clear.
        c.put(3, 3);
        assert_eq!(c.len(), 1);
    }

    // ── capacity accessor ─────────────────────────────────────────────────────

    #[test]
    fn capacity_accessor() {
        let c: LRU<u32, u32> = LRU::new(7);
        assert_eq!(c.capacity(), 7);
    }

    // ── single-slot edge case ─────────────────────────────────────────────────

    #[test]
    fn capacity_one_always_evicts_on_new_key() {
        let mut c: LRU<u32, u32> = LRU::new(1);
        c.put(1, 10);
        c.put(2, 20);
        assert_eq!(c.peek(&1), None);
        assert_eq!(c.peek(&2), Some(&20));
        assert_eq!(c.len(), 1);
    }

    // ── compile-time check: values only need no extra bounds ──────────────────

    #[test]
    fn works_with_non_copy_values() {
        let mut c: LRU<u32, String> = LRU::new(2);
        c.put(1, "hello".to_string());
        c.put(2, "world".to_string());
        assert_eq!(c.get(&1).map(String::as_str), Some("hello"));
        c.put(3, "!".to_string()); // evicts 2
        assert_eq!(c.peek(&2), None);
    }

    // ── drain helper self-test (meta) ─────────────────────────────────────────

    #[test]
    fn drain_order_helper_works() {
        let mut c: LRU<u32, u32> = LRU::new(3);
        c.put(10, 100);
        c.put(20, 200);
        let got = drain_order(&mut c, &[10, 20, 30]);
        assert_eq!(got, vec![Some(100), Some(200), None]);
    }
}
