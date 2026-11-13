#pragma once
// spsc.hpp — C++17 lock-free single-producer / single-consumer bounded queue
//
// Guarantees:
//   • try_push must only be called from one thread (the producer).
//   • try_pop  must only be called from one thread (the consumer).
//   • Capacity must be a power of two; enforced by static_assert.
//   • head and tail live on separate cache lines to eliminate false sharing.
//   • Each side caches the other's index to reduce cross-core atomic traffic.

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

// Use the C++17 hardware constant when available; fall back to 64 bytes.
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t kCacheLine =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t kCacheLine = 64;
#endif

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscQueue: Capacity must be a power of two");
    static constexpr std::size_t kMask = Capacity - 1;

    // -------------------------------------------------------------------------
    // Producer side — owns tail_, caches head_ to avoid frequent acquire-loads.
    // -------------------------------------------------------------------------
    struct alignas(kCacheLine) ProducerSide {
        std::atomic<std::size_t> tail{0};
        // Non-atomic: only the producer ever reads or writes this field.
        std::size_t head_cache{0};
    };
    static_assert(sizeof(ProducerSide) <= kCacheLine,
                  "ProducerSide exceeds one cache line — increase kCacheLine");

    // -------------------------------------------------------------------------
    // Consumer side — owns head_, caches tail_ to avoid frequent acquire-loads.
    // -------------------------------------------------------------------------
    struct alignas(kCacheLine) ConsumerSide {
        std::atomic<std::size_t> head{0};
        // Non-atomic: only the consumer ever reads or writes this field.
        std::size_t tail_cache{0};
    };
    static_assert(sizeof(ConsumerSide) <= kCacheLine,
                  "ConsumerSide exceeds one cache line — increase kCacheLine");

    // -------------------------------------------------------------------------
    // Ring buffer — aligned so it doesn't share a line with ConsumerSide.
    // -------------------------------------------------------------------------
    ProducerSide prod_{};
    ConsumerSide cons_{};
    alignas(kCacheLine) T buf_[Capacity];

public:
    SpscQueue()  = default;
    ~SpscQueue() = default;

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // -------------------------------------------------------------------------
    // try_push — copy.  Returns false (without blocking) when the queue is full.
    // Must be called from exactly one producer thread.
    // -------------------------------------------------------------------------
    bool try_push(const T& value) {
        const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (tail - prod_.head_cache >= Capacity) {
            // Stale cache; refresh with an acquire-load to sync with try_pop.
            prod_.head_cache = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.head_cache >= Capacity)
                return false;   // truly full
        }
        buf_[tail & kMask] = value;
        // Release: makes the slot write visible to the consumer.
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // try_push — move.  Same contract as the copy overload.
    // -------------------------------------------------------------------------
    bool try_push(T&& value) {
        const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (tail - prod_.head_cache >= Capacity) {
            prod_.head_cache = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.head_cache >= Capacity)
                return false;
        }
        buf_[tail & kMask] = std::move(value);
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // try_pop.  Returns false (without blocking) when the queue is empty.
    // Moves the slot value into `val` on success.
    // Must be called from exactly one consumer thread.
    // -------------------------------------------------------------------------
    bool try_pop(T& val) {
        const std::size_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.tail_cache) {
            // Stale cache; refresh with an acquire-load to sync with try_push.
            cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.tail_cache)
                return false;   // truly empty
        }
        val = std::move(buf_[head & kMask]);
        // Release: makes the slot read visible to the producer (slot is free).
        cons_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // Approximate occupancy — no fence, may be stale.
    // -------------------------------------------------------------------------
    std::size_t size() const noexcept {
        const std::size_t h = cons_.head.load(std::memory_order_relaxed);
        const std::size_t t = prod_.tail.load(std::memory_order_relaxed);
        return t - h;
    }

    bool empty() const noexcept { return size() == 0; }

    static constexpr std::size_t capacity() noexcept { return Capacity; }
};
