// bench.cpp — SPSCQueue benchmark
// Pushes 10 million uint64_t values from a producer thread and pops them
// in a consumer thread, verifying that every value arrives in order.
// Prints throughput in millions of operations per second at the end.
//
// Build (C++17):
//   g++ -std=c++17 -O2 -pthread -o bench bench.cpp
//   clang++ -std=c++17 -O2 -pthread -o bench bench.cpp
//
// Run:
//   ./bench

#include "spsc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

static constexpr uint64_t kMessages  = 10'000'000ULL;
static constexpr uint64_t kQueueSize = 65'536ULL;   // 64 KiB worth of slots

int main() {
    SPSCQueue<uint64_t> q(kQueueSize);

    // Signals between main and the consumer thread.
    std::atomic<bool> ready{false};
    std::atomic<bool> ok{true};        // set to false if ordering breaks

    // Synchronise start: producer waits until the consumer is running.
    std::atomic<bool> go{false};

    std::thread consumer([&] {
        uint64_t expected = 0;
        uint64_t popped   = 0;
        uint64_t val      = 0;

        // Wait for the producer to signal "go".
        while (!go.load(std::memory_order_acquire)) {
            // tight spin — both threads are running by the time 'go' is set
        }

        while (popped < kMessages) {
            if (q.try_pop(val)) {
                if (val != expected) {
                    ok.store(false, std::memory_order_relaxed);
                    // Keep draining so the producer doesn't stall.
                }
                ++expected;
                ++popped;
            }
        }
        ready.store(true, std::memory_order_release);
    });

    // Give the consumer thread a moment to get scheduled before starting the
    // clock — avoids measuring OS scheduler latency.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto t0 = std::chrono::steady_clock::now();

    // Unblock the consumer now that the timer is started.
    go.store(true, std::memory_order_release);

    // Producer: push 0 … kMessages-1 in order.
    for (uint64_t i = 0; i < kMessages; ++i) {
        // Spin on a full queue — normal for a bounded ring.
        while (!q.try_push(i)) {
            // Let the OS reschedule rather than burning a core indefinitely.
            std::this_thread::yield();
        }
    }

    // Wait for the consumer to finish before stopping the clock.
    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto t1 = std::chrono::steady_clock::now();

    consumer.join();

    // Report
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double mops      = static_cast<double>(kMessages) / elapsed_s / 1e6;

    std::printf("messages : %llu\n",   (unsigned long long)kMessages);
    std::printf("queue cap: %llu\n",   (unsigned long long)q.capacity());
    std::printf("time     : %.3f s\n", elapsed_s);
    std::printf("throughput: %.2f M ops/s\n", mops);
    std::printf("ordering : %s\n", ok.load() ? "OK" : "FAIL — values out of order!");

    return ok.load() ? 0 : 1;
}
