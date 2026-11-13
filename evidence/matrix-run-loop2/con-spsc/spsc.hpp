#pragma once

// spsc.hpp — lock-free single-producer / single-consumer bounded queue
// C++17, std::atomic acquire/release, cache-line padded head and tail.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

static constexpr std::size_t kCacheLineBytes = 64;

// SpscQueue<T, Capacity>
//   Capacity must be a power of two; the implementation uses bitwise masking
//   instead of modulo.  Maximum live elements is Capacity - 1.
//
//   Only ONE thread may call try_push at a time (the producer).
//   Only ONE thread may call try_pop  at a time (the consumer).
//   Both threads may operate concurrently with no external locking.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscQueue()  = default;
    ~SpscQueue() = default;

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&)                 = delete;
    SpscQueue& operator=(SpscQueue&&)      = delete;

    // Producer side.  Returns false if the queue is full.
    bool try_push(const T& val) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t tail     = tail_.load(std::memory_order_relaxed);
        const std::size_t next     = (tail + 1) & kMask;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;   // full
        }
        buffer_[tail] = val;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push(T&& val) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail     = tail_.load(std::memory_order_relaxed);
        const std::size_t next     = (tail + 1) & kMask;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[tail] = std::move(val);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side.  Returns false if the queue is empty.
    bool try_pop(T& out) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;   // empty
        }
        out = buffer_[head];
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate; not safe to call from both threads simultaneously.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity - 1; }

private:
    // Keep head_ and tail_ on separate cache lines so the producer and
    // consumer do not bounce the same line back and forth.

    alignas(kCacheLineBytes) std::atomic<std::size_t> head_{0};
    // Explicit padding to the next cache line boundary.
    char pad0_[kCacheLineBytes - sizeof(std::atomic<std::size_t>)]{};

    alignas(kCacheLineBytes) std::atomic<std::size_t> tail_{0};
    char pad1_[kCacheLineBytes - sizeof(std::atomic<std::size_t>)]{};

    // Buffer on its own cache line so the first slot is not shared with tail_.
    alignas(kCacheLineBytes) T buffer_[Capacity]{};
};
