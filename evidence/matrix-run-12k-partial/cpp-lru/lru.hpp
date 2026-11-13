#pragma once

#include <cstddef>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// LRU<K, V> — a fixed-capacity least-recently-used cache.
//
// - get()  returns a pointer to the value (nullptr on miss) and marks the
//          entry as most-recently-used.
// - put()  inserts or updates a key; evicts the LRU entry when at capacity.
// - erase() removes a key; returns true if it was present.
// - size() / capacity() are O(1).
//
// All operations are O(1) average.  The list keeps insertion-order with the
// MRU entry at the front and the LRU entry at the back; the map holds
// iterators into that list so splice and erase are O(1) without invalidating
// any other iterator.

template <typename K, typename V>
class LRU {
    using Pair  = std::pair<K, V>;
    using List  = std::list<Pair>;
    using Map   = std::unordered_map<K, typename List::iterator>;

public:
    explicit LRU(std::size_t capacity) : cap_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("LRU: capacity must be > 0");
        map_.reserve(capacity);
    }

    // Non-copyable; movable.
    LRU(const LRU&)            = delete;
    LRU& operator=(const LRU&) = delete;
    LRU(LRU&&)                 = default;
    LRU& operator=(LRU&&)      = default;

    // Returns a pointer to the cached value, or nullptr on a miss.
    // On a hit the entry becomes the most-recently-used.
    V* get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return nullptr;
        // splice is O(1) and does not invalidate the iterator stored in map_
        list_.splice(list_.begin(), list_, it->second);
        return &it->second->second;
    }

    // Const overload — does not update recency.
    const V* peek(const K& key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second->second;
    }

    // Insert or update.  key and value are taken by value so the caller can
    // pass either lvalues (copied once) or rvalues (moved).
    void put(K key, V value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update the value in-place and move the node to the front.
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        // Evict the LRU entry if we are at capacity.
        if (list_.size() == cap_) {
            map_.erase(list_.back().first);
            list_.pop_back();
        }
        // Emplace at the front; the key is moved into the list node and the
        // map key is taken from that same node so nothing is copied twice.
        list_.emplace_front(std::move(key), std::move(value));
        map_.emplace(list_.front().first, list_.begin());
    }

    // Remove a key.  Returns true if it was present.
    bool erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    // O(1) membership test without updating recency.
    bool contains(const K& key) const {
        return map_.count(key) != 0;
    }

    std::size_t size()     const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return cap_; }
    bool        empty()    const noexcept { return map_.empty(); }

    void clear() noexcept {
        list_.clear();
        map_.clear();
    }

private:
    std::size_t cap_;
    List        list_;  // front = MRU, back = LRU
    Map         map_;
};
