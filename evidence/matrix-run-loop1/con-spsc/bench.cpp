// bench.cpp — SPSC queue benchmark
// Pushes 10 million uint64_t values from a producer thread and pops them on
// a consumer thread, verifying strict ordering and printing throughput.
//
// Build (C++17):
//   g++ -std=c++17 -O2 -pthread -o bench bench.cpp
//   clang++ -std=c++17 -O2 -pthread -o bench bench.cpp
//
// Run:
//   ./bench

#include "spsc.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

static constexpr uint64_t kMessages  = 10'000'000ULL;
static constexpr std::size_t kQCap   = 1 << 16;   // 65536 slots (power-of-two)

int main() {
    // Queue lives on the heap so both threads share the same object.
    SPSCQueue<uint64_t, kQCap> queue;

    std::printf("SPSC queue bench\n");
    std::printf("  capacity : %zu slots\n", queue.capacity());
    std::printf("  messages : %llu\n", static_cast<unsigned long long>(kMessages));
    std::printf("  element  : uint64_t (%zu bytes)\n\n", sizeof(uint64_t));

    // Shared error flag — consumer sets it on ordering violation.
    std::atomic<bool> ordering_ok{true};

    const auto t0 = std::chrono::steady_clock::now();

    // -------------------------------------------------------------------------
    // Producer thread — pushes 0 .. kMessages-1 in order.
    // Busy-spins on try_push when the queue is full.
    // -------------------------------------------------------------------------
    std::thread producer([&] {
        for (uint64_t i = 0; i < kMessages; ++i) {
            while (!queue.try_push(i)) {
                // Yield the CPU slice rather than burning a hot spin.
                // On a loaded system this matters; on an idle box it's a no-op.
                std::this_thread::yield();
            }
        }
    });

    // -------------------------------------------------------------------------
    // Consumer thread — pops and checks strict monotone increment.
    // -------------------------------------------------------------------------
    std::thread consumer([&] {
        uint64_t expected = 0;
        uint64_t received = 0;

        while (received < kMessages) {
            auto val = queue.try_pop();
            if (!val) {
                std::this_thread::yield();
                continue;
            }

            if (*val != expected) {
                // Report the first violation and bail out.
                std::fprintf(stderr,
                    "ordering violation at msg %llu: expected %llu, got %llu\n",
                    static_cast<unsigned long long>(received),
                    static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(*val));
                ordering_ok.store(false, std::memory_order_relaxed);
                return;
            }

            ++expected;
            ++received;
        }
    });

    producer.join();
    consumer.join();

    const auto t1 = std::chrono::steady_clock::now();

    // -------------------------------------------------------------------------
    // Results
    // -------------------------------------------------------------------------
    if (!ordering_ok.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "FAIL: ordering violation detected\n");
        return 1;
    }

    const double elapsed_s =
        std::chrono::duration<double>(t1 - t0).count();
    const double throughput_mps = static_cast<double>(kMessages) / elapsed_s / 1e6;
    const double ns_per_msg     = elapsed_s * 1e9 / static_cast<double>(kMessages);

    std::printf("PASS: all %llu messages received in order\n\n",
        static_cast<unsigned long long>(kMessages));
    std::printf("elapsed   : %.3f s\n",   elapsed_s);
    std::printf("throughput: %.2f M msg/s\n", throughput_mps);
    std::printf("latency   : %.1f ns/msg (round-trip, not one-way)\n", ns_per_msg);

    return 0;
}
