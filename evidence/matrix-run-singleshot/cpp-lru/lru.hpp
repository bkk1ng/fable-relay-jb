#pragma once

// lru.hpp — header-only LRU cache, C++17
// LRU<K, V>: O(1) get / put / erase via std::list + std::unordered_map.
// list front = MRU, list back = LRU.

#include <cstddef>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

template <typename K, typename V>
class LRU {
public:
    // ── types ────────────────────────────────────────────────────────────────
    using key_type    = K;
    using mapped_type = V;
    using size_type   = std::size_t;

private:
    using Entry     = std::pair<K, V>;
    using List      = std::list<Entry>;
    using ListIter  = typename List::iterator;
    using Map       = std::unordered_map<K, ListIter>;

public:
    // ── construction ─────────────────────────────────────────────────────────
    explicit LRU(size_type capacity) : cap_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("LRU capacity must be > 0");
        map_.reserve(capacity);
    }

    // move-only: iterator stability requires we don't copy the list
    LRU(const LRU&)            = delete;
    LRU& operator=(const LRU&) = delete;
    LRU(LRU&&)                 = default;
    LRU& operator=(LRU&&)      = default;

    // ── accessors ────────────────────────────────────────────────────────────

    /// Returns a copy of the value for key, or nullopt if absent.
    /// Moves the entry to the MRU position on hit.
    [[nodiscard]] std::optional<V> get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        touch(it->second);
        return it->second->second;   // copy after splice
    }

    /// Insert or update key → value.
    /// On update the entry moves to MRU. On insert the LRU entry is evicted
    /// when size would exceed capacity.
    void put(K key, V value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            touch(it->second);
            return;
        }
        if (list_.size() == cap_) evict();
        list_.emplace_front(key, std::move(value));
        map_.emplace(std::move(key), list_.begin());
    }

    /// Remove key. Returns true iff the key was present.
    bool erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    // ── observers ────────────────────────────────────────────────────────────
    [[nodiscard]] size_type size()     const noexcept { return map_.size(); }
    [[nodiscard]] size_type capacity() const noexcept { return cap_; }
    [[nodiscard]] bool      empty()    const noexcept { return map_.empty(); }

private:
    // Move iter to list front (O(1), no allocation).
    void touch(ListIter it) {
        list_.splice(list_.begin(), list_, it);
    }

    // Drop the least-recently-used entry (list back).
    void evict() {
        map_.erase(list_.back().first);
        list_.pop_back();
    }

    size_type cap_;
    List      list_;
    Map       map_;
};
