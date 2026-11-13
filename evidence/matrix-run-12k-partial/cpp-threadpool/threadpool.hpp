#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

// Thread pool with bounded queue and backpressure.
//
// - enqueue() returns a std::future for the result.
// - If the queue is full, enqueue() blocks until a slot opens (backpressure).
// - shutdown() / ~ThreadPool() drain all queued tasks before joining workers.
class ThreadPool {
public:
    // num_threads    : worker thread count
    // max_queue_size : maximum pending tasks before enqueue blocks
    explicit ThreadPool(std::size_t num_threads, std::size_t max_queue_size)
        : max_queue_size_{max_queue_size}
    {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Graceful shutdown: drains all queued tasks, then joins every worker.
    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    // Submit a callable with arguments.  Blocks when the queue is at capacity.
    // Throws std::runtime_error if shutdown() has already been called.
    template<typename F, typename... Args>
    [[nodiscard]] auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    // Explicitly initiate shutdown.  Idempotent; safe to call before destruction.
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        // Wake workers so they can drain and exit, and wake blocked producers
        // so they can observe the stop flag and throw.
        cv_worker_.notify_all();
        cv_producer_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void worker_loop()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Wait until there is work to do or a shutdown is in progress.
                cv_worker_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });
                // Graceful drain: keep running tasks until the queue is empty,
                // even after stop_ is set.
                if (stop_ && tasks_.empty()) break;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // Notify a potentially blocked producer now that a slot opened up.
            cv_producer_.notify_one();
            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_worker_;    // signals workers
    std::condition_variable           cv_producer_;  // signals blocked enqueue callers
    std::size_t                       max_queue_size_;
    bool                              stop_{false};
};

// ---------------------------------------------------------------------------
// Template implementation
// ---------------------------------------------------------------------------

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using R = std::invoke_result_t<F, Args...>;

    // Capture the callable and all arguments into a packaged_task.
    // std::make_tuple + std::apply gives C++17-compatible pack capture without
    // relying on the C++20 [...pack = expr] lambda-init-capture extension.
    auto task_ptr = std::make_shared<std::packaged_task<R()>>(
        [fn  = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            return std::apply(std::move(fn), std::move(tup));
        }
    );

    std::future<R> fut = task_ptr->get_future();

    {
        std::unique_lock<std::mutex> lock(mutex_);

        // Backpressure: block the caller until the queue has room or a shutdown
        // is in progress.
        cv_producer_.wait(lock, [this] {
            return stop_ || tasks_.size() < max_queue_size_;
        });

        if (stop_) {
            throw std::runtime_error("ThreadPool::enqueue: pool is shut down");
        }

        // Wrap in a type-erased callable; the shared_ptr keeps the task alive.
        tasks_.emplace([tp = std::move(task_ptr)] { (*tp)(); });
    }

    cv_worker_.notify_one();
    return fut;
}
