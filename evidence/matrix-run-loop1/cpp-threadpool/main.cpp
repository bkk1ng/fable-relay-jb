#include "threadpool.hpp"

#include <iostream>
#include <future>
#include <vector>

int main() {
    // 4 worker threads, bounded queue of 128 pending tasks (backpressure).
    const std::size_t num_threads = std::max(2u, std::thread::hardware_concurrency());
    ThreadPool pool(num_threads, 128);

    // Submit 1000 tasks; each returns its index. Collect the futures so we
    // can sum every result once all work has finished.
    std::vector<std::future<long long>> futures;
    futures.reserve(1000);

    for (int i = 0; i < 1000; ++i) {
        futures.push_back(pool.enqueue([i] {
            // Simulate a tiny unit of work so the pool actually schedules
            // across threads instead of finishing inline.
            volatile long long acc = 0;
            for (int j = 0; j < 10'000; ++j) acc += j;
            return static_cast<long long>(i) + acc - acc; // keep compiler honest
        }));
    }

    // Sum every result. get() blocks until the individual task is done.
    long long sum = 0;
    for (auto& f : futures) {
        sum += f.get();
    }

    // 0 + 1 + ... + 999 = 499500
    std::cout << "sum of 1000 task results = " << sum << '\n';
    std::cout << "expected:               " << (999LL * 1000 / 2) << '\n';

    // Destructor calls shutdown(): drains any remaining queued tasks, joins workers.
    return 0;
}
