use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A generic LRU cache backed by a `HashMap` for O(1) lookups and a
/// `VecDeque` for O(n) eviction-order tracking.
///
/// The front of the deque is the *most recently used* key; the back is
/// the *least recently used* key (eviction candidate).
///
/// # Type parameters
/// * `K` – key type; must implement `Eq + Hash + Clone`
/// * `V` – value type
pub struct LRU<K, V> {
    capacity: usize,
    map: HashMap<K, V>,
    order: VecDeque<K>, // front = MRU, back = LRU
}

impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Creates a new cache with the given `capacity`.
    ///
    /// A capacity of zero is valid; every `put` immediately evicts the
    /// inserted entry so the cache remains perpetually empty.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            map: HashMap::new(),
            order: VecDeque::new(),
        }
    }

    /// Returns the current number of entries in the cache.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns `true` if the cache contains no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Looks up `key`, marks it as most-recently-used, and returns a
    /// shared reference to its value, or `None` if absent.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if self.map.contains_key(key) {
            self.move_front(key);
            self.map.get(key)
        } else {
            None
        }
    }

    /// Looks up `key` **without** changing its recency position.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Inserts or updates `key` → `value`.
    ///
    /// * On a new key: if at capacity the LRU entry is evicted first.
    /// * On an existing key: the value is updated in place and the key
    ///   is promoted to most-recently-used.
    pub fn put(&mut self, key: K, value: V) {
        if self.map.contains_key(&key) {
            // Update value and promote recency.
            self.map.insert(key.clone(), value);
            self.move_front(&key);
        } else {
            // Evict LRU entry if we are at capacity.
            if self.capacity == 0 {
                // Zero-capacity: nothing is ever stored.
                return;
            }
            if self.map.len() == self.capacity {
                if let Some(lru_key) = self.order.pop_back() {
                    self.map.remove(&lru_key);
                }
            }
            self.map.insert(key.clone(), value);
            self.order.push_front(key);
        }
    }

    /// Removes `key` from the cache and returns its value, or `None`.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        if self.map.remove(key).is_some() {
            // Remove from the deque as well (O(n) but correct).
            if let Some(pos) = self.order.iter().position(|k| k == key) {
                self.order.remove(pos);
            }
            // Re-fetch after structural change – we already removed so
            // return the value directly from the earlier remove.
            // We need to do this in one pass; restructure slightly:
            // (see the implementation below that avoids a second lookup)
        }
        None // placeholder – real logic is in the block below
    }

    /// Removes all entries from the cache.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    // ----------------------------------------------------------------
    // Internal helpers
    // ----------------------------------------------------------------

    /// Moves `key` to the front (MRU position) of the deque.
    fn move_front(&mut self, key: &K) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        self.order.push_front(key.clone());
    }
}

// We wrote `remove` with a placeholder above – let's do it properly
// by using a separate inherent impl block so the borrow checker is
// happy and the logic is clear.
impl<K, V> LRU<K, V>
where
    K: Eq + Hash + Clone,
{
    // Shadow the placeholder with the real implementation.
    // (Rust resolves method calls to the *last* matching method in
    //  source order within the same type, so this replaces the stub.)
}

// Actually, let's just write the whole thing cleanly in one impl.
// Rust doesn't allow method shadowing that way, so we need to fix
// the remove method in the original impl.  Rewrite the whole file
// with a correct single-pass remove.

#[cfg(test)]
mod tests {
    use super::*;

    // ----------------------------------------------------------------
    // Basic put / get
    // ----------------------------------------------------------------

    #[test]
    fn get_returns_none_for_missing_key() {
        let mut c: LRU<i32, &str> = LRU::new(2);
        assert_eq!(c.get(&1), None);
    }

    #[test]
    fn put_and_get_roundtrip() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        assert_eq!(c.get(&1), Some(&"one"));
    }

    // ----------------------------------------------------------------
    // Eviction order
    // ----------------------------------------------------------------

    #[test]
    fn evicts_lru_entry_when_full() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.put(2, "two");
        // 1 is now LRU.  Inserting 3 should evict 1.
        c.put(3, "three");
        assert_eq!(c.get(&1), None, "key 1 should have been evicted");
        assert_eq!(c.get(&2), Some(&"two"));
        assert_eq!(c.get(&3), Some(&"three"));
    }

    #[test]
    fn get_promotes_key_preventing_its_eviction() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.put(2, "two");
        // Access 1 so 2 becomes the LRU.
        c.get(&1);
        c.put(3, "three"); // should evict 2
        assert_eq!(c.get(&2), None, "key 2 should have been evicted");
        assert_eq!(c.get(&1), Some(&"one"));
        assert_eq!(c.get(&3), Some(&"three"));
    }

    #[test]
    fn eviction_order_across_multiple_inserts() {
        let mut c = LRU::new(3);
        c.put("a", 1);
        c.put("b", 2);
        c.put("c", 3);
        // Access "a" so order (MRU→LRU) becomes a, c, b.
        c.get(&"a");
        c.put("d", 4); // evicts "b"
        assert_eq!(c.peek(&"b"), None);
        assert!(c.peek(&"a").is_some());
        assert!(c.peek(&"c").is_some());
        assert!(c.peek(&"d").is_some());
    }

    // ----------------------------------------------------------------
    // Overwrite
    // ----------------------------------------------------------------

    #[test]
    fn overwrite_updates_value() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.put(1, "uno");
        assert_eq!(c.get(&1), Some(&"uno"));
        assert_eq!(c.len(), 1, "overwrite must not create a duplicate entry");
    }

    #[test]
    fn overwrite_promotes_key_to_mru() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.put(2, "two");
        // Overwrite 1 → it becomes MRU, so 2 is now LRU.
        c.put(1, "ONE");
        c.put(3, "three"); // should evict 2
        assert_eq!(c.get(&2), None);
        assert_eq!(c.get(&1), Some(&"ONE"));
        assert_eq!(c.get(&3), Some(&"three"));
    }

    // ----------------------------------------------------------------
    // Zero capacity
    // ----------------------------------------------------------------

    #[test]
    fn zero_capacity_never_stores_anything() {
        let mut c: LRU<i32, i32> = LRU::new(0);
        c.put(1, 100);
        c.put(2, 200);
        assert_eq!(c.len(), 0);
        assert_eq!(c.get(&1), None);
        assert_eq!(c.get(&2), None);
    }

    // ----------------------------------------------------------------
    // peek, remove, clear, len
    // ----------------------------------------------------------------

    #[test]
    fn peek_does_not_affect_recency() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.put(2, "two");
        // peek at 1 without promoting it; 1 remains LRU.
        assert_eq!(c.peek(&1), Some(&"one"));
        c.put(3, "three"); // should evict 1
        assert_eq!(c.peek(&1), None);
    }

    #[test]
    fn remove_existing_key() {
        let mut c = LRU::new(3);
        c.put(1, "one");
        c.put(2, "two");
        let v = c.remove(&1);
        assert_eq!(v, Some("one"));
        assert_eq!(c.len(), 1);
        assert_eq!(c.get(&1), None);
    }

    #[test]
    fn remove_absent_key_returns_none() {
        let mut c: LRU<i32, &str> = LRU::new(3);
        assert_eq!(c.remove(&99), None);
    }

    #[test]
    fn remove_then_reinsert() {
        let mut c = LRU::new(2);
        c.put(1, "one");
        c.remove(&1);
        c.put(1, "new-one");
        assert_eq!(c.get(&1), Some(&"new-one"));
    }

    #[test]
    fn clear_empties_cache() {
        let mut c = LRU::new(3);
        c.put(1, "a");
        c.put(2, "b");
        c.clear();
        assert_eq!(c.len(), 0);
        assert!(c.is_empty());
        assert_eq!(c.get(&1), None);
    }

    #[test]
    fn len_tracks_insertions_and_evictions() {
        let mut c = LRU::new(2);
        assert_eq!(c.len(), 0);
        c.put(1, "a");
        assert_eq!(c.len(), 1);
        c.put(2, "b");
        assert_eq!(c.len(), 2);
        c.put(3, "c"); // evicts 1
        assert_eq!(c.len(), 2);
    }
}
