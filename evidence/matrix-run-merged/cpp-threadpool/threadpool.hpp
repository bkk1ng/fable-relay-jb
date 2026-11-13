#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count, std::size_t max_queue_size = 0)
        : max_queue_size_(max_queue_size), stop_(false), drained_(false)
    {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() { shutdown(); }

    // Enqueue a callable and return a future for its result.
    // Blocks the caller (backpressure) when the queue is at capacity.
    // Throws std::runtime_error if the pool has been shut down.
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<R()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        std::future<R> fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Backpressure: wait until there is room or the pool is stopping.
            if (max_queue_size_ > 0) {
                not_full_.wait(lock, [this] {
                    return stop_ || tasks_.size() < max_queue_size_;
                });
            }

            if (stop_) {
                throw std::runtime_error("ThreadPool: enqueue called after shutdown");
            }

            tasks_.emplace([task]() { (*task)(); });
        }

        not_empty_.notify_one();
        return fut;
    }

    // Graceful shutdown: waits for all queued tasks to complete before
    // joining workers. Safe to call multiple times.
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (drained_) return;   // already shut down
            stop_ = true;
        }
        // Wake everyone so workers and blocked enqueuers can exit.
        not_empty_.notify_all();
        not_full_.notify_all();

        for (std::thread& w : workers_) {
            if (w.joinable()) w.join();
        }

        std::unique_lock<std::mutex> lock(mutex_);
        drained_ = true;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Wait for work; keep draining even after stop_ is set.
                not_empty_.wait(lock, [this] {
                    return !tasks_.empty() || stop_;
                });

                if (tasks_.empty()) {
                    // stop_ is true and no work left — exit.
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // Notify a potentially blocked enqueuer that a slot opened.
            not_full_.notify_one();

            task();
        }
    }

    std::size_t                        max_queue_size_;
    bool                               stop_;
    bool                               drained_;
    std::mutex                         mutex_;
    std::condition_variable            not_empty_;
    std::condition_variable            not_full_;
    std::queue<std::function<void()>>  tasks_;
    std::vector<std::thread>           workers_;
};
