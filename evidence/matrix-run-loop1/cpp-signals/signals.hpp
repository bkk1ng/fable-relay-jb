#pragma once

// signals.hpp — C++17 header-only, thread-safe signal/slot library
//
// Usage:
//   Signal<int, std::string> sig;
//   auto conn = sig.connect([](int x, std::string s){ ... });
//   sig.emit(42, "hello");
//   conn.disconnect();
//
// Thread safety:
//   - connect/disconnect and emit may be called from any thread concurrently.
//   - emit takes a snapshot of the slot list (copy-on-emit), so slots added or
//     removed during emission take effect on the NEXT emit, never the current one.
//   - Slots are invoked outside the mutex, so slots may safely call connect or
//     disconnect on the same signal without deadlocking.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace sig {

namespace detail {

// Monotonically increasing connection ID.
using ConnId = std::uint64_t;

inline ConnId next_id() noexcept {
    static std::atomic<ConnId> counter{0};
    return ++counter;
}

// Forward declaration so Connection can hold a back-pointer.
template <typename... Args>
class SignalBase;

} // namespace detail

// ---------------------------------------------------------------------------
// Connection — returned by Signal::connect().
// Calling disconnect() (or letting the Connection fall out of scope via
// ScopedConnection) removes the slot from the signal.
// ---------------------------------------------------------------------------
class Connection {
public:
    Connection() noexcept = default;

    // Non-copyable, movable.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;

    void disconnect() {
        if (auto fn = disconnect_fn_.lock()) {
            (*fn)();
            disconnect_fn_.reset();
        }
    }

    [[nodiscard]] bool connected() const noexcept {
        return !disconnect_fn_.expired();
    }

private:
    template <typename... Args>
    friend class Signal;

    // The disconnect function is owned by the Signal; we hold a weak_ptr so
    // that if the Signal is destroyed before this Connection, disconnect() is
    // a harmless no-op.
    explicit Connection(std::shared_ptr<std::function<void()>> fn) noexcept
        : disconnect_fn_(fn) {}

    std::weak_ptr<std::function<void()>> disconnect_fn_;
};

// ---------------------------------------------------------------------------
// ScopedConnection — RAII wrapper; disconnects on destruction.
// ---------------------------------------------------------------------------
class ScopedConnection {
public:
    ScopedConnection() noexcept = default;

    /*implicit*/ ScopedConnection(Connection conn) noexcept
        : conn_(std::move(conn)) {}

    // Non-copyable, movable.
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&&) noexcept = default;
    ScopedConnection& operator=(ScopedConnection&&) noexcept = default;

    ~ScopedConnection() { conn_.disconnect(); }

    void disconnect() { conn_.disconnect(); }
    [[nodiscard]] bool connected() const noexcept { return conn_.connected(); }

    // Release ownership without disconnecting.
    [[nodiscard]] Connection release() noexcept { return std::move(conn_); }

private:
    Connection conn_;
};

// ---------------------------------------------------------------------------
// Signal<Args...>
// ---------------------------------------------------------------------------
template <typename... Args>
class Signal {
public:
    using SlotFn = std::function<void(Args...)>;

    Signal() = default;

    // Non-copyable (copying a signal with live slots is semantically ambiguous).
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    // Movable.
    Signal(Signal&&) noexcept = default;
    Signal& operator=(Signal&&) noexcept = default;

    ~Signal() = default;

    // Connect a slot. Returns a Connection that can be used to disconnect.
    // O(n) copy of the slot list (copy-on-write).
    [[nodiscard]] Connection connect(SlotFn slot) {
        auto id = detail::next_id();

        // Build the disconnect callable and hand a shared_ptr of it to the
        // Connection; we keep a weak_ptr in the slot entry so we can drop it
        // when the slot is erased.
        auto disconnect_fn = std::make_shared<std::function<void()>>();
        *disconnect_fn = [this, id]() { this->disconnect(id); };

        {
            std::lock_guard lock(mutex_);
            // Copy-on-write: replace the shared slot list with a new one.
            auto new_slots = std::make_shared<SlotList>(*current_slots_());
            new_slots->push_back({id, std::move(slot), disconnect_fn});
            slots_ = std::move(new_slots);
        }

        return Connection(disconnect_fn);
    }

    // Emit the signal. Takes a snapshot of the slot list under the lock, then
    // invokes each slot outside the lock.
    void emit(Args... args) const {
        // Snapshot — increments the shared_ptr ref-count, cheap.
        std::shared_ptr<const SlotList> snapshot;
        {
            std::lock_guard lock(mutex_);
            snapshot = slots_;
        }
        if (!snapshot) return;
        for (auto& entry : *snapshot) {
            if (entry.fn) {
                entry.fn(args...);
            }
        }
    }

    // Convenience: operator() as an alias for emit.
    void operator()(Args... args) const { emit(std::forward<Args>(args)...); }

    // Number of currently connected slots.
    [[nodiscard]] std::size_t slot_count() const noexcept {
        std::lock_guard lock(mutex_);
        return slots_ ? slots_->size() : 0u;
    }

    // Disconnect all slots at once.
    void disconnect_all() {
        std::lock_guard lock(mutex_);
        slots_ = std::make_shared<SlotList>();
    }

private:
    struct SlotEntry {
        detail::ConnId                        id;
        SlotFn                                fn;
        // Keeping the shared_ptr alive here means Connection::connected()
        // returns true as long as the slot is in the list.
        std::shared_ptr<std::function<void()>> disconnect_guard;
    };

    using SlotList = std::vector<SlotEntry>;

    // Erase by id. Called from the disconnect closure stored in Connection.
    void disconnect(detail::ConnId id) {
        std::lock_guard lock(mutex_);
        auto& current = *current_slots_();
        auto new_slots = std::make_shared<SlotList>();
        new_slots->reserve(current.size());
        for (auto& e : current) {
            if (e.id != id) new_slots->push_back(e);
        }
        slots_ = std::move(new_slots);
    }

    // Always-valid accessor: initialises slots_ on first use.
    std::shared_ptr<SlotList>& current_slots_() {
        if (!slots_) slots_ = std::make_shared<SlotList>();
        return slots_;
    }

    mutable std::mutex              mutex_;
    std::shared_ptr<SlotList>       slots_;
};

} // namespace sig
