// bench.cpp — SPSC queue throughput benchmark
//
// One thread pushes 10 million uint64_t values (0..N-1) into the queue;
// another thread pops them and verifies strict FIFO ordering.  Reports
// elapsed wall-clock time and throughput in millions of ops/s.
//
// Build:  g++ -std=c++17 -O2 -pthread bench.cpp -o bench
//         clang++ -std=c++17 -O2 -pthread bench.cpp -o bench
// Run:    ./bench

#include "spsc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
    constexpr std::uint64_t N   = 10'000'000;  // total elements to push
    constexpr std::size_t   Cap = 1 << 16;     // 65 536 slots

    spsc::SpscQueue<std::uint64_t, Cap> q;

    auto t0 = std::chrono::steady_clock::now();

    // Consumer: pop every value and check it arrives in sequence.
    std::thread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t value;
        while (expected < N) {
            if (q.try_pop(value)) {
                if (value != expected) {
                    std::cerr << "ORDER VIOLATION: expected "
                              << expected << ", got " << value << '\n';
                    std::exit(1);
                }
                ++expected;
            }
        }
    });

    // Producer: push 0..N-1, spinning when the queue is full.
    for (std::uint64_t i = 0; i < N; ++i)
        while (!q.try_push(i))
            ;

    consumer.join();

    auto t1  = std::chrono::steady_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double secs = static_cast<double>(ns) / 1e9;
    double mops = static_cast<double>(N) / secs / 1e6;

    std::cout << "elements   : " << N << '\n';
    std::cout << "capacity   : " << Cap << '\n';
    std::cout << "ordering   : verified (0.." << N - 1
              << " received in sequence)\n";
    std::cout << "elapsed    : " << secs << " s\n";
    std::cout << "throughput : " << mops << " M ops/s\n";
}
