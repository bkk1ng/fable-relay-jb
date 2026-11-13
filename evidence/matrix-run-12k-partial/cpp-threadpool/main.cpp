#include "threadpool.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    // 4 workers, queue capped at 64 to demonstrate backpressure.
    ThreadPool pool(4, 64);

    constexpr int NUM_TASKS = 1000;
    std::vector<std::future<int>> futures;
    futures.reserve(NUM_TASKS);

    for (int i = 1; i <= NUM_TASKS; ++i) {
        // enqueue blocks when the queue is full (64 pending tasks).
        futures.push_back(pool.enqueue([i]() -> int {
            return i;
        }));
    }

    // Drain futures and accumulate results.
    long long sum = 0;
    for (auto& f : futures) {
        sum += f.get();
    }

    // Expected: 1 + 2 + ... + 1000 = 500500
    std::cout << "Sum of tasks 1..1000 = " << sum << "\n";

    // pool destructor calls shutdown(), which drains the queue and joins workers.
    return 0;
}
