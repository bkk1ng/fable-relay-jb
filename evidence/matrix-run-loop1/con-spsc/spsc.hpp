#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <new>

// Cache line size — 64 bytes on x86/ARM
static constexpr std::size_t kCacheLineSize = 64;

// Rounds up to the next power of two (compile-time).
constexpr std::size_t next_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ----------------------------------------------------------------------------
// SPSCQueue<T, Capacity>
//
// Lock-free, bounded, single-producer / single-consumer queue.
//
// Template parameters:
//   T        – element type (must be trivially copyable for best performance)
//   Capacity – requested capacity; actual capacity is next_pow2(Capacity)
//
// Ordering contract:
//   try_push  – stores element, then releases tail_ (release store)
//   try_pop   – acquires tail_ then acquires head_ (acquire loads)
//
// Cache layout:
//   head_ and tail_ live on separate cache lines to prevent false sharing
//   between the producer core (owns tail_) and the consumer core (owns head_).
// ----------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
class SPSCQueue {
public:
    static_assert(Capacity > 0, "Capacity must be > 0");

    static constexpr std::size_t kCapacity = next_pow2(Capacity);
    static constexpr std::size_t kMask     = kCapacity - 1;

    SPSCQueue() : head_(0), tail_(0) {
        buf_ = std::make_unique<T[]>(kCapacity);
    }

    // Not copyable or movable — the queue owns its buffer and atomic state.
    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // -------------------------------------------------------------------------
    // try_push — called only by the producer thread.
    //
    // Returns true if the element was enqueued, false if the queue was full.
    // Uses a relaxed load on head_ (producer only reads it to check fullness)
    // and a release store on tail_ so the consumer sees the written element.
    // -------------------------------------------------------------------------
    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & kMask;

        // Full when next write position == read position
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buf_[tail] = value;

        // Release: everything above is visible before the consumer reads tail_
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Move overload — avoids a copy for non-trivial T.
    bool try_push(T&& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & kMask;

        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buf_[tail] = std::move(value);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // try_pop — called only by the consumer thread.
    //
    // Returns the element wrapped in std::optional, or std::nullopt if empty.
    // Acquire load on tail_ pairs with the producer's release store; acquire
    // store on head_ keeps later loads from floating above the pop.
    // -------------------------------------------------------------------------
    std::optional<T> try_pop() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // Acquire: see all writes the producer released before storing tail_
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;  // empty
        }

        T value = buf_[head];

        // Release: prevent reordering of the read above with future pushes
        head_.store((head + 1) & kMask, std::memory_order_release);
        return value;
    }

    // Approximate occupancy — not exact under concurrent use.
    std::size_t size_approx() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_relaxed);
        return (tail - head + kCapacity) & kMask;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept { return kCapacity - 1; }

private:
    // Separate cache lines to eliminate false sharing between producer (tail_)
    // and consumer (head_).  alignas pads each member to its own line.

    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;

    // Ring buffer — kCapacity slots, one slot always reserved as the sentinel
    // that distinguishes full from empty without a separate counter.
    std::unique_ptr<T[]> buf_;
};
