#pragma once

// lru.hpp — header-only LRU cache, C++17
// LRU<K,V>  capacity-bounded cache.
//   list front = most-recently-used, back = least-recently-used.
//   map  key → list iterator for O(1) lookup + splice.

#include <cstddef>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

template <typename K, typename V>
class LRU {
    using Entry = std::pair<K, V>;
    using List  = std::list<Entry>;
    using Map   = std::unordered_map<K, typename List::iterator>;

public:
    // capacity must be >= 1
    explicit LRU(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("LRU capacity must be > 0");
    }

    LRU(const LRU&)            = delete;
    LRU& operator=(const LRU&) = delete;
    LRU(LRU&&)                 = default;
    LRU& operator=(LRU&&)      = default;

    // Returns pointer to the cached value, or nullptr on miss.
    // Marks the entry as most-recently-used on hit.
    V* get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        list_.splice(list_.begin(), list_, it->second);
        return &it->second->second;
    }

    // Insert or update.  On insert when at capacity, evicts the LRU entry.
    void put(K key, V value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // update in place and promote to MRU
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        if (map_.size() == capacity_) {
            // evict least-recently-used (back of list)
            const K& lru_key = list_.back().first;
            map_.erase(lru_key);
            list_.pop_back();
        }
        list_.emplace_front(std::move(key), std::move(value));
        map_.emplace(list_.front().first, list_.begin());
    }

    // Remove a single entry.  Returns true if it was present.
    bool erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    std::size_t size()     const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool        empty()    const noexcept { return map_.empty(); }

private:
    std::size_t capacity_;
    List        list_;   // front = MRU, back = LRU
    Map         map_;
};
