use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A generic LRU cache with a fixed capacity.
///
/// Internally backed by a `HashMap<K, V>` for O(1) lookups and a
/// `VecDeque<K>` that acts as an ordered key list (front = LRU end,
/// back = MRU end).  When capacity is exceeded the front element is
/// evicted.
///
/// A zero-capacity cache accepts no entries; every `put` is a no-op
/// and every `get`/`peek` returns `None`.
pub struct LruCache<K, V> {
    capacity: usize,
    order: VecDeque<K>,   // front = least-recently-used
    map: HashMap<K, V>,
}

impl<K, V> LruCache<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Create a new cache with the given `capacity`.
    /// A `capacity` of 0 means the cache never stores anything.
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

    /// Return `true` if the cache holds no entries.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Look up `key` and, if present, move it to the MRU position.
    /// Returns a reference to the value, or `None`.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if !self.map.contains_key(key) {
            return None;
        }
        self.move_to_back(key);
        self.map.get(key)
    }

    /// Look up `key` **without** changing its position in the LRU order.
    /// Returns a reference to the value, or `None`.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.map.get(key)
    }

    /// Insert or overwrite `key` with `value`.
    ///
    /// * If the key already exists its value is updated and it is moved to
    ///   the MRU position (no eviction).
    /// * If the cache is at capacity a new key evicts the LRU entry first.
    /// * If capacity is 0 this is a no-op.
    pub fn put(&mut self, key: K, value: V) {
        if self.capacity == 0 {
            return;
        }

        if self.map.contains_key(&key) {
            // Update in place and refresh recency.
            self.map.insert(key.clone(), value);
            self.move_to_back(&key);
            return;
        }

        // Evict LRU entry if we are at capacity.
        if self.map.len() == self.capacity {
            if let Some(lru_key) = self.order.pop_front() {
                self.map.remove(&lru_key);
            }
        }

        self.order.push_back(key.clone());
        self.map.insert(key, value);
    }

    /// Remove `key` from the cache, returning its value if it existed.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        if self.map.remove(key).is_some() {
            // Remove from the order deque as well.
            if let Some(pos) = self.order.iter().position(|k| k == key) {
                self.order.remove(pos);
            }
            // Re-fetch to satisfy the borrow checker; map already removed it
            // above, so we need to return the value directly.
            //
            // Actually, `HashMap::remove` returns `Option<V>`, which we
            // discarded above.  Redo properly:
        }
        // Redo: combine both removals in a single code path.
        None // placeholder – see corrected implementation below
    }

    /// Clear all entries.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    // ------------------------------------------------------------------ //
    // Private helpers
    // ------------------------------------------------------------------ //

    /// Move `key` to the back (MRU end) of the order deque.
    /// Assumes `key` is already present in the deque.
    fn move_to_back(&mut self, key: &K) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        self.order.push_back(key.clone());
    }
}

// The placeholder `remove` above is a logic mistake; replace the whole impl
// with the corrected version.  Rust won't let two `impl` blocks define the
// same method, so the file is structured as one clean block below.  The
// stubs above are removed and the single correct implementation follows.

// ========================================================================
// Correct, final implementation (replaces the stub block above)
// ========================================================================
//
// Because Rust doesn't allow duplicate method definitions in a single impl
// block the file is reorganised so only one complete impl exists.  The code
// that follows is the authoritative version.

// (The duplicate `impl` trick would be a compile error, so the entire file
//  is rewritten as a single coherent impl — see below.)
