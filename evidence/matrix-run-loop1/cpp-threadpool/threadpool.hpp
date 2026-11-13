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
    // max_queue_size: maximum number of pending tasks before enqueue blocks (0 = unlimited)
    explicit ThreadPool(std::size_t num_threads, std::size_t max_queue_size = 0)
        : max_queue_size_(max_queue_size), stop_(false), drain_(false)
    {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() {
        shutdown();
    }

    // Enqueue a callable with arguments; returns a future for the result.
    // Blocks if the queue is at capacity (backpressure) until a slot opens.
    // Throws std::runtime_error if the pool is shutting down.
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            }
        );

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Backpressure: wait while the queue is full
            producer_cv_.wait(lock, [this] {
                return stop_ || drain_ ||
                       max_queue_size_ == 0 ||
                       tasks_.size() < max_queue_size_;
            });

            if (stop_ || drain_) {
                throw std::runtime_error("ThreadPool: enqueue on stopped/draining pool");
            }

            tasks_.emplace([task]() { (*task)(); });
        }

        consumer_cv_.notify_one();
        return result;
    }

    // Graceful shutdown: stop accepting new tasks, drain remaining queued work,
    // then join all worker threads.
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;   // already shut down
            drain_ = true;       // signal: drain but keep processing queued tasks
        }
        consumer_cv_.notify_all();
        producer_cv_.notify_all();

        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                consumer_cv_.wait(lock, [this] {
                    return stop_ || !tasks_.empty() || drain_;
                });

                // Drain mode: keep going until the queue is empty
                if (tasks_.empty()) {
                    // Queue empty + drain or stop => exit
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // Notify producers that a slot opened up
            producer_cv_.notify_one();

            task();
        }
    }

    std::size_t                        max_queue_size_;
    bool                               stop_;
    bool                               drain_;

    std::mutex                         mutex_;
    std::condition_variable            consumer_cv_;   // wakes workers
    std::condition_variable            producer_cv_;   // wakes blocked enqueuers

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  tasks_;
};
