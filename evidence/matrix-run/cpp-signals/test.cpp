// test.cpp — signals.hpp test suite
//
// Covers:
//   1. Basic connect and emit
//   2. Disconnect via Connection handle
//   3. RAII ScopedConnection auto-disconnect on scope exit
//   4. Multiple slots, partial disconnect
//   5. Concurrent emit from N threads (data-race free under TSan)
//   6. Slot self-disconnect during emit (copy-on-emit safety)
//   7. Signal destroyed while Connection is live (connected() goes false)
//
// Build (Linux / macOS):
//   g++ -std=c++17 -Wall -Wextra -pthread -o test test.cpp && ./test
//
// Build with ThreadSanitizer:
//   g++ -std=c++17 -Wall -Wextra -pthread -fsanitize=thread -o test_tsan test.cpp && ./test_tsan

#include "signals.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// ── tiny test harness ──────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (expr) {                                                       \
            ++g_passed;                                                   \
        } else {                                                          \
            ++g_failed;                                                   \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                    \
                         __FILE__, __LINE__, #expr);                      \
        }                                                                 \
    } while (0)

#define SECTION(name) std::printf("  [ %s ]\n", name)

// ── tests ──────────────────────────────────────────────────────────────────────

static void test_basic_connect_emit() {
    SECTION("basic connect and emit");

    signals::Signal<int> sig;
    int received = -1;

    auto conn = sig.connect([&](int v) { received = v; });
    CHECK(conn.connected());

    sig.emit(42);
    CHECK(received == 42);

    // operator() shorthand
    sig(99);
    CHECK(received == 99);
}

static void test_disconnect() {
    SECTION("disconnect via Connection");

    signals::Signal<int> sig;
    int count = 0;

    auto conn = sig.connect([&](int) { ++count; });
    sig.emit(1);
    CHECK(count == 1);

    conn.disconnect();
    CHECK(!conn.connected());

    sig.emit(2);
    CHECK(count == 1); // slot not called after disconnect

    // idempotent — no crash
    conn.disconnect();
    CHECK(!conn.connected());
}

static void test_scoped_connection() {
    SECTION("RAII ScopedConnection");

    signals::Signal<> sig;
    int count = 0;

    {
        signals::ScopedConnection sc{sig.connect([&] { ++count; })};
        CHECK(sc.connected());
        sig.emit();
        CHECK(count == 1);
    } // sc destroyed here → auto-disconnect

    sig.emit();
    CHECK(count == 1); // slot gone
}

static void test_multiple_slots_partial_disconnect() {
    SECTION("multiple slots, partial disconnect");

    signals::Signal<int> sig;
    int a = 0, b = 0, c = 0;

    auto ca = sig.connect([&](int v) { a += v; });
    auto cb = sig.connect([&](int v) { b += v; });
    auto cc = sig.connect([&](int v) { c += v; });

    sig.emit(10);
    CHECK(a == 10 && b == 10 && c == 10);

    cb.disconnect();
    sig.emit(5);
    CHECK(a == 15 && b == 10 && c == 15); // b unchanged
}

static void test_no_slots() {
    SECTION("emit with no slots (no crash)");

    signals::Signal<std::string> sig;
    sig.emit("hello"); // must not crash
    CHECK(true);
}

static void test_signal_destroyed_before_connection() {
    SECTION("Signal destroyed while Connection is live");

    signals::Connection conn;
    {
        signals::Signal<> sig;
        conn = sig.connect([] {});
        CHECK(conn.connected());
    } // sig destroyed

    CHECK(!conn.connected());
    conn.disconnect(); // must not crash
}

static void test_self_disconnect_during_emit() {
    SECTION("slot self-disconnects during emit (copy-on-emit safety)");

    signals::Signal<int> sig;
    int count = 0;

    // 'conn' is captured by reference inside the lambda; initialised before
    // connect() so the address is stable by the time the lambda runs.
    signals::Connection conn;
    conn = sig.connect([&](int) {
        ++count;
        conn.disconnect(); // remove self while emit iterates the snapshot
    });

    sig.emit(1); // triggers once, then removes itself
    CHECK(count == 1);
    sig.emit(2); // should fire no slots
    CHECK(count == 1);
}

static void test_concurrent_emit() {
    SECTION("concurrent emit from multiple threads");

    // N threads each emit M times; the slot atomically increments a counter.
    // Under TSan this proves there is no data race.

    constexpr int N = 8;   // threads
    constexpr int M = 500; // emits per thread

    signals::Signal<> sig;
    std::atomic<int> counter{0};

    auto conn = sig.connect([&] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < M; ++j) {
                sig.emit();
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK(counter.load() == N * M);
}

static void test_concurrent_connect_disconnect_emit() {
    SECTION("concurrent connect / disconnect / emit");

    // One thread continuously emits; another repeatedly connects and
    // disconnects.  No crash and no data race is the success criterion.

    signals::Signal<int> sig;
    std::atomic<bool> stop{false};
    std::atomic<int>  emits{0};

    std::thread emitter([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sig.emit(1);
            emits.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread toggler([&] {
        for (int i = 0; i < 2000; ++i) {
            auto conn = sig.connect([](int) {});
            // conn goes out of scope → ScopedConnection-equivalent via dtor
            // (here it is a plain Connection, so we disconnect explicitly)
            conn.disconnect();
        }
    });

    toggler.join();
    stop.store(true, std::memory_order_relaxed);
    emitter.join();

    CHECK(emits.load() > 0); // at least some emits occurred — sanity
}

static void test_move_scoped_connection() {
    SECTION("ScopedConnection move semantics");

    signals::Signal<> sig;
    int count = 0;

    signals::ScopedConnection sc2;
    {
        signals::ScopedConnection sc1{sig.connect([&] { ++count; })};
        CHECK(sc1.connected());
        sc2 = std::move(sc1);
        CHECK(!sc1.connected()); // moved-from: disconnected
        CHECK(sc2.connected());
        sig.emit();
        CHECK(count == 1);
    } // sc1 destroyed (already empty) — sc2 still alive

    sig.emit();
    CHECK(count == 2);

    sc2.disconnect();
    sig.emit();
    CHECK(count == 2);
}

// ── main ───────────────────────────────────────────────────────────────────────

int main() {
    std::printf("signals.hpp test suite\n");
    std::printf("──────────────────────\n");

    test_basic_connect_emit();
    test_disconnect();
    test_scoped_connection();
    test_multiple_slots_partial_disconnect();
    test_no_slots();
    test_signal_destroyed_before_connection();
    test_self_disconnect_during_emit();
    test_concurrent_emit();
    test_concurrent_connect_disconnect_emit();
    test_move_scoped_connection();

    std::printf("──────────────────────\n");
    std::printf("passed: %d  failed: %d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
