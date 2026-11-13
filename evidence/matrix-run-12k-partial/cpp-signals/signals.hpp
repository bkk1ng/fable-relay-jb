#pragma once

// signals.hpp — header-only, C++17, thread-safe signal/slot library
//
// Usage:
//   sig::Signal<int, float> s;
//   auto conn = s.connect([](int a, float b){ ... }); // RAII Connection
//   s.emit(1, 2.f);            // copy-on-emit: snapshot before calling slots
//   conn.disconnect();         // explicit early disconnect
//   // conn's destructor disconnects automatically

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sig {

template <typename... Args>
class Signal {
public:
    using SlotFn = std::function<void(Args...)>;
    using ID     = std::uint64_t;

    // ------------------------------------------------------------------
    // Connection — RAII handle returned by Signal::connect().
    //
    // * Move-only (non-copyable).
    // * Destructor calls disconnect(), making it a "scoped connection".
    // * Holds only a weak_ptr to the shared State, so it is safe to
    //   outlive the Signal — disconnect() becomes a no-op in that case.
    // ------------------------------------------------------------------
    class Connection {
        friend class Signal;

        std::weak_ptr<struct State> wstate_;
        ID                          id_{0};

        // Private; only Signal::connect() constructs a live Connection.
        Connection(std::shared_ptr<struct State> s, ID id)
            : wstate_{std::move(s)}, id_{id} {}

    public:
        Connection() = default;

        ~Connection() { disconnect(); }

        Connection(const Connection&)            = delete;
        Connection& operator=(const Connection&) = delete;

        Connection(Connection&& o) noexcept
            : wstate_{std::move(o.wstate_)}, id_{o.id_} {
            o.id_ = 0; // source is now a no-op handle
        }

        Connection& operator=(Connection&& o) noexcept {
            if (this != &o) {
                disconnect();
                wstate_ = std::move(o.wstate_);
                id_     = o.id_;
                o.id_   = 0;
            }
            return *this;
        }

        // Explicitly remove the slot from the Signal.  Safe to call
        // multiple times and safe to call from inside the slot itself
        // (copy-on-emit ensures we iterate a snapshot, not the live list).
        void disconnect() noexcept {
            if (id_ == 0) return;
            if (auto s = wstate_.lock()) {
                s->remove(id_);
            }
            id_ = 0;
        }

        // Returns false once disconnected or once the Signal is destroyed.
        [[nodiscard]] bool connected() const noexcept {
            return id_ != 0 && !wstate_.expired();
        }
    };

    // ------------------------------------------------------------------
    // Signal
    // ------------------------------------------------------------------
    Signal() : state_{std::make_shared<State>()} {}

    // Signals are non-copyable but movable.
    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) noexcept            = default;
    Signal& operator=(Signal&&) noexcept = default;
    ~Signal()                            = default;

    // Connect a callable; returns a RAII Connection.
    // Thread-safe: multiple threads may call connect() concurrently.
    [[nodiscard]] Connection connect(SlotFn slot) {
        std::lock_guard<std::mutex> lk{state_->mutex};
        const ID id =
            state_->next_id.fetch_add(1, std::memory_order_relaxed);
        state_->slots.emplace_back(id, std::move(slot));
        return Connection{state_, id};
    }

    // Emit — copy-on-emit strategy:
    //   1. Lock, copy the slot vector, unlock.
    //   2. Iterate the snapshot without any lock held.
    //
    // This means:
    //   * Slots may safely call connect() or disconnect() during emission.
    //   * Slots added during emission are NOT called for that emit().
    //   * Slots removed during emission may still be called once from the
    //     snapshot (consistent with standard signal/slot semantics).
    //   * Multiple threads may call emit() concurrently with no data races.
    void emit(Args... args) const {
        std::vector<std::pair<ID, SlotFn>> snap;
        {
            std::lock_guard<std::mutex> lk{state_->mutex};
            snap = state_->slots; // O(n) copy; keeps originals intact
        }
        for (auto& [id, fn] : snap) {
            fn(args...);
        }
    }

    // Convenience: signal objects are callable.
    void operator()(Args... args) const { emit(std::forward<Args>(args)...); }

    // Approximate slot count (consistent snapshot, but may change immediately).
    [[nodiscard]] std::size_t slot_count() const noexcept {
        std::lock_guard<std::mutex> lk{state_->mutex};
        return state_->slots.size();
    }

private:
    // Shared between the Signal and all live Connection objects.
    struct State {
        mutable std::mutex                 mutex;
        std::vector<std::pair<ID, SlotFn>> slots;
        std::atomic<ID>                    next_id{1};

        void remove(ID id) noexcept {
            std::lock_guard<std::mutex> lk{mutex};
            slots.erase(
                std::remove_if(slots.begin(), slots.end(),
                               [id](const auto& p) { return p.first == id; }),
                slots.end());
        }
    };

    std::shared_ptr<State> state_;
};

} // namespace sig
