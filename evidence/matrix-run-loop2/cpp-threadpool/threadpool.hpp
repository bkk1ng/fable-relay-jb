#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
    // num_threads: worker count. max_queue: backpressure limit — enqueue blocks
    // when this many tasks are already waiting.
    explicit ThreadPool(std::size_t num_threads, std::size_t max_queue = 256)
        : max_queue_size_(max_queue), stop_(false)
    {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Graceful shutdown: signals workers to stop, then joins only after the
    // task queue drains completely.
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_consumer_.notify_all(); // wake workers so they can see stop_
        cv_producer_.notify_all(); // wake any enqueue callers blocked on backpressure
        for (auto& t : workers_) {
            t.join();
        }
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    // Enqueue a callable with arguments; returns a future for the result.
    // Blocks the calling thread when the queue is at capacity (backpressure).
    // Throws std::runtime_error if the pool has already been stopped.
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        // Bind the callable and all arguments now, while their types and
        // lifetimes are still valid, using std::apply to avoid the
        // pack-capture restriction in C++17 lambdas.
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            }
        );

        std::future<ReturnType> fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Backpressure: wait until there is room or the pool is stopping.
            cv_producer_.wait(lock, [this] {
                return stop_ || queue_.size() < max_queue_size_;
            });

            if (stop_) {
                throw std::runtime_error("enqueue on a stopped ThreadPool");
            }

            queue_.emplace([task]() { (*task)(); });
        }

        cv_consumer_.notify_one();
        return fut;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                // Sleep until there is work, or we are told to stop.
                cv_consumer_.wait(lock, [this] {
                    return stop_ || !queue_.empty();
                });

                // Drain: finish remaining tasks even after stop_ is set.
                // Only exit once the queue is fully empty.
                if (stop_ && queue_.empty()) {
                    return;
                }

                task = std::move(queue_.front());
                queue_.pop();
            }

            // Notify a blocked enqueue caller that a slot opened up.
            cv_producer_.notify_one();

            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mutex_;
    std::condition_variable           cv_consumer_; // workers wait here
    std::condition_variable           cv_producer_; // enqueuers wait here
    const std::size_t                 max_queue_size_;
    bool                              stop_;
};
