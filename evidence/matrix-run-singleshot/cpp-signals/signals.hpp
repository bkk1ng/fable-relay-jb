// signals.hpp — C++17 header-only, thread-safe signal/slot library
//
// Design notes:
//  - Signal<Args...> stores slots identified by a monotonically increasing id.
//  - connect() returns a Connection that holds a weak_ptr to a disconnect token.
//  - Each SlotEntry owns a shared_ptr to that same token; when the entry is erased
//    the shared_ptr is destroyed and the Connection's weak_ptr expires, so
//    connected() naturally returns false without any extra bookkeeping.
//  - emit() snapshots the slot list under the lock (copy-on-emit), then releases
//    the lock before invoking any slot.  This lets slots safely connect/disconnect
//    re-entrantly and lets multiple threads emit concurrently without deadlock.
//  - Signal stores its mutable state in a shared_ptr<Impl> so the disconnect
//    tokens captured in lambdas remain safe even if the Signal is moved.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// ─── Connection ──────────────────────────────────────────────────────────────

class Connection {
public:
    Connection() = default;

    // Removes the associated slot from its Signal (no-op if already disconnected
    // or if the Signal has been destroyed).
    void disconnect() {
        if (auto fn = token_.lock()) {
            (*fn)();
        }
        token_.reset();
    }

    // True while the slot is still registered.
    [[nodiscard]] bool connected() const noexcept { return !token_.expired(); }

private:
    std::weak_ptr<std::function<void()>> token_;

    explicit Connection(std::weak_ptr<std::function<void()>> t)
        : token_(std::move(t)) {}

    template <typename... Args>
    friend class Signal;
};

// ─── ScopedConnection ────────────────────────────────────────────────────────

// RAII wrapper — disconnects on destruction.
class ScopedConnection {
public:
    ScopedConnection() = default;
    explicit ScopedConnection(Connection c) : conn_(std::move(c)) {}

    ~ScopedConnection() { conn_.disconnect(); }

    ScopedConnection(ScopedConnection&&) noexcept            = default;
    ScopedConnection& operator=(ScopedConnection&&) noexcept = default;

    ScopedConnection(const ScopedConnection&)            = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    void disconnect() noexcept { conn_.disconnect(); }

    [[nodiscard]] bool connected() const noexcept { return conn_.connected(); }

    // Relinquish ownership without disconnecting.
    [[nodiscard]] Connection release() noexcept {
        Connection c = std::move(conn_);
        conn_        = Connection{};
        return c;
    }

private:
    Connection conn_;
};

// ─── Signal ──────────────────────────────────────────────────────────────────

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(const Args&...)>;

    Signal() : impl_(std::make_shared<Impl>()) {}

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;

    Signal(Signal&&) noexcept            = default;
    Signal& operator=(Signal&&) noexcept = default;

    ~Signal() = default;

    // Register a slot and return a Connection that can disconnect it.
    [[nodiscard]] Connection connect(Slot slot) {
        std::lock_guard<std::mutex> lk(impl_->mutex);

        const auto id = impl_->next_id++;

        // The token shared_ptr is co-owned by:
        //   • the SlotEntry (keeps it alive while the slot is registered), and
        //   • the Connection / ScopedConnection (held as a weak_ptr).
        // Calling the token function erases the slot entry from Impl, which
        // drops Impl's shared_ptr; the Connection's weak_ptr then expires.
        std::weak_ptr<Impl> weak = impl_;
        auto token = std::make_shared<std::function<void()>>(
            [weak, id] {
                if (auto impl = weak.lock()) {
                    impl->erase(id);
                }
            });

        impl_->slots.push_back({id, std::move(slot), token});
        return Connection{std::weak_ptr<std::function<void()>>(token)};
    }

    // Disconnect every slot at once.
    void disconnect_all() {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        impl_->slots.clear();
    }

    // Invoke all currently connected slots.
    // Thread-safe: multiple threads may call emit() concurrently.
    void emit(const Args&... args) const {
        // Snapshot under the lock so slots can safely connect/disconnect
        // during emission (copy-on-emit).
        std::vector<SlotEntry> local;
        {
            std::lock_guard<std::mutex> lk(impl_->mutex);
            local = impl_->slots;
        }
        for (const auto& entry : local) {
            entry.fn(args...);
        }
    }

    void operator()(const Args&... args) const { emit(args...); }

private:
    struct SlotEntry {
        uint64_t                              id;
        Slot                                  fn;
        std::shared_ptr<std::function<void()>> token;  // keeps Connection's weak_ptr valid
    };

    struct Impl {
        std::mutex            mutex;
        std::vector<SlotEntry> slots;
        uint64_t               next_id = 0;

        void erase(uint64_t id) {
            std::lock_guard<std::mutex> lk(mutex);
            slots.erase(
                std::remove_if(slots.begin(), slots.end(),
                               [id](const SlotEntry& e) { return e.id == id; }),
                slots.end());
        }
    };

    std::shared_ptr<Impl> impl_;
};
