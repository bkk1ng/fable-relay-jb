// bench.cpp — SPSC queue benchmark
// Pushes 10 million uint64_t values from a producer thread and pops them on
// the consumer thread, verifying sequential ordering and printing throughput.
//
// Build (C++17):
//   g++ -std=c++17 -O2 -pthread -o bench bench.cpp
//   clang++ -std=c++17 -O2 -pthread -o bench bench.cpp

#include "spsc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

static constexpr std::size_t  kQueueCap  = 1 << 14;   // 16 384 slots
static constexpr std::uint64_t kMessages = 10'000'000;

int main() {
    // Queue lives on the heap to avoid stack-size surprises.
    auto* q = new SpscQueue<std::uint64_t, kQueueCap>();

    // Shared flag: producer sets this after sending all items so the consumer
    // knows when no more items are coming.
    std::atomic<bool> done{false};

    // ── producer ────────────────────────────────────────────────────────────
    auto producer = [&]() {
        for (std::uint64_t i = 0; i < kMessages; ++i) {
            // Spin until there is space.
            while (!q->try_push(i)) {
                // Yield to avoid starving the consumer on a single-core host.
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    };

    // ── consumer ────────────────────────────────────────────────────────────
    std::atomic<bool> ordering_ok{true};
    std::atomic<std::uint64_t> received{0};

    auto consumer = [&]() {
        std::uint64_t expected = 0;
        std::uint64_t val      = 0;

        while (true) {
            if (q->try_pop(val)) {
                if (val != expected) {
                    ordering_ok.store(false, std::memory_order_relaxed);
                    // Keep draining so the producer is never blocked forever.
                }
                ++expected;
                received.fetch_add(1, std::memory_order_relaxed);
            } else if (done.load(std::memory_order_acquire)) {
                // Producer is finished; drain any last items still in flight.
                while (q->try_pop(val)) {
                    if (val != expected) {
                        ordering_ok.store(false, std::memory_order_relaxed);
                    }
                    ++expected;
                    received.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    };

    // ── run ─────────────────────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();

    std::thread prod_thread(producer);
    std::thread cons_thread(consumer);

    prod_thread.join();
    cons_thread.join();

    auto t1 = std::chrono::steady_clock::now();

    // ── results ─────────────────────────────────────────────────────────────
    double elapsed_s =
        std::chrono::duration<double>(t1 - t0).count();
    double throughput_mops = static_cast<double>(kMessages) / elapsed_s / 1e6;

    std::uint64_t total_received = received.load(std::memory_order_relaxed);
    bool          count_ok       = (total_received == kMessages);
    bool          ok             = ordering_ok.load(std::memory_order_relaxed) && count_ok;

    std::printf("messages : %llu\n",  (unsigned long long)kMessages);
    std::printf("received : %llu\n",  (unsigned long long)total_received);
    std::printf("ordering : %s\n",    ok ? "OK" : "FAIL");
    std::printf("time     : %.3f s\n", elapsed_s);
    std::printf("throughput: %.2f Mops/s\n", throughput_mops);

    delete q;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
