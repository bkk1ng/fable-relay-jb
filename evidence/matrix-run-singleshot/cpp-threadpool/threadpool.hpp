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
    explicit ThreadPool(std::size_t thread_count, std::size_t max_queue_size)
        : max_queue_size_(max_queue_size), stop_(false), draining_(false)
    {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    ~ThreadPool() { shutdown(); }

    // Enqueue a callable; blocks (backpressure) when the queue is full.
    // Returns std::future<> for the result. Throws if the pool is shutting down.
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<R()>>(
            [func = std::forward<F>(f),
             targs = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(targs));
            });

        std::future<R> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Backpressure: wait until there is room or the pool is stopping
            not_full_.wait(lock, [this] {
                return tasks_.size() < max_queue_size_ || stop_;
            });

            if (stop_) {
                throw std::runtime_error("ThreadPool: enqueue on stopped pool");
            }

            tasks_.emplace([task = std::move(task)]() { (*task)(); });
        }

        not_empty_.notify_one();
        return result;
    }

    // Signal workers to stop after draining all queued work, then join.
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;   // already shut down
            draining_ = true;
        }
        not_empty_.notify_all();

        // Wait for the queue to drain before setting stop_
        {
            std::unique_lock<std::mutex> lock(mutex_);
            drained_.wait(lock, [this] { return tasks_.empty(); });
            stop_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();

        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                not_empty_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) return;
                if (tasks_.empty()) continue;

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // Notify producers that a slot opened up
            not_full_.notify_one();

            task();

            // After popping, check if the queue is now empty while draining
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (draining_ && tasks_.empty()) {
                    drained_.notify_all();
                }
            }
        }
    }

    const std::size_t              max_queue_size_;
    bool                           stop_;
    bool                           draining_;
    std::mutex                     mutex_;
    std::condition_variable        not_empty_;
    std::condition_variable        not_full_;
    std::condition_variable        drained_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread>       workers_;
};
