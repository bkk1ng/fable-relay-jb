#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// LRU<K,V> — header-only least-recently-used cache (C++17)
//
// Invariants:
//   * list_ front  = most-recently used
//   * list_ back   = least-recently used (next eviction candidate)
//   * map_ holds iterators into list_ for O(1) lookup + splice
template <typename K, typename V>
class LRU {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = std::pair<K, V>;   // what the list stores

    // -----------------------------------------------------------------
    // construction
    // -----------------------------------------------------------------

    explicit LRU(std::size_t capacity)
        : capacity_(capacity)
    {
        if (capacity_ == 0)
            throw std::invalid_argument("LRU: capacity must be > 0");
        map_.reserve(capacity_);
    }

    // non-copyable (iterators would dangle), movable
    LRU(const LRU&)            = delete;
    LRU& operator=(const LRU&) = delete;
    LRU(LRU&&)                 = default;
    LRU& operator=(LRU&&)      = default;

    ~LRU() = default;

    // -----------------------------------------------------------------
    // observers
    // -----------------------------------------------------------------

    std::size_t size()     const noexcept { return list_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool        empty()    const noexcept { return list_.empty(); }

    // -----------------------------------------------------------------
    // get
    //
    // Returns a copy of the value associated with key, or std::nullopt.
    // A hit promotes the entry to MRU position.
    // -----------------------------------------------------------------
    std::optional<V> get(const K& key)
    {
        auto it = map_.find(key);
        if (it == map_.end())
            return std::nullopt;
        touch(it->second);
        return it->second->second;   // copy of V
    }

    // -----------------------------------------------------------------
    // put
    //
    // Insert or update.  Accepts key and value by value so the caller
    // can move in.  An update also promotes the entry to MRU position.
    // Evicts the LRU entry when the cache is at capacity.
    // -----------------------------------------------------------------
    void put(K key, V value)
    {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // update existing entry in-place, then move to front
            it->second->second = std::move(value);
            touch(it->second);
            return;
        }

        if (list_.size() == capacity_)
            evict();

        list_.emplace_front(std::move(key), std::move(value));
        // list_.front().first is the canonical key storage; the map
        // stores a reference to that same string/key to avoid duplication
        map_.emplace(list_.front().first, list_.begin());
    }

    // -----------------------------------------------------------------
    // erase
    //
    // Removes the entry for key.  Returns true if the key was present.
    // -----------------------------------------------------------------
    bool erase(const K& key)
    {
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

private:
    using list_t = std::list<value_type>;
    using iter_t = typename list_t::iterator;

    // Move an existing list node to the front (O(1) splice).
    void touch(iter_t it)
    {
        list_.splice(list_.begin(), list_, it);
    }

    // Drop the LRU entry (back of list).
    void evict()
    {
        map_.erase(list_.back().first);
        list_.pop_back();
    }

    std::size_t                      capacity_;
    list_t                           list_;
    std::unordered_map<K, iter_t>    map_;
};
