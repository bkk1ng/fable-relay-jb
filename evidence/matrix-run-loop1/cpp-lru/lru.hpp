#pragma once

#include <list>
#include <unordered_map>
#include <optional>
#include <stdexcept>
#include <utility>

// LRU<K, V> — header-only, C++17
//
// Capacity must be > 0. get/put are O(1) average.
// Most-recently-used item is at the front of the list;
// least-recently-used is at the back and is evicted on overflow.

template <typename K, typename V>
class LRU {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = std::pair<K, V>;

private:
    using list_t = std::list<value_type>;
    using iter_t = typename list_t::iterator;
    using map_t  = std::unordered_map<K, iter_t>;

    std::size_t cap_;
    list_t      items_;   // front = MRU, back = LRU
    map_t       index_;

    // Move the referenced list node to the front (mark as most-recently used).
    void touch(iter_t it) {
        if (it != items_.begin())
            items_.splice(items_.begin(), items_, it);
    }

public:
    explicit LRU(std::size_t capacity) : cap_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("LRU capacity must be > 0");
        index_.reserve(capacity);
    }

    // Non-copyable; move is fine.
    LRU(const LRU&)            = delete;
    LRU& operator=(const LRU&) = delete;
    LRU(LRU&&)                 = default;
    LRU& operator=(LRU&&)      = default;

    ~LRU() = default;

    // Returns the number of entries currently in the cache.
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }

    // Returns the maximum number of entries the cache will hold.
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

    // Look up key. Returns a reference to the value and promotes it to MRU.
    // Returns std::nullopt when the key is absent.
    std::optional<std::reference_wrapper<V>> get(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end())
            return std::nullopt;
        touch(it->second);
        return it->second->second;
    }

    // Insert or update. On update the entry is promoted to MRU.
    // On insert, if the cache is full the LRU entry is evicted first.
    void put(K key, V value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            // Update in-place with move, then promote.
            it->second->second = std::move(value);
            touch(it->second);
            return;
        }

        // Evict the LRU entry if at capacity.
        if (index_.size() >= cap_) {
            index_.erase(items_.back().first);
            items_.pop_back();
        }

        // Insert at the front.
        items_.emplace_front(std::move(key), std::move(value));
        index_.emplace(items_.front().first, items_.begin());
    }

    // Remove a key. Returns true when the key was present.
    bool erase(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end())
            return false;
        items_.erase(it->second);
        index_.erase(it);
        return true;
    }

    // Returns true when the cache contains the key (without promoting it).
    [[nodiscard]] bool contains(const K& key) const {
        return index_.count(key) != 0;
    }
};
