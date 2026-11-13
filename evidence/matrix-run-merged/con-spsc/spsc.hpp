// spsc.hpp — Lock-free single-producer / single-consumer bounded queue
// C++17, header-only.  Power-of-two capacity with bitmask indexing,
// acquire/release atomics for cross-thread ordering, and cache-line
// alignment so the producer and consumer indices don't false-share.
//
// Thread-safety contract:
//   * Exactly one thread calls try_push (producer).
//   * Exactly one thread calls try_pop  (consumer).
//   * No external locking required.
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace spsc {

inline constexpr std::size_t cache_line_size = 64;

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 1, "Capacity must be >= 1");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr std::size_t capacity = Capacity;

    SpscQueue() = default;
    ~SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // ── Producer side ───────────────────────────────────────────────
    // Enqueue one element.  Returns false when the queue is full.
    bool try_push(const T& value) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (t - h >= Capacity)          // queue full
            return false;
        buffer_[t & mask_] = value;
        // Release: the buffer write above must be visible to the
        // consumer before it observes the incremented tail.
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (t - h >= Capacity)
            return false;
        buffer_[t & mask_] = std::move(value);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // ── Consumer side ────────────────────────────────────────────────
    // Dequeue one element into out.  Returns false when the queue
    // is empty.
    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (h == t)                     // queue empty
            return false;
        out = std::move(buffer_[h & mask_]);
        // Release: the buffer read above must complete before the
        // producer observes the incremented head (and reuses the slot).
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate count — diagnostics only, not a synchronization point.
    std::size_t size() const {
        return tail_.load(std::memory_order_relaxed)
             - head_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    // head_ — owned by the consumer, observed by the producer (acquire).
    // tail_ — owned by the producer, observed by the consumer (acquire).
    // alignas(64) pins each to its own cache line, so the producer writing
    // tail_ never invalidates the cache line the consumer reads head_ from.
    alignas(cache_line_size) std::atomic<std::size_t> head_{0};
    alignas(cache_line_size) std::atomic<std::size_t> tail_{0};

    T buffer_[Capacity];
};

} // namespace spsc
