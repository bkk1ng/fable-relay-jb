// signals.hpp — C++17 header-only, thread-safe signal/slot library
//
// Usage:
//   sig::Signal<int, std::string> on_event;
//
//   auto conn  = on_event.connect([](int n, std::string s){ ... });
//   auto scoped = sig::ScopedConnection{ on_event.connect([](int n, std::string){}) };
//
//   on_event.emit(42, "hello");   // thread-safe from any thread
//   conn.disconnect();            // explicit removal
//   // scoped disconnects automatically at end of scope
//
// Thread-safety contract
//   • emit() copies the slot list under the lock, then calls slots
//     without holding it — re-entrant safe and deadlock-free.
//   • connect/disconnect/emit may be called concurrently from any thread.
//   • A single Connection object must NOT be shared across threads
//     (owns its own connected_ flag without synchronisation).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sig {

// Forward declaration so Connection can friend all specialisations.
template <typename...>
class Signal;

// ── Connection ────────────────────────────────────────────────────────────────
//
// Returned by Signal::connect(). Move-only.
// Calling disconnect() removes the slot; a no-op if already disconnected or
// if the originating Signal has been destroyed.
class Connection {
public:
    Connection() = default;

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& o) noexcept
        : eraser_(std::move(o.eraser_))
        , id_(o.id_)
        , connected_(o.connected_)
    {
        o.connected_ = false;
    }

    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            disconnect();              // drop any existing connection first
            eraser_      = std::move(o.eraser_);
            id_          = o.id_;
            connected_   = o.connected_;
            o.connected_ = false;
        }
        return *this;
    }

    ~Connection() = default;   // does NOT auto-disconnect — use ScopedConnection

    void disconnect() noexcept {
        if (connected_) {
            if (eraser_) eraser_(id_);
            connected_ = false;
        }
    }

    [[nodiscard]] bool connected() const noexcept { return connected_; }

private:
    // Only Signal<Args...> constructs non-empty Connections.
    template <typename...>
    friend class Signal;

    using EraserFn = std::function<void(uint64_t)>;

    Connection(EraserFn eraser, uint64_t id)
        : eraser_(std::move(eraser)), id_(id), connected_(true) {}

    EraserFn eraser_;
    uint64_t id_        = 0;
    bool     connected_ = false;
};

// ── ScopedConnection ──────────────────────────────────────────────────────────
//
// RAII wrapper: automatically calls disconnect() when it goes out of scope.
class ScopedConnection {
public:
    ScopedConnection() = default;
    explicit ScopedConnection(Connection c) : conn_(std::move(c)) {}

    ~ScopedConnection() { conn_.disconnect(); }

    ScopedConnection(const ScopedConnection&)            = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&&)            = default;
    ScopedConnection& operator=(ScopedConnection&&) = default;

    void disconnect() noexcept              { conn_.disconnect(); }
    [[nodiscard]] bool connected() const noexcept { return conn_.connected(); }

    // Transfer ownership so the slot is NOT disconnected on destruction.
    [[nodiscard]] Connection release() { return std::move(conn_); }

private:
    Connection conn_;
};

// ── Signal ────────────────────────────────────────────────────────────────────
//
// All public methods are thread-safe.
// The Signal may be destroyed while Connections that came from it are still
// alive: calling disconnect() on a stale Connection is a safe no-op.
template <typename... Args>
class Signal {
public:
    using SlotFn = std::function<void(Args...)>;

    Signal()  = default;
    ~Signal() = default;

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;

    // Movable: existing Connections remain valid after a move because they
    // hold a weak_ptr into the heap-allocated State, not into the Signal object.
    Signal(Signal&&)            = default;
    Signal& operator=(Signal&&) = default;

    // ── connect ───────────────────────────────────────────────────────────────
    [[nodiscard]] Connection connect(SlotFn slot) {
        const uint64_t id = state_->next_id.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->slots.push_back({id, std::move(slot)});
        }
        // The eraser captures a weak_ptr so it safely no-ops after Signal
        // destruction; it does not extend the Signal's lifetime.
        std::weak_ptr<State> weak = state_;
        return Connection{
            [weak](uint64_t remove_id) {
                if (auto s = weak.lock()) {
                    s->remove(remove_id);
                }
            },
            id
        };
    }

    // ── disconnect (free-function style convenience) ──────────────────────────
    static void disconnect(Connection& c) noexcept { c.disconnect(); }

    // ── emit ──────────────────────────────────────────────────────────────────
    // Copy-on-emit: snapshot the slot list under the lock, then invoke outside
    // the lock. Slots added or removed during emission affect subsequent emits
    // but not the current one.
    void emit(Args... args) {
        // Take a copy so we don't hold the lock while calling user code.
        std::vector<Entry> snapshot;
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            snapshot = state_->slots;          // copies std::function objects
        }
        for (auto& e : snapshot) {
            e.fn(args...);                     // value args: safe to call N times
        }
    }

    // ── slot_count ────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t slot_count() const noexcept {
        std::lock_guard<std::mutex> lk(state_->mtx);
        return state_->slots.size();
    }

private:
    struct Entry {
        uint64_t id;
        SlotFn   fn;
    };

    struct State {
        mutable std::mutex       mtx;
        std::vector<Entry>       slots;
        std::atomic<uint64_t>    next_id{0};

        void remove(uint64_t id) {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = std::find_if(slots.begin(), slots.end(),
                                   [id](const Entry& e) { return e.id == id; });
            if (it != slots.end()) slots.erase(it);
        }
    };

    // Heap-allocated so Connections outlive a moved-from Signal safely.
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace sig
