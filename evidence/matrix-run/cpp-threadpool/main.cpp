#include "threadpool.hpp"

#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

int main() {
    // Hardware concurrency, with a sensible fallback.
    const std::size_t num_threads = std::thread::hardware_concurrency() > 0
                                        ? std::thread::hardware_concurrency()
                                        : 4;

    // Allow at most 2× the thread count in queued tasks to cap peak memory
    // usage; producers block when the queue is full (backpressure).
    const std::size_t max_queue = num_threads * 2;

    ThreadPool pool(num_threads, max_queue);

    constexpr int NUM_TASKS = 1000;
    std::vector<std::future<int>> futures;
    futures.reserve(NUM_TASKS);

    for (int i = 1; i <= NUM_TASKS; ++i) {
        // Each task captures its index and returns it; enqueue() blocks if
        // the queue is full, so this loop naturally throttles.
        futures.push_back(pool.enqueue([i]() { return i; }));
    }

    // Accumulate results.  get() blocks until the task finishes.
    std::int64_t sum = 0;
    for (auto& f : futures)
        sum += f.get();

    // Expected: 1 + 2 + … + 1000 = 500500
    std::cout << "Sum of tasks 1..1000: " << sum << "\n";

    // pool destructor calls shutdown(), draining any remaining tasks before
    // joining all worker threads.
    return 0;
}
