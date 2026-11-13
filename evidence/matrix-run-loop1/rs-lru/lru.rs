use std::collections::{HashMap, VecDeque};
use std::hash::Hash;

/// A fixed-capacity Least Recently Used cache.
///
/// Ordering is tracked with a `VecDeque<K>` (front = most recent,
/// back = least recent); values are stored in a `HashMap<K, V>`.
#[derive(Debug)]
pub struct LruCache<K, V> {
    capacity: usize,
    entries: HashMap<K, V>,
    order: VecDeque<K>,
}

impl<K, V> LruCache<K, V>
where
    K: Eq + Hash + Clone,
{
    /// Create a cache that holds at most `capacity` entries.
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            entries: HashMap::with_capacity(capacity),
            order: VecDeque::with_capacity(capacity),
        }
    }

    /// Maximum number of entries the cache can hold.
    pub fn capacity(&self) -> usize {
        self.capacity
    }

    /// Number of entries currently stored.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the cache is empty.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Remove every entry.
    pub fn clear(&mut self) {
        self.entries.clear();
        self.order.clear();
    }

    /// Look up a key and mark it most recently used.
    /// Returns `None` when the key is absent.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if !self.entries.contains_key(key) {
            return None;
        }
        self.touch(key);
        self.entries.get(key)
    }

    /// Look up a key without changing recency.
    pub fn peek(&self, key: &K) -> Option<&V> {
        self.entries.get(key)
    }

    /// Insert or overwrite an entry, evicting the least recently used
    /// item when the cache is full. A zero-capacity cache stores nothing.
    pub fn put(&mut self, key: K, value: V) {
        if self.capacity == 0 {
            return;
        }

        // Overwrite: update the stored value and bump recency.
        if let Some(slot) = self.entries.get_mut(&key) {
            *slot = value;
            self.touch(&key);
            return;
        }

        // New entry: evict the back of the deque if at capacity.
        if self.len() >= self.capacity {
            if let Some(evicted) = self.order.pop_back() {
                self.entries.remove(&evicted);
            }
        }

        self.entries.insert(key.clone(), value);
        self.order.push_front(key);
    }

    /// Remove a key and return its value if it was present.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        if let Some(idx) = self.order.iter().position(|k| k == key) {
            self.order.remove(idx);
        }
        self.entries.remove(key)
    }

    /// Move `key` to the front (most recent) of the ordering deque.
    fn touch(&mut self, key: &K) {
        if let Some(idx) = self.order.iter().position(|k| k == key) {
            let k = self.order.remove(idx).expect("index from position is valid");
            self.order.push_front(k);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn eviction_order() {
        let mut cache: LruCache<u32, &str> = LruCache::new(3);

        cache.put(1, "one");
        cache.put(2, "two");
        cache.put(3, "three");

        // Access key 1 — it becomes most recently used.
        cache.get(&1);

        // Insert key 4 — key 2 is now the LRU and gets evicted.
        cache.put(4, "four");
        assert_eq!(cache.get(&2), None);
        assert_eq!(cache.peek(&1), Some(&"one"));
        assert_eq!(cache.peek(&3), Some(&"three"));
        assert_eq!(cache.peek(&4), Some(&"four"));

        // Order is now [4, 1, 3] (MRU -> LRU). Insert key 5 — evicts 3.
        cache.put(5, "five");
        assert_eq!(cache.get(&3), None);
        assert_eq!(cache.len(), 3);
    }

    #[test]
    fn overwrite_existing_key() {
        let mut cache: LruCache<u32, &str> = LruCache::new(2);

        cache.put(1, "one");
        cache.put(2, "two");

        // Overwrite key 1 — value updates and recency bumps.
        cache.put(1, "ONE");
        assert_eq!(cache.peek(&1), Some(&"ONE"));
        assert_eq!(cache.len(), 2);

        // Key 1 is now MRU, so key 2 is the next eviction target.
        cache.put(3, "three");
        assert_eq!(cache.get(&2), None);
        assert_eq!(cache.peek(&1), Some(&"ONE"));
        assert_eq!(cache.peek(&3), Some(&"three"));
    }

    #[test]
    fn zero_capacity() {
        let mut cache: LruCache<u32, &str> = LruCache::new(0);

        cache.put(1, "one");
        assert_eq!(cache.len(), 0);
        assert_eq!(cache.capacity(), 0);
        assert_eq!(cache.get(&1), None);
        assert_eq!(cache.peek(&1), None);
        assert_eq!(cache.remove(&1), None);

        cache.clear();
        assert_eq!(cache.len(), 0);
        assert!(cache.is_empty());
    }
}
