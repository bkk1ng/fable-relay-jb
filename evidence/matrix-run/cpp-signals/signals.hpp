#pragma once

// signals.hpp — header-only, C++17, thread-safe signal/slot library
//
// Copy-on-emit strategy: emit() snapshots the slot list under a lock,
// then iterates the snapshot without holding the lock. connect() and
// disconnect() always replace (never mutate) the shared slot vector,
// so an in-flight emit iteration is never invalidated.
//
// Build: any C++17-capable compiler; link with -pthread on Linux/macOS.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace signals {

// ── internal ──────────────────────────────────────────────────────────────────

namespace detail {

using SlotId = std::uint64_t;

/// Monotonically increasing, process-unique slot identifier.
inline SlotId next_id() noexcept {
    static std::atomic<SlotId> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace detail

// ── Connection ────────────────────────────────────────────────────────────────

/// Lightweight handle returned by Signal::connect().
/// Calling disconnect() unregisters the slot; subsequent calls are no-ops.
/// A default-constructed Connection is already in the disconnected state.
class Connection {
public:
    Connection() noexcept = default;

    /// Unregister the slot.  Safe to call multiple times and from any thread.
    void disconnect() {
        if (auto fn = disconnect_fn_.lock()) {
            (*fn)(id_);
            disconnect_fn_.reset(); // make subsequent calls cheap no-ops
        }
    }

    /// Returns false once disconnected or once the owning Signal is destroyed.
    [[nodiscard]] bool connected() const noexcept {
        return !disconnect_fn_.expired();
    }

private:
    template<typename...>
    friend class Signal;

    // The callable that removes this id from the slot list.
    // Owned by Signal; shared among all Connections for that Signal instance.
    using DisconnectFn = std::function<void(detail::SlotId)>;

    Connection(detail::SlotId id, std::weak_ptr<DisconnectFn> fn) noexcept
        : id_(id), disconnect_fn_(std::move(fn)) {}

    detail::SlotId id_{0};
    std::weak_ptr<DisconnectFn> disconnect_fn_;
};

// ── ScopedConnection ──────────────────────────────────────────────────────────

/// RAII wrapper: disconnects automatically on destruction.
/// Non-copyable, movable.
class ScopedConnection {
public:
    ScopedConnection() noexcept = default;

    explicit ScopedConnection(Connection c) noexcept : conn_(std::move(c)) {}

    ~ScopedConnection() { conn_.disconnect(); }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&& o) noexcept : conn_(std::move(o.conn_)) {}

    ScopedConnection& operator=(ScopedConnection&& o) noexcept {
        if (this != &o) {
            conn_.disconnect();
            conn_ = std::move(o.conn_);
        }
        return *this;
    }

    void disconnect()                       { conn_.disconnect(); }
    [[nodiscard]] bool connected() const noexcept { return conn_.connected(); }

private:
    Connection conn_;
};

// ── Signal ────────────────────────────────────────────────────────────────────

/// Thread-safe signal.  Multiple threads may call emit(), connect(), and
/// disconnect() concurrently without external synchronisation.
///
/// emit() passes its arguments by value to each slot, so every slot in a
/// given emit call receives the same values regardless of what other slots do.
///
/// Non-copyable and non-movable: Connection objects hold a weak reference
/// into this instance's internals and must not be silently invalidated.
template<typename... Args>
class Signal {
    using Fn  = std::function<void(Args...)>;

    struct Slot {
        detail::SlotId id;
        Fn             fn;
    };

    using SlotVec = std::vector<Slot>;

    // Shared mutable state.  Accessed only under mutex.
    struct State {
        mutable std::mutex             mutex;
        // Always replaced, never mutated; emit() holds its own shared_ptr.
        std::shared_ptr<const SlotVec> slots{std::make_shared<SlotVec>()};
    };

    // state_ must be declared before disconnect_fn_ so NSDMI order is correct.
    std::shared_ptr<State> state_{std::make_shared<State>()};

    // The erase-by-id callable shared with every Connection for this Signal.
    // When Signal is destroyed, this shared_ptr is released; all Connection
    // weak_ptrs expire and connected() correctly returns false.
    std::shared_ptr<Connection::DisconnectFn> disconnect_fn_{
        std::make_shared<Connection::DisconnectFn>(
            [weak = std::weak_ptr<State>(state_)](detail::SlotId id) {
                auto s = weak.lock();
                if (!s) return; // signal already destroyed
                std::lock_guard<std::mutex> lk(s->mutex);
                // Copy-on-write: build a new vector without 'id', then swap.
                auto next = std::make_shared<SlotVec>(*s->slots);
                next->erase(
                    std::remove_if(next->begin(), next->end(),
                        [id](const Slot& slot) { return slot.id == id; }),
                    next->end());
                s->slots = std::move(next);
            })};

public:
    Signal()  = default;
    ~Signal() = default;

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&)                 = delete;
    Signal& operator=(Signal&&)      = delete;

    /// Register a slot.  Returns a Connection handle for later disconnection.
    [[nodiscard]] Connection connect(Fn fn) {
        const auto id = detail::next_id();
        {
            std::lock_guard<std::mutex> lk(state_->mutex);
            auto next = std::make_shared<SlotVec>(*state_->slots);
            next->push_back({id, std::move(fn)});
            state_->slots = std::move(next);
        }
        return Connection{id, disconnect_fn_};
    }

    /// Convenience overload: disconnect via a Connection handle.
    void disconnect(Connection& c) { c.disconnect(); }

    /// Invoke all currently registered slots with copies of the arguments.
    /// Safe to call concurrently with other emit(), connect(), or disconnect()
    /// calls.  A slot that calls disconnect() on itself or a sibling during
    /// emit will not affect the in-flight snapshot.
    void emit(Args... args) const {
        // Grab an immutable snapshot while holding the lock, then drop it.
        std::shared_ptr<const SlotVec> snapshot;
        {
            std::lock_guard<std::mutex> lk(state_->mutex);
            snapshot = state_->slots;
        }
        // Iterate without the lock; snapshot is immutable.
        for (const auto& slot : *snapshot) {
            slot.fn(args...); // pass by value — no forwarding-in-loop hazard
        }
    }

    /// Syntactic sugar: sig(args...) == sig.emit(args...).
    void operator()(Args... args) const { emit(std::forward<Args>(args)...); }
};

} // namespace signals
