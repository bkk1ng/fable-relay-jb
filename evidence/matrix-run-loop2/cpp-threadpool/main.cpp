#include "threadpool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

int main() {
    // Hardware concurrency, with a sensible fallback.
    const std::size_t num_threads = std::thread::hardware_concurrency() > 0
                                      ? std::thread::hardware_concurrency()
                                      : 4;

    // Max queue of 128 means at most 128 tasks sit waiting before enqueue
    // blocks; this exercises the backpressure path with 1000 submissions.
    ThreadPool pool(num_threads, 128);

    constexpr int kTaskCount = 1000;
    std::vector<std::future<int>> futures;
    futures.reserve(kTaskCount);

    for (int i = 1; i <= kTaskCount; ++i) {
        // Capture i by value; each task returns its own index.
        futures.push_back(pool.enqueue([i]() -> int { return i; }));
    }

    // Collect results. futures[i].get() blocks until that task completes,
    // so the loop drains all in-flight work before the pool destructor runs.
    int64_t sum = 0;
    for (auto& f : futures) {
        sum += f.get();
    }

    // Expected: 1 + 2 + ... + 1000 = 500500
    std::cout << "Sum of 1.." << kTaskCount << " = " << sum << '\n';

    // Pool destructor signals shutdown and joins only after the queue drains.
    return 0;
}
