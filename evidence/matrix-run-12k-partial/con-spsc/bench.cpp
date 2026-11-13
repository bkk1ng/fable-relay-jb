// bench.cpp — SPSCQueue throughput benchmark.
//
// Builds with any C++17 compiler:
//   g++ -std=c++17 -O2 -pthread -o bench bench.cpp
//   clang++ -std=c++17 -O2 -pthread -o bench bench.cpp
//   cl /std:c++17 /EHsc /O2 bench.cpp      (MSVC: no -pthread needed)
//
// Runs:
//   ./bench            (uses default 10M elements)
//   ./bench 50000000   (push 50 million instead)

#include "spsc.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    // Default: 10 million uint64_t values.
    const std::uint64_t count =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000ULL;

    // Capacity must exceed what the producer can outrun the consumer by, but
    // since the consumer keeps up closely a modest buffer is fine. 2x a
    // generous chunk keeps push retries rare while staying cache-friendly.
    const std::size_t capacity = 1 << 20; // 1 Mi slots, power of two
    SPSCQueue<std::uint64_t> queue(capacity);

    std::atomic<bool> producer_done{false};
    std::atomic<bool> order_ok{true};

    // ---- Consumer thread -------------------------------------------------
    // Pops every value, verifies strict FIFO order, counts throughput.
    auto consumer = [&]() {
        std::uint64_t expected = 0;
        std::uint64_t value    = 0;

        while (expected < count) {
            if (queue.try_pop(value)) {
                if (value != expected) {
                    order_ok.store(false, std::memory_order_relaxed);
                    std::cerr << "ORDER VIOLATION at index " << expected
                              << ": got " << value << '\n';
                    // Drain remaining to let the producer finish.
                    while (queue.try_pop(value)) {}
                    return;
                }
                ++expected;
            } else {
                // Queue empty but producer still running — spin.
                if (producer_done.load(std::memory_order_relaxed) &&
                    queue.empty()) {
                    std::cerr << "UNDERFLOW: consumer saw only "
                              << expected << " of " << count << '\n';
                    order_ok.store(false, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }
        }
    };

    // ---- Producer thread -------------------------------------------------
    // Pushes 0..count-1, spinning if the queue is full.
    auto producer = [&]() {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    };

    const auto start = std::chrono::steady_clock::now();

    std::thread prod(producer);
    std::thread cons(consumer);
    prod.join();
    cons.join();

    const auto end  = std::chrono::steady_clock::now();
    const double secs =
        std::chrono::duration<double>(end - start).count();
    const double elems_per_sec =
        (secs > 0.0) ? static_cast<double>(count) / secs : 0.0;
    const double bytes_per_sec = elems_per_sec * sizeof(std::uint64_t);

    std::cout << "elements:        " << count           << '\n';
    std::cout << "time:            " << secs            << " s\n";
    std::cout << "throughput:      " << elems_per_sec   << " ops/s\n";
    std::cout << "bandwidth:       " << bytes_per_sec   << " B/s\n";
    std::cout << "ordering check:  "
              << (order_ok.load() ? "PASS" : "FAIL")   << '\n';

    return order_ok.load() ? EXIT_SUCCESS : EXIT_FAILURE;
}
