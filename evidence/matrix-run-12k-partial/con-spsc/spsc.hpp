#pragma once

// C++17 lock-free single-producer / single-consumer bounded queue.
//
// Guarantees:
//   - Capacity is rounded up to the next power of two; indexing uses a bitmask.
//   - head_ and tail_ each occupy their own cache line to eliminate false sharing.
//   - try_push uses release on head_; try_pop acquires head_ and releases tail_.
//   - Slots are untyped storage; elements are placement-new'd and explicitly
//     destroyed, so T need not be default-constructible.
//
// Thread safety: exactly one producer thread calls try_push, exactly one
// consumer thread calls try_pop. All other members are read-only after
// construction.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

template <typename T>
class SPSCQueue {
public:
    // 64 bytes covers x86-64 and ARM64 coherency granules.
    static constexpr std::size_t kCacheLineSize = 64;

    // min_capacity is rounded up to the next power of two internally.
    explicit SPSCQueue(std::size_t min_capacity) {
        assert(min_capacity > 0 && "capacity must be > 0");
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        capacity_ = cap;
        mask_     = cap - 1;

        const std::size_t align =
            alignof(T) > kCacheLineSize ? alignof(T) : kCacheLineSize;
        storage_ = static_cast<std::byte*>(
            ::operator new(cap * sizeof(T), std::align_val_t{align}));
    }

    ~SPSCQueue() noexcept {
        // Destroy any elements that were pushed but never popped.
        if constexpr (!std::is_trivially_destructible_v<T>) {
            std::size_t tail = tail_.load(std::memory_order_acquire);
            const std::size_t head = head_.load(std::memory_order_acquire);
            while (tail != head) {
                slot(tail & mask_)->~T();
                ++tail;
            }
        }
        const std::size_t align =
            alignof(T) > kCacheLineSize ? alignof(T) : kCacheLineSize;
        ::operator delete(storage_, std::align_val_t{align});
    }

    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&)                 = delete;
    SPSCQueue& operator=(SPSCQueue&&)      = delete;

    // Producer side — call from exactly one thread.
    // Returns false without blocking if the queue is full.
    template <typename U>
    [[nodiscard]] bool try_push(U&& val) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire tail_ so we see the consumer's store that freed the slot.
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail == capacity_) return false;   // full

        new (slot(head & mask_)) T(std::forward<U>(val));

        // Release: makes the constructed element visible to the consumer.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side — call from exactly one thread.
    // Returns false without blocking if the queue is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept(
        std::is_nothrow_move_assignable_v<T> && std::is_nothrow_destructible_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire head_ so we see the element the producer stored.
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;               // empty

        T* p = slot(tail & mask_);
        out  = std::move(*p);
        p->~T();

        // Release: makes the freed slot visible to the producer.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Approximate — only safe to call from one of the two threads.
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    T* slot(std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(storage_ + index * sizeof(T)));
    }

    // Read-only after construction; shared by both threads but never written
    // concurrently, so no special alignment needed here.
    std::size_t  capacity_{0};
    std::size_t  mask_{0};
    std::byte*   storage_{nullptr};

    // Written only by the producer, read by both.
    // Padded to its own cache line to prevent false sharing with tail_.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    // Pad to fill the rest of the cache line so tail_ lands on the next one.
    char head_pad_[kCacheLineSize - sizeof(std::atomic<std::size_t>)]{};

    // Written only by the consumer, read by both.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    char tail_pad_[kCacheLineSize - sizeof(std::atomic<std::size_t>)]{};
};
