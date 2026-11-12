#pragma once

// LRU<K, V> — header-only, C++17
// Policy: front of list = MRU, back = LRU.
// get/put are O(1) average via unordered_map + splice.

#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

template <typename K, typename V>
class LRU {
public:
    explicit LRU(std::size_t capacity) : cap_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("LRU capacity must be > 0");
        map_.reserve(capacity);
    }

    // Non-copyable (iterator stability; copying a whole cache is rarely what you want).
    LRU(const LRU&) = delete;
    LRU& operator=(const LRU&) = delete;

    LRU(LRU&&) = default;
    LRU& operator=(LRU&&) = default;

    ~LRU() = default;

    // Returns a copy of the value and promotes the entry to MRU.
    // Returns std::nullopt when the key is absent.
    std::optional<V> get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return std::nullopt;
        list_.splice(list_.begin(), list_, it->second);
        return it->second->second;  // copy
    }

    // Insert or update. Accepts the key and value by value so callers can
    // move into either. On update the new value replaces in-place and the
    // entry is promoted to MRU. On overflow the LRU entry is evicted first.
    void put(K key, V value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        // Evict before inserting so the map never exceeds cap_.
        if (map_.size() == cap_) {
            map_.erase(list_.back().first);
            list_.pop_back();
        }

        // key is copied into the list node; the same key is then moved into
        // the map to avoid a second copy.
        list_.emplace_front(key, std::move(value));
        map_.emplace(std::move(key), list_.begin());
    }

    // Removes the entry for key. Returns true if an entry was erased.
    bool erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    std::size_t size()     const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return cap_; }
    bool        empty()    const noexcept { return map_.empty(); }

private:
    using Entry   = std::pair<K, V>;
    using List    = std::list<Entry>;
    using ListIt  = typename List::iterator;
    using HashMap = std::unordered_map<K, ListIt>;

    std::size_t cap_;
    List        list_;   // front = MRU, back = LRU
    HashMap     map_;
};
