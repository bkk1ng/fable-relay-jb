use std::collections::HashMap;
use std::collections::VecDeque;
use std::hash::Hash;

/// A generic LRU cache with a fixed capacity.
///
/// Recency order is tracked by a `VecDeque<K>` (front = MRU, back = LRU)
/// paired with a `HashMap<K, V>` for O(1) value look-up.
/// Eviction on `put` removes the entry at the back of the deque.
///
/// # Type parameters
/// - `K` – key type; must implement `Eq + Hash + Clone`
/// - `V` – value type; unconstrained
pub struct Lru<K, V> {
    capacity: usize,
    order: VecDeque<K>, // front = MRU, back = LRU
    map: HashMap<K, V>,
}

impl<K, V> Lru<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Creates a new cache. A capacity of `0` is valid; every `put` is a
    /// no-op and the cache stays permanently empty.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            order: VecDeque::new(),
            map: HashMap::new(),
        }
    }

    /// Returns the current number of entries.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns `true` when the cache holds no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Looks up a value and moves its key to the MRU position.
    /// Returns `None` when the key is absent.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if self.map.contains_key(key) {
            Self::touch(&mut self.order, key);
            self.map.get(key)
        } else {
            None
        }
    }

    /// Looks up a value *without* updating recency order.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Inserts or overwrites a key-value pair.
    ///
    /// - If the key already exists the value is updated and the key is moved
    ///   to the MRU position; no eviction occurs.
    /// - If the key is new and the cache is at capacity, the LRU entry is
    ///   evicted before insertion.
    /// - When capacity is `0` nothing is stored.
    pub fn put(&mut self, key: K, value: V) {
        if self.map.contains_key(&key) {
            self.map.insert(key.clone(), value);
            Self::touch(&mut self.order, &key);
            return;
        }

        if self.capacity == 0 {
            return;
        }

        if self.map.len() == self.capacity {
            if let Some(lru_key) = self.order.pop_back() {
                self.map.remove(&lru_key);
            }
        }

        self.order.push_front(key.clone());
        self.map.insert(key, value);
    }

    /// Removes a key-value pair and returns its value, or `None` when the
    /// key is absent.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        let value = self.map.remove(key)?;
        self.order.retain(|k| k != key);
        Some(value)
    }

    /// Removes all entries, leaving capacity unchanged.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    // ── private helpers ──────────────────────────────────────────────────

    /// Moves `key` to the front of `order` (MRU position).
    /// No-op when the key is already at the front.
    fn touch(order: &mut VecDeque<K>, key: &K) {
        if order.front().map_or(false, |f| f == key) {
            return;
        }
        if let Some(pos) = order.iter().position(|k| k == key) {
            order.remove(pos);
        }
        order.push_front(key.clone());
    }
}

// ─────────────────────────────────────────────────────────────────────────
#[cfg(test)]
mod tests {
    use super::Lru;

    // ── basic put / get ───────────────────────────────────────────────────

    #[test]
    fn get_returns_inserted_value() {
        let mut c: Lru<&str, i32> = Lru::new(4);
        c.put("a", 1);
        assert_eq!(c.get(&"a"), Some(&1));
    }

    #[test]
    fn get_missing_key_returns_none() {
        let mut c: Lru<&str, i32> = Lru::new(4);
        assert_eq!(c.get(&"x"), None);
    }

    // ── eviction order ───────────────────────────────────────────────────

    /// Fill a capacity-3 cache, then add a 4th entry.
    /// The first inserted key ("a") must be evicted.
    #[test]
    fn evicts_lru_on_overflow() {
        let mut c = Lru::new(3);
        c.put("a", 1);
        c.put("b", 2);
        c.put("c", 3);
        c.put("d", 4); // "a" is evicted
        assert_eq!(c.peek(&"a"), None);
        assert_eq!(c.peek(&"b"), Some(&2));
        assert_eq!(c.peek(&"c"), Some(&3));
        assert_eq!(c.peek(&"d"), Some(&4));
    }

    /// A `get` refreshes recency, so the previously-LRU entry survives the
    /// next eviction and the now-LRU entry is evicted instead.
    #[test]
    fn get_refreshes_recency() {
        let mut c = Lru::new(3);
        c.put("a", 1);
        c.put("b", 2);
        c.put("c", 3);
        // Touch "a" — it is no longer LRU; "b" becomes LRU.
        c.get(&"a");
        c.put("d", 4); // should evict "b", not "a"
        assert_eq!(c.peek(&"a"), Some(&1));
        assert_eq!(c.peek(&"b"), None);
        assert_eq!(c.peek(&"c"), Some(&3));
        assert_eq!(c.peek(&"d"), Some(&4));
    }

    /// Eviction order across multiple overflows.
    #[test]
    fn eviction_order_multiple_overflows() {
        let mut c: Lru<u32, char> = Lru::new(2);
        c.put(1, 'a');
        c.put(2, 'b');
        c.put(3, 'c'); // evicts 1
        assert_eq!(c.peek(&1), None);
        c.put(4, 'd'); // evicts 2
        assert_eq!(c.peek(&2), None);
        assert_eq!(c.peek(&3), Some(&'c'));
        assert_eq!(c.peek(&4), Some(&'d'));
    }

    // ── overwrite ────────────────────────────────────────────────────────

    /// Overwriting an existing key updates the value, keeps `len` stable,
    /// and moves the key to MRU so it is not the next eviction target.
    #[test]
    fn overwrite_updates_value_and_refreshes() {
        let mut c = Lru::new(3);
        c.put("a", 1);
        c.put("b", 2);
        c.put("c", 3);
        // Overwrite "a" — it should become MRU.
        c.put("a", 99);
        assert_eq!(c.len(), 3);
        assert_eq!(c.peek(&"a"), Some(&99));
        // "b" is now the LRU; inserting "d" should evict it.
        c.put("d", 4);
        assert_eq!(c.peek(&"b"), None);
        assert_eq!(c.peek(&"a"), Some(&99));
        assert_eq!(c.peek(&"c"), Some(&3));
        assert_eq!(c.peek(&"d"), Some(&4));
    }

    // ── zero-capacity ────────────────────────────────────────────────────

    #[test]
    fn zero_capacity_stores_nothing() {
        let mut c: Lru<u32, u32> = Lru::new(0);
        c.put(1, 100);
        c.put(2, 200);
        assert_eq!(c.len(), 0);
        assert_eq!(c.get(&1), None);
    }

    // ── remove ───────────────────────────────────────────────────────────

    #[test]
    fn remove_returns_value_and_decrements_len() {
        let mut c = Lru::new(4);
        c.put("x", 42);
        assert_eq!(c.remove(&"x"), Some(42));
        assert_eq!(c.len(), 0);
        assert_eq!(c.peek(&"x"), None);
    }

    #[test]
    fn remove_absent_key_returns_none() {
        let mut c: Lru<&str, i32> = Lru::new(4);
        assert_eq!(c.remove(&"missing"), None);
    }

    /// After removing the current LRU entry, the next eviction must pick
    /// the new LRU correctly.
    #[test]
    fn eviction_after_remove_uses_correct_lru() {
        let mut c = Lru::new(2);
        c.put("a", 1);
        c.put("b", 2);
        c.remove(&"a"); // remove the current LRU
        c.put("c", 3);  // should evict "b"
        assert_eq!(c.peek(&"b"), None);
        assert_eq!(c.peek(&"c"), Some(&3));
    }

    // ── clear ────────────────────────────────────────────────────────────

    #[test]
    fn clear_empties_cache_and_allows_reuse() {
        let mut c = Lru::new(3);
        c.put("a", 1);
        c.put("b", 2);
        c.clear();
        assert!(c.is_empty());
        assert_eq!(c.len(), 0);
        // Capacity must still work after clear.
        c.put("c", 3);
        c.put("d", 4);
        c.put("e", 5);
        c.put("f", 6); // evicts "c"
        assert_eq!(c.peek(&"c"), None);
        assert_eq!(c.len(), 3);
    }

    // ── peek ─────────────────────────────────────────────────────────────

    /// peek must not alter eviction order: the entry peeked at should still
    /// be LRU and get evicted on the next overflow.
    #[test]
    fn peek_does_not_affect_eviction_order() {
        let mut c = Lru::new(2);
        c.put("a", 1);
        c.put("b", 2);
        // Peek at the LRU entry without promoting it.
        assert_eq!(c.peek(&"a"), Some(&1));
        c.put("c", 3); // "a" is still LRU and must be evicted
        assert_eq!(c.peek(&"a"), None);
        assert_eq!(c.peek(&"b"), Some(&2));
        assert_eq!(c.peek(&"c"), Some(&3));
    }

    // ── capacity-1 edge case ─────────────────────────────────────────────

    #[test]
    fn capacity_one_always_holds_latest() {
        let mut c = Lru::new(1);
        c.put("a", 1);
        c.put("b", 2);
        assert_eq!(c.peek(&"a"), None);
        assert_eq!(c.get(&"b"), Some(&2));
        assert_eq!(c.len(), 1);
    }
}
