#pragma once

// signals.hpp — header-only, thread-safe C++17 signal/slot library
//
// API
//   Signal<Args...>      emittable signal; non-copyable, movable
//   Connection           move-only handle returned by Signal::connect()
//   ScopedConnection     RAII wrapper that disconnects on destruction
//
// Thread-safety
//   Signal::connect, disconnect, and emit may be called from any thread.
//   Copy-on-emit: the slot list is snapshotted under a mutex; slots are
//   invoked outside the lock so emitting re-entrantly or connecting from
//   within a slot is safe.
//   Connection handles themselves are NOT shared across threads; each
//   thread should own its own Connection objects.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace signals {

// ─── forward declaration ────────────────────────────────────────────────────
template <typename... Args>
class Signal;

// ─── Connection ─────────────────────────────────────────────────────────────
// Lightweight move-only handle. Calling disconnect() removes the slot from
// its signal. If the signal was already destroyed, disconnect() is a no-op.
class Connection {
public:
    Connection() = default;

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : disconnect_fn_(std::move(other.disconnect_fn_))
        , connected_fn_(std::move(other.connected_fn_))
    {}

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            disconnect_fn_ = std::move(other.disconnect_fn_);
            connected_fn_  = std::move(other.connected_fn_);
        }
        return *this;
    }

    // Remove the associated slot from its signal (idempotent).
    void disconnect() noexcept {
        if (disconnect_fn_) {
            disconnect_fn_();
            disconnect_fn_ = nullptr;
            connected_fn_  = nullptr;
        }
    }

    // Returns false if already disconnected or if the signal was destroyed.
    [[nodiscard]] bool connected() const noexcept {
        return connected_fn_ && connected_fn_();
    }

private:
    template <typename... Args>
    friend class Signal;

    std::function<void()> disconnect_fn_;
    std::function<bool()> connected_fn_;
};

// ─── ScopedConnection ────────────────────────────────────────────────────────
// RAII wrapper: disconnects automatically when it goes out of scope.
// Move-only. Use release() to transfer ownership without disconnecting.
class ScopedConnection {
public:
    ScopedConnection() = default;

    explicit ScopedConnection(Connection conn) noexcept
        : conn_(std::move(conn)) {}

    ScopedConnection(const ScopedConnection&)            = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&& other) noexcept
        : conn_(std::move(other.conn_)) {}

    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
        if (this != &other) {
            conn_.disconnect();
            conn_ = std::move(other.conn_);
        }
        return *this;
    }

    ~ScopedConnection() { conn_.disconnect(); }

    void disconnect() noexcept { conn_.disconnect(); }

    [[nodiscard]] bool connected() const noexcept { return conn_.connected(); }

    // Transfer ownership without triggering disconnect.
    Connection release() noexcept { return std::move(conn_); }

private:
    Connection conn_;
};

// ─── internal shared state ───────────────────────────────────────────────────
namespace detail {

template <typename... Args>
class SignalState {
public:
    using SlotFn = std::function<void(Args...)>;

    // Add a slot; returns its unique id.
    std::uint64_t add(SlotFn fn) {
        const std::uint64_t id =
            next_id_.fetch_add(1u, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mutex_);
        slots_.push_back({id, std::move(fn)});
        return id;
    }

    // Remove the slot with the given id (no-op if already removed).
    void remove(std::uint64_t id) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(slots_.begin(), slots_.end(),
                               [id](const Slot& s) noexcept {
                                   return s.id == id;
                               });
        if (it != slots_.end()) slots_.erase(it);
    }

    // Returns true if a slot with this id is still registered.
    bool has(std::uint64_t id) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return std::any_of(slots_.begin(), slots_.end(),
                           [id](const Slot& s) noexcept {
                               return s.id == id;
                           });
    }

    // Copy-on-emit: snapshot under lock, invoke without lock.
    void fire(const Args&... args) {
        std::vector<Slot> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snapshot = slots_; // copy
        }
        for (const auto& slot : snapshot) {
            slot.fn(args...);
        }
    }

private:
    struct Slot {
        std::uint64_t id;
        SlotFn        fn;
    };

    mutable std::mutex            mutex_;
    std::vector<Slot>             slots_;
    std::atomic<std::uint64_t>    next_id_{1u};
};

} // namespace detail

// ─── Signal ─────────────────────────────────────────────────────────────────
template <typename... Args>
class Signal {
public:
    using slot_fn = std::function<void(Args...)>;

    Signal() : state_(std::make_shared<detail::SignalState<Args...>>()) {}

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;

    Signal(Signal&&) noexcept            = default;
    Signal& operator=(Signal&&) noexcept = default;

    // Register a slot. Returns a Connection that can be used to unsubscribe.
    [[nodiscard]] Connection connect(slot_fn fn) {
        const std::uint64_t id = state_->add(std::move(fn));

        // Capture a weak_ptr so the lambda does not keep the signal alive.
        std::weak_ptr<detail::SignalState<Args...>> weak = state_;

        Connection conn;
        conn.disconnect_fn_ = [weak, id]() noexcept {
            if (auto s = weak.lock()) s->remove(id);
        };
        conn.connected_fn_ = [weak, id]() noexcept -> bool {
            auto s = weak.lock();
            return s && s->has(id);
        };
        return conn;
    }

    // Invoke all currently registered slots with copy-on-emit semantics.
    void emit(const Args&... args) {
        state_->fire(args...);
    }

private:
    std::shared_ptr<detail::SignalState<Args...>> state_;
};

} // namespace signals
