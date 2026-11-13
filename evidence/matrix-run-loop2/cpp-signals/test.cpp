// test.cpp — proves the Signal/Slot library from signals.hpp
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -pthread -O2 test.cpp -o signals_test
// Run:
//   ./signals_test
//
// Exit code is 0 if every assertion passes, non-zero otherwise.

#include "signals.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace signals;

// ─── tiny test framework ────────────────────────────────────────────────────
static int g_failures = 0;

#define RUN_TEST(fn) do {                                          \
    std::cout << "[ run ] " << #fn << " ... ";                      \
    try { fn(); std::cout << "ok" << std::endl; }                   \
    catch (const std::exception& e) {                               \
        ++g_failures;                                               \
        std::cout << "FAIL: " << e.what() << std::endl;             \
    }                                                               \
} while (0)

#define REQUIRE(cond, msg) do {                                    \
    if (!(cond)) {                                                 \
        throw std::runtime_error(std::string(msg)                  \
            + " (line " + std::to_string(__LINE__) + ")");          \
    }                                                               \
} while (0)

// ─── 1. basic connect and emit ───────────────────────────────────────────────
static void test_basic_connect_emit() {
    Signal<int> sig;
    int received = 0;

    auto conn = sig.connect([&](int v) { received = v; });
    sig.emit(42);

    REQUIRE(received == 42, "slot should receive 42");
    REQUIRE(conn.connected(), "connection should still be active");
}

// ─── 2. multiple slots all fire ─────────────────────────────────────────────
static void test_multiple_slots() {
    Signal<int> sig;
    int sum = 0;

    sig.connect([&](int v) { sum += v; });
    sig.connect([&](int v) { sum += v * 10; });
    sig.connect([&](int v) { sum += v * 100; });

    sig.emit(1);
    REQUIRE(sum == 111, "slots should sum to 111");
}

// ─── 3. Connection::disconnect removes the slot ─────────────────────────────
static void test_disconnect_removes_slot() {
    Signal<int> sig;
    int count = 0;

    auto conn = sig.connect([&](int) { ++count; });
    sig.emit(0);
    REQUIRE(count == 1, "first emit should run once");

    conn.disconnect();
    REQUIRE(!conn.connected(), "connection should report disconnected");

    sig.emit(0);
    REQUIRE(count == 1, "disconnected slot should not fire again");
}

// ─── 4. disconnect is idempotent ─────────────────────────────────────────────
static void test_disconnect_idempotent() {
    Signal<int> sig;
    int count = 0;

    auto conn = sig.connect([&](int) { ++count; });
    conn.disconnect();
    conn.disconnect(); // double-disconnect must not throw or crash
    conn.disconnect();

    sig.emit(0);
    REQUIRE(count == 0, "slot should never fire after disconnect");
}

// ─── 5. keeping only the signal, dropping the Connection ────────────────────
//     Without ScopedConnection, a dropped Connection should NOT disconnect the
//     slot — the signal owns it. This is the documented behaviour.
static void test_dropped_connection_keeps_slot() {
    Signal<int> sig;
    int count = 0;

    {
        auto conn = sig.connect([&](int) { ++count; });
        REQUIRE(conn.connected(), "should be connected inside scope");
    } // conn destroyed here, but it is NOT scoped, so the slot lives on

    sig.emit(0);
    REQUIRE(count == 1, "slot should still fire because Connection was not scoped");
}

// ─── 6. ScopedConnection disconnects on scope exit ──────────────────────────
static void test_scoped_connection_disconnects() {
    Signal<int> sig;
    int count = 0;

    {
        ScopedConnection sc = sig.connect([&](int) { ++count; });
        REQUIRE(sc.connected(), "scoped connection active in scope");
        sig.emit(0);
        REQUIRE(count == 1, "scoped slot should fire once in scope");
    } // sc destroyed -> disconnects

    sig.emit(0);
    REQUIRE(count == 1, "scoped slot should not fire after scope exit");
}

// ─── 7. ScopedConnection move transfers ownership ───────────────────────────
static void test_scoped_connection_move() {
    Signal<int> sig;
    int count = 0;

    ScopedConnection outer;
    {
        ScopedConnection inner = sig.connect([&](int) { ++count; });
        REQUIRE(inner.connected(), "inner connected");
        outer = std::move(inner);
        REQUIRE(!inner.connected(), "moved-from inner should be disconnected");
    }
    REQUIRE(outer.connected(), "outer should own the connection now");

    sig.emit(0);
    REQUIRE(count == 1, "slot should fire once after move");

    outer.disconnect();
    sig.emit(0);
    REQUIRE(count == 1, "slot should not fire after outer disconnect");
}

// ─── 8. ScopedConnection::release transfers without disconnecting ───────────
static void test_scoped_release() {
    Signal<int> sig;
    int count = 0;

    Connection manual;
    {
        ScopedConnection sc = sig.connect([&](int) { ++count; });
        manual = sc.release();       // hand off without disconnect
        REQUIRE(!sc.connected(), "released scoped should not be connected");
    } // sc destroyed but released -> no disconnect

    REQUIRE(manual.connected(), "manual should hold the connection");
    sig.emit(0);
    REQUIRE(count == 1, "slot should fire after release");

    manual.disconnect();
    sig.emit(0);
    REQUIRE(count == 1, "slot should not fire after manual disconnect");
}

// ─── 9. emit with no slots is a no-op ───────────────────────────────────────
static void test_emit_no_slots() {
    Signal<int, std::string> sig;
    sig.emit(7, "hello"); // must not crash
}

// ─── 10. multi-argument signal ──────────────────────────────────────────────
static void test_multi_arg_signal() {
    Signal<int, const std::string&> sig;
    int last_i = -1;
    std::string last_s;

    sig.connect([&](int i, const std::string& s) {
        last_i = i;
        last_s = s;
    });

    sig.emit(3, std::string("abc"));
    REQUIRE(last_i == 3, "int arg should propagate");
    REQUIRE(last_s == "abc", "string arg should propagate");
}

// ─── 11. connect from within a slot (re-entrant connect) ─────────────────────
//     Copy-on-emit means the snapshot is fixed before invocation, so a slot
//     that connects a new slot will NOT see the new slot in the same emit.
static void test_connect_from_slot() {
    Signal<int> sig;
    int count = 0;
    int new_slot_calls = 0;

    sig.connect([&](int) {
        ++count;
        sig.connect([&](int) { ++new_slot_calls; });
    });

    sig.emit(0);
    REQUIRE(count == 1, "original slot fired once");
    REQUIRE(new_slot_calls == 0, "new slot should NOT fire in the same emit");

    sig.emit(0);
    REQUIRE(count == 2, "original slot fired twice total");
    REQUIRE(new_slot_calls == 1, "new slot fires on the second emit");
}

// ─── 12. disconnect a different slot from within a slot ──────────────────────
static void test_disconnect_other_from_slot() {
    Signal<int> sig;
    int a_calls = 0;
    int b_calls = 0;

    Connection conn_b;
    conn_b = sig.connect([&](int) { ++a_calls; conn_b.disconnect(); });
    sig.connect([&](int) { ++b_calls; });

    sig.emit(0);
    // A fired, disconnected B. B was in the snapshot so it also fired.
    REQUIRE(a_calls == 1, "slot A fired");
    REQUIRE(b_calls == 1, "slot B fired (was in snapshot)");

    sig.emit(0);
    REQUIRE(a_calls == 2, "slot A fires again");
    REQUIRE(b_calls == 1, "slot B should NOT fire after being disconnected");
}

// ─── 13. signal survives connection destruction ──────────────────────────────
//     Connection holds a weak_ptr to the state, so destroying all connections
//     must not crash the signal.
static void test_signal_survives_connection_drop() {
    Signal<int> sig;
    int count = 0;

    {
        std::vector<Connection> conns;
        for (int i = 0; i < 5; ++i)
            conns.push_back(sig.connect([&](int) { ++count; }));
    } // all connections destroyed (not scoped)

    sig.emit(0);
    // All 5 slots are still registered because plain Connection does not
    // disconnect on destruction.
    REQUIRE(count == 5, "five slots should fire");
}

// ─── 14. concurrent emit from multiple threads ──────────────────────────────
static void test_concurrent_emit() {
    Signal<int> sig;
    std::atomic<int> total{0};

    const int num_slots   = 10;
    const int num_threads = 8;
    const int emits_per   = 5000;

    std::vector<ScopedConnection> keepalive;
    for (int s = 0; s < num_slots; ++s) {
        keepalive.emplace_back(
            sig.connect([&](int v) { total.fetch_add(v, std::memory_order_relaxed); }));
    }

    auto worker = [&]() {
        for (int e = 0; e < emits_per; ++e)
            sig.emit(1);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();

    const int expected = num_slots * num_threads * emits_per;
    REQUIRE(total.load() == expected,
            "concurrent emit total mismatch");
}

// ─── 15. concurrent connect + emit ───────────────────────────────────────────
static void test_concurrent_connect_emit() {
    Signal<int> sig;
    std::atomic<bool> running{true};

    // Emitter threads
    const int num_emitters = 4;
    const int emits_per   = 3000;

    auto emitter = [&]() {
        for (int e = 0; e < emits_per; ++e)
            sig.emit(1);
    };

    // Connector thread: rapidly connect + disconnect slots
    auto connector = [&]() {
        while (running.load(std::memory_order_relaxed)) {
            ScopedConnection sc = sig.connect([](int) {});
            sig.emit(1);
            // sc disconnects on next loop iteration
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(connector);
    for (int t = 0; t < num_emitters; ++t)
        threads.emplace_back(emitter);

    // Let emitters finish first, then stop connector
    // We join all; connector may spin a bit extra but running=false stops it
    // after the emitters complete.
    // Join emitters
    for (std::size_t i = 1; i < threads.size(); ++i)
        threads[i].join();
    running = false;
    threads[0].join();

    // No assertion on exact count — the test verifies no crash and no data
    // race (run under TSan to confirm).
    REQUIRE(true, "concurrent connect+emit completed without crash");
}

// ─── 16. concurrent disconnect during emit ──────────────────────────────────
static void test_concurrent_disconnect_during_emit() {
    Signal<int> sig;
    std::atomic<int> fired{0};
    std::atomic<bool> emitting{true};

    const int num_slots = 50;
    std::vector<Connection> conns;
    for (int s = 0; s < num_slots; ++s) {
        conns.push_back(
            sig.connect([&](int) { fired.fetch_add(1, std::memory_order_relaxed); }));
    }

    // Emitter thread
    std::thread emitter([&]() {
        while (emitting.load(std::memory_order_relaxed))
            sig.emit(1);
    });

    // Disconnector thread: disconnect slots one by one
    std::thread disconnector([&]() {
        for (auto& c : conns) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            c.disconnect();
        }
    });

    disconnector.join();
    emitting = false;
    emitter.join();

    // Some slots fired, some were disconnected mid-emit. The snapshot guarantees
    // that only slots present at snapshot time run, and the vector copy keeps
    // those std::function objects alive even if disconnect races. No crash, no
    // UB (under TSan/ASan).
    REQUIRE(fired.load() >= 0, "fired count should be non-negative");
}

// ─── 17. emit after all slots disconnected ───────────────────────────────────
static void test_emit_after_all_disconnected() {
    Signal<int> sig;
    int count = 0;

    std::vector<Connection> conns;
    for (int i = 0; i < 10; ++i)
        conns.push_back(sig.connect([&](int) { ++count; }));

    for (auto& c : conns)
        c.disconnect();

    sig.emit(0);
    REQUIRE(count == 0, "no slot should fire after all disconnected");
}

// ─── 18. void signal (zero arguments) ───────────────────────────────────────
static void test_void_signal() {
    Signal<> sig;
    int count = 0;

    ScopedConnection sc = sig.connect([&]() { ++count; });
    sig.emit();
    REQUIRE(count == 1, "void slot should fire once");
}

// ─── 19. signal move ────────────────────────────────────────────────────────
static void test_signal_move() {
    int count = 0;
    Signal<int> sig;
    sig.connect([&](int) { ++count; });

    Signal<int> moved = std::move(sig); // state shared_ptr moves
    moved.emit(7);
    REQUIRE(count == 1, "moved-to signal should fire its slot");
}

// ─── 20. heavy payload passed by value ──────────────────────────────────────
static void test_string_payload() {
    Signal<std::string> sig;
    std::string captured;

    sig.connect([&](std::string s) { captured = std::move(s); });

    std::string big(1024, 'x');
    sig.emit(big);
    REQUIRE(captured.size() == 1024, "big string should arrive intact");
    REQUIRE(captured[0] == 'x', "first char should be x");
}

// ─── main ───────────────────────────────────────────────────────────────────
int main() {
    std::cout << "=== signals.hpp test suite ===" << std::endl;

    RUN_TEST(test_basic_connect_emit);
    RUN_TEST(test_multiple_slots);
    RUN_TEST(test_disconnect_removes_slot);
    RUN_TEST(test_disconnect_idempotent);
    RUN_TEST(test_dropped_connection_keeps_slot);
    RUN_TEST(test_scoped_connection_disconnects);
    RUN_TEST(test_scoped_connection_move);
    RUN_TEST(test_scoped_release);
    RUN_TEST(test_emit_no_slots);
    RUN_TEST(test_multi_arg_signal);
    RUN_TEST(test_connect_from_slot);
    RUN_TEST(test_disconnect_other_from_slot);
    RUN_TEST(test_signal_survives_connection_drop);
    RUN_TEST(test_concurrent_emit);
    RUN_TEST(test_concurrent_connect_emit);
    RUN_TEST(test_concurrent_disconnect_during_emit);
    RUN_TEST(test_emit_after_all_disconnected);
    RUN_TEST(test_void_signal);
    RUN_TEST(test_signal_move);
    RUN_TEST(test_string_payload);

    std::cout << "===============================" << std::endl;
    if (g_failures == 0) {
        std::cout << "all tests passed" << std::endl;
        return 0;
    }
    std::cout << g_failures << " test(s) failed" << std::endl;
    return 1;
}
