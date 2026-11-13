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

// A fixed-size thread pool with std::future-based task submission.
//
// Backpressure: when max_queue_size > 0, enqueue() blocks the caller until a
// slot becomes available rather than growing the queue without bound.
//
// Graceful shutdown: the destructor (or an explicit shutdown() call) lets every
// task already in the queue finish before joining workers.  New enqueue() calls
// after shutdown() throw std::runtime_error.
class ThreadPool {
public:
    // num_threads     – number of worker threads to spawn.
    // max_queue_size  – maximum number of pending tasks (0 = unbounded).
    explicit ThreadPool(std::size_t num_threads,
                        std::size_t max_queue_size = 0);

    // Calls shutdown(); blocks until all queued tasks have run.
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    // Submit a callable with arguments.  Blocks when the queue is full
    // (backpressure).  Returns a future for the callable's result.
    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>,
                                            std::decay_t<Args>...>>;

    // Signal shutdown and wait for all currently-queued tasks to finish.
    // Idempotent – safe to call multiple times; subsequent calls are no-ops.
    void shutdown();

private:
    void worker_loop();

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  tasks_;
    std::mutex                         mutex_;
    std::condition_variable            worker_cv_;   // wakes workers on new work
    std::condition_variable            producer_cv_; // wakes producers on free slot
    std::size_t                        max_queue_size_;
    bool                               stop_{ false };
};

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

inline ThreadPool::ThreadPool(std::size_t num_threads,
                               std::size_t max_queue_size)
    : max_queue_size_(max_queue_size)
{
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        workers_.emplace_back(&ThreadPool::worker_loop, this);
}

inline ThreadPool::~ThreadPool()
{
    shutdown();
}

inline void ThreadPool::shutdown()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_) return;   // already shut down
        stop_ = true;
    }
    // Unblock everyone: workers waiting for tasks and producers waiting for
    // queue space.
    worker_cv_.notify_all();
    producer_cv_.notify_all();

    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

inline void ThreadPool::worker_loop()
{
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // Sleep until there is work to do or the pool is stopping.
            worker_cv_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });

            // Drain: keep running tasks even after stop_ is set.
            // Only exit once the queue is completely empty.
            if (tasks_.empty())
                break;   // stop_ is true and nothing left to do

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // Notify a blocked producer that a slot has opened up.  Done outside
        // the lock to avoid holding it longer than necessary.
        producer_cv_.notify_one();

        task();
    }
}

template <typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>,
                                        std::decay_t<Args>...>>
{
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // packaged_task is move-only; wrap it in a shared_ptr so the lambda
    // stored in the queue remains copyable (std::function requires that).
    auto ptask = std::make_shared<std::packaged_task<R()>>(
        [f        = std::forward<F>(f),
         arg_pack = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(f), std::move(arg_pack));
        });

    std::future<R> fut = ptask->get_future();

    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (stop_)
            throw std::runtime_error(
                "ThreadPool::enqueue called after shutdown");

        // Backpressure: block until there is room in the queue.
        if (max_queue_size_ > 0) {
            producer_cv_.wait(lock, [this] {
                return tasks_.size() < max_queue_size_ || stop_;
            });

            if (stop_)
                throw std::runtime_error(
                    "ThreadPool::enqueue called after shutdown");
        }

        tasks_.emplace([ptask] { (*ptask)(); });
    }

    worker_cv_.notify_one();
    return fut;
}
