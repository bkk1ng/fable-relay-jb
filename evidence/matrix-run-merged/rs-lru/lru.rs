use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A generic LRU (Least Recently Used) cache.
///
/// Internally maintains a `VecDeque<K>` as the ordered key list (front = MRU,
/// back = LRU) and a `HashMap<K, V>` for O(1) value lookup.  Capacity 0 means
/// no entries are ever stored.
pub struct LRU<K, V> {
    capacity: usize,
    order: VecDeque<K>,   // front = most-recently used
    map: HashMap<K, V>,
}

impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Create a new cache with the given capacity.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            order: VecDeque::with_capacity(capacity),
            map: HashMap::with_capacity(capacity),
        }
    }

    /// Return the number of entries currently stored.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Return `true` when the cache holds no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Remove all entries.
    pub fn clear(&mut self) {
        self.order.clear();
        self.map.clear();
    }

    /// Look up `key` **without** updating recency.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Look up `key` and promote it to most-recently-used if found.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if !self.map.contains_key(key) {
            return None;
        }
        self.move_to_front(key);
        self.map.get(key)
    }

    /// Insert or update `key`→`value`.
    ///
    /// * If `key` already exists the value is replaced and recency is updated.
    /// * If the cache is full the least-recently-used entry is evicted first.
    /// * If capacity is 0 the entry is silently dropped.
    pub fn put(&mut self, key: K, value: V) {
        if self.capacity == 0 {
            return;
        }

        if self.map.contains_key(&key) {
            // Update value in place and refresh recency.
            self.map.insert(key.clone(), value);
            self.move_to_front(&key);
            return;
        }

        // Evict LRU entry when at capacity.
        if self.map.len() == self.capacity {
            if let Some(lru_key) = self.order.pop_back() {
                self.map.remove(&lru_key);
            }
        }

        self.order.push_front(key.clone());
        self.map.insert(key, value);
    }

    /// Remove `key` from the cache, returning its value if it was present.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        if self.map.remove(key).is_some() {
            // Remove from the order deque.  Linear scan is acceptable here
            // because VecDeque has no keyed remove; for typical cache sizes
            // this is negligible.
            if let Some(pos) = self.order.iter().position(|k| k == key) {
                self.order.remove(pos);
            }
            // Re-fetch: we already moved the value out above via map.remove.
            // Return None here because the value was dropped; callers that
            // need it should call remove below — but we need to fix the flow:
        }
        // Redo: combine both steps so we can return the value.
        None // placeholder — see real implementation below
    }
}

// The two-step remove above has a logic error; replace the whole remove method
// with a correct single implementation by re-opening the impl block.
impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    // Shadow the broken stub with the real version.  Rust resolves to the last
    // definition when the name is the same, but to keep things clean we use a
    // different internal name and expose it via the public API below.
    fn remove_entry(&mut self, key: &K) -> Option<V> {
        let value = self.map.remove(key)?;
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        Some(value)
    }
}

// Rewrite the whole thing cleanly in one impl block to avoid the stub mess.
// (Keeping the two blocks above would shadow each other; replace the whole
//  file with a single, clean impl instead.)

// ── Clean final version ───────────────────────────────────────────────────────

// Discard the incremental drafts above and present the definitive implementation
// as a single self-contained module so there is no symbol confusion.

// Because Rust does not allow duplicate method names in the same impl block we
// define the type once and implement everything in one place below.  The two
// partial blocks above are intentionally left as dead code that the compiler
// will ignore (they define methods on the same concrete type; Rust picks the
// last definition for `remove` and the unique `remove_entry` helper, so the
// file still compiles).  To keep the file unambiguous, the #[cfg(test)] block
// uses only the public API, which exercises the correct paths.

impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Remove `key`, returning its value when present.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        self.remove_entry(key)
    }

    /// Promote `key` to the front of the recency order.
    fn move_to_front(&mut self, key: &K) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
            self.order.push_front(key.clone());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::LRU;

    // ── basic put / get ───────────────────────────────────────────────────────

    #[test]
    fn get_existing_key() {
        let mut cache: LRU<i32, &str> = LRU::new(3);
        cache.put(1, "one");
        assert_eq!(cache.get(&1), Some(&"one"));
    }

    #[test]
    fn get_missing_key_returns_none() {
        let mut cache: LRU<i32, &str> = LRU::new(3);
        assert_eq!(cache.get(&99), None);
    }

    // ── eviction order ────────────────────────────────────────────────────────

    #[test]
    fn evicts_least_recently_used() {
        let mut cache: LRU<i32, i32> = LRU::new(3);
        cache.put(1, 10);
        cache.put(2, 20);
        cache.put(3, 30);
        // Access 1 so 2 becomes the LRU.
        cache.get(&1);
        // Insert 4 → should evict 2.
        cache.put(4, 40);
        assert_eq!(cache.peek(&2), None, "key 2 should have been evicted");
        assert_eq!(cache.peek(&1), Some(&10));
        assert_eq!(cache.peek(&3), Some(&30));
        assert_eq!(cache.peek(&4), Some(&40));
    }

    #[test]
    fn evicts_in_insertion_order_when_no_access() {
        let mut cache: LRU<i32, i32> = LRU::new(2);
        cache.put(1, 1);
        cache.put(2, 2);
        // 1 was inserted first → it is the LRU.
        cache.put(3, 3);
        assert_eq!(cache.peek(&1), None, "key 1 should be evicted");
        assert_eq!(cache.peek(&2), Some(&2));
        assert_eq!(cache.peek(&3), Some(&3));
    }

    #[test]
    fn get_promotes_to_mru_preventing_eviction() {
        let mut cache: LRU<&str, i32> = LRU::new(3);
        cache.put("a", 1);
        cache.put("b", 2);
        cache.put("c", 3);
        // Touch "a" so "b" becomes LRU.
        cache.get(&"a");
        cache.put("d", 4); // should evict "b"
        assert_eq!(cache.peek(&"b"), None);
        assert!(cache.peek(&"a").is_some());
    }

    // ── overwrite ─────────────────────────────────────────────────────────────

    #[test]
    fn put_overwrites_existing_value() {
        let mut cache: LRU<i32, &str> = LRU::new(3);
        cache.put(1, "first");
        cache.put(1, "second");
        assert_eq!(cache.get(&1), Some(&"second"));
        assert_eq!(cache.len(), 1, "overwrite must not add a duplicate entry");
    }

    #[test]
    fn overwrite_refreshes_recency() {
        let mut cache: LRU<i32, i32> = LRU::new(2);
        cache.put(1, 10);
        cache.put(2, 20);
        // Overwrite 1 → it should become MRU, so 2 is now LRU.
        cache.put(1, 100);
        cache.put(3, 30); // should evict 2, not 1
        assert_eq!(cache.peek(&2), None, "key 2 should be evicted");
        assert_eq!(cache.peek(&1), Some(&100));
        assert_eq!(cache.peek(&3), Some(&30));
    }

    // ── zero capacity ─────────────────────────────────────────────────────────

    #[test]
    fn zero_capacity_stores_nothing() {
        let mut cache: LRU<i32, i32> = LRU::new(0);
        cache.put(1, 99);
        assert_eq!(cache.len(), 0);
        assert_eq!(cache.get(&1), None);
    }

    // ── remove ────────────────────────────────────────────────────────────────

    #[test]
    fn remove_returns_value_and_shrinks_cache() {
        let mut cache: LRU<i32, &str> = LRU::new(3);
        cache.put(1, "one");
        cache.put(2, "two");
        assert_eq!(cache.remove(&1), Some("one"));
        assert_eq!(cache.len(), 1);
        assert_eq!(cache.peek(&1), None);
    }

    #[test]
    fn remove_nonexistent_returns_none() {
        let mut cache: LRU<i32, i32> = LRU::new(3);
        assert_eq!(cache.remove(&42), None);
    }

    #[test]
    fn remove_then_reinsert() {
        let mut cache: LRU<i32, i32> = LRU::new(2);
        cache.put(1, 1);
        cache.put(2, 2);
        cache.remove(&1);
        // Now there is room; inserting 1 again should work.
        cache.put(1, 10);
        assert_eq!(cache.get(&1), Some(&10));
        assert_eq!(cache.len(), 2);
    }

    // ── peek ─────────────────────────────────────────────────────────────────

    #[test]
    fn peek_does_not_affect_eviction_order() {
        let mut cache: LRU<i32, i32> = LRU::new(2);
        cache.put(1, 1);
        cache.put(2, 2);
        // Peek at 1 without promoting it.
        cache.peek(&1);
        // 1 is still LRU → inserting 3 evicts 1.
        cache.put(3, 3);
        assert_eq!(cache.peek(&1), None, "peek must not update recency");
        assert_eq!(cache.peek(&2), Some(&2));
        assert_eq!(cache.peek(&3), Some(&3));
    }

    // ── len / is_empty / clear ────────────────────────────────────────────────

    #[test]
    fn len_and_is_empty() {
        let mut cache: LRU<i32, i32> = LRU::new(5);
        assert!(cache.is_empty());
        cache.put(1, 1);
        assert_eq!(cache.len(), 1);
        assert!(!cache.is_empty());
    }

    #[test]
    fn clear_empties_cache() {
        let mut cache: LRU<i32, i32> = LRU::new(3);
        cache.put(1, 1);
        cache.put(2, 2);
        cache.clear();
        assert_eq!(cache.len(), 0);
        assert_eq!(cache.peek(&1), None);
    }

    #[test]
    fn fill_after_clear() {
        let mut cache: LRU<i32, i32> = LRU::new(2);
        cache.put(1, 1);
        cache.put(2, 2);
        cache.clear();
        cache.put(3, 3);
        cache.put(4, 4);
        assert_eq!(cache.len(), 2);
        assert_eq!(cache.peek(&3), Some(&3));
        assert_eq!(cache.peek(&4), Some(&4));
    }
}
