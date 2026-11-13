#pragma once

// Lock-free single-producer / single-consumer bounded queue — C++17
//
// Ordering model
//   Producer: relaxed-load head (owns it), acquire-load tail (needs consumer's
//             latest store), write slot, release-store head.
//   Consumer: relaxed-load tail (owns it), acquire-load head (needs producer's
//             slot write to be visible), read slot, release-store tail.
//
// The release on head_ / tail_ pairs with the acquire on the other side,
// making the slot data visible across the cache-coherence boundary without
// a full sequential-consistency fence.
//
// Restriction: T must be trivially copyable.  For heap-allocated or
// non-trivial types, slot construction / destruction bookkeeping is needed;
// that is out of scope for this implementation.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>          // std::align_val_t
#include <type_traits>

template <typename T>
class SPSCQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue requires a trivially copyable element type");

public:
    // Allocates the next power-of-two slots >= min_capacity.
    explicit SPSCQueue(std::size_t min_capacity) {
        assert(min_capacity >= 1);
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        capacity_ = cap;
        mask_     = cap - 1;
        // Align the slot buffer to a cache line so the first element isn't
        // sharing a line with unrelated heap metadata.
        slots_ = static_cast<T*>(
            ::operator new(cap * sizeof(T),
                           std::align_val_t{kCacheLine}));
    }

    ~SPSCQueue() {
        ::operator delete(slots_, std::align_val_t{kCacheLine});
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    // --- Producer API (call from one thread only) ---

    // Returns true and enqueues val, or returns false if the queue is full.
    [[nodiscard]] bool try_push(T val) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        // Acquire the consumer's tail so we see the most recent slot reads.
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= capacity_) return false;   // full
        slots_[h & mask_] = val;
        // Release: makes the slot write visible to the consumer before it
        // observes the new head value.
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // --- Consumer API (call from one thread only) ---

    // Returns true and writes the front element to out, or returns false if
    // the queue is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        // Acquire the producer's head so we see all slot writes sequenced
        // before the head store.
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (h == t) return false;               // empty
        out = slots_[t & mask_];
        // Release: makes it visible to the producer that this slot is free.
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Approximate size; exact only when called from a single thread with no
    // concurrent activity.
    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

private:
    static constexpr std::size_t kCacheLine = 64;

    // Each atomic lives on its own cache line to prevent false sharing between
    // the producer (which writes head_) and the consumer (which writes tail_).
    // The pad arrays fill the remainder of each cache line.
    static constexpr std::size_t kAtomicSz = sizeof(std::atomic<std::size_t>);
    static_assert(kAtomicSz <= kCacheLine,
                  "atomic<size_t> wider than one cache line — revisit padding");

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    char pad_head_[kCacheLine - kAtomicSz];

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    char pad_tail_[kCacheLine - kAtomicSz];

    // Read by both threads; not written after construction, so shared reads
    // from different threads are fine without synchronisation.
    T*          slots_{nullptr};
    std::size_t capacity_{0};
    std::size_t mask_{0};
};
