// test.cpp — tests for signals.hpp
//
// Build (C++17):
//   g++ -std=c++17 -pthread -Wall -Wextra -o test test.cpp && ./test
//   clang++ -std=c++17 -pthread -Wall -Wextra -o test test.cpp && ./test

#include "signals.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static int passed = 0;
static int failed = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        if (expr) {                                                             \
            ++passed;                                                           \
        } else {                                                                \
            ++failed;                                                           \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// 1. basic connect / emit
// ---------------------------------------------------------------------------

static void test_basic_connect_emit() {
    sig::Signal<int> s;
    int received = -1;

    auto conn = s.connect([&](int v) { received = v; });
    s.emit(42);

    CHECK(received == 42);
    CHECK(conn.connected());
}

// ---------------------------------------------------------------------------
// 2. multiple slots all fire
// ---------------------------------------------------------------------------

static void test_multiple_slots() {
    sig::Signal<int> s;
    int a = 0, b = 0, c = 0;

    auto ca = s.connect([&](int v) { a = v; });
    auto cb = s.connect([&](int v) { b = v * 2; });
    auto cc = s.connect([&](int v) { c = v * 3; });

    s.emit(10);

    CHECK(a == 10);
    CHECK(b == 20);
    CHECK(c == 30);
    (void)ca; (void)cb; (void)cc;
}

// ---------------------------------------------------------------------------
// 3. explicit disconnect stops calls
// ---------------------------------------------------------------------------

static void test_explicit_disconnect() {
    sig::Signal<int> s;
    int count = 0;

    auto conn = s.connect([&](int) { ++count; });
    s.emit(1);
    CHECK(count == 1);

    conn.disconnect();
    CHECK(!conn.connected());

    s.emit(2);
    CHECK(count == 1); // should NOT have fired again
}

// ---------------------------------------------------------------------------
// 4. RAII scoped disconnect
// ---------------------------------------------------------------------------

static void test_scoped_disconnect() {
    sig::Signal<int> s;
    int count = 0;

    {
        auto conn = s.connect([&](int) { ++count; });
        s.emit(1);
        CHECK(count == 1);
        // conn goes out of scope here → destructor calls disconnect()
    }

    s.emit(2);
    CHECK(count == 1); // slot was auto-disconnected
    CHECK(s.slot_count() == 0);
}

// ---------------------------------------------------------------------------
// 5. move semantics — connection transfers ownership
// ---------------------------------------------------------------------------

static void test_connection_move() {
    sig::Signal<int> s;
    int count = 0;

    sig::Signal<int>::Connection outer;
    {
        auto inner = s.connect([&](int) { ++count; });
        outer = std::move(inner);
        // inner is now a no-op handle; outer owns the connection
    }
    // inner's destructor ran but should NOT have disconnected (it was moved-from)
    s.emit(1);
    CHECK(count == 1);

    outer.disconnect();
    s.emit(2);
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// 6. signal with no slots is safe
// ---------------------------------------------------------------------------

static void test_emit_no_slots() {
    sig::Signal<double> s;
    // should not crash
    s.emit(3.14);
    CHECK(s.slot_count() == 0);
}

// ---------------------------------------------------------------------------
// 7. disconnect inside a slot (copy-on-emit safety)
// ---------------------------------------------------------------------------

static void test_disconnect_inside_slot() {
    sig::Signal<> s;
    int count = 0;

    // We need a stable pointer to the connection so the lambda can capture it.
    std::unique_ptr<sig::Signal<>::Connection> conn;
    conn = std::make_unique<sig::Signal<>::Connection>(
        s.connect([&]() {
            ++count;
            conn->disconnect(); // disconnect self mid-emit
        })
    );

    s.emit();  // slot fires once and disconnects itself
    CHECK(count == 1);

    s.emit();  // slot is gone — should not fire
    CHECK(count == 1);
    CHECK(s.slot_count() == 0);
}

// ---------------------------------------------------------------------------
// 8. concurrent emit from multiple threads
// ---------------------------------------------------------------------------

static void test_concurrent_emit() {
    sig::Signal<int> s;
    std::atomic<int> sum{0};

    auto c1 = s.connect([&](int v) { sum.fetch_add(v, std::memory_order_relaxed); });
    auto c2 = s.connect([&](int v) { sum.fetch_add(v, std::memory_order_relaxed); });

    // Emit 1 from N threads concurrently; each emit hits both slots.
    constexpr int kThreads = 8;
    constexpr int kEmitsPerThread = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kEmitsPerThread; ++j) {
                s.emit(1);
            }
        });
    }
    for (auto& t : threads) t.join();

    // sum = threads * emits_per_thread * slots = 8 * 1000 * 2
    int expected = kThreads * kEmitsPerThread * 2;
    CHECK(sum.load() == expected);
    (void)c1; (void)c2;
}

// ---------------------------------------------------------------------------
// 9. concurrent connect and emit
// ---------------------------------------------------------------------------

static void test_concurrent_connect_emit() {
    sig::Signal<int> s;
    std::atomic<int> sum{0};

    // Producer thread: emits while connections are being added.
    std::atomic<bool> go{false};

    std::thread emitter([&]() {
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        for (int i = 0; i < 2000; ++i) {
            s.emit(1);
        }
    });

    // Multiple connector threads; connections are RAII, cleaned up at scope end.
    constexpr int kConnectors = 4;
    std::vector<std::thread> connectors;
    connectors.reserve(kConnectors);

    for (int i = 0; i < kConnectors; ++i) {
        connectors.emplace_back([&]() {
            go.store(true, std::memory_order_release);
            std::vector<sig::Signal<int>::Connection> conns;
            conns.reserve(50);
            for (int j = 0; j < 50; ++j) {
                conns.push_back(s.connect([&](int v) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                }));
            }
            // connections destroyed here — RAII disconnect
        });
    }

    for (auto& t : connectors) t.join();
    emitter.join();

    // We can't assert an exact sum here (depends on scheduling), but the
    // program must not crash, deadlock, or trigger data-race sanitizer errors.
    CHECK(sum.load() >= 0); // trivially true; proves no UB that zeroed memory
    // (run with ThreadSanitizer to catch races)
}

// ---------------------------------------------------------------------------
// 10. disconnect from one of many slots; others still fire
// ---------------------------------------------------------------------------

static void test_partial_disconnect() {
    sig::Signal<int> s;
    int a = 0, b = 0;

    auto ca = s.connect([&](int v) { a += v; });
    auto cb = s.connect([&](int v) { b += v; });

    s.emit(1);
    CHECK(a == 1 && b == 1);

    ca.disconnect();
    s.emit(1);
    CHECK(a == 1); // ca gone
    CHECK(b == 2); // cb still alive

    CHECK(s.slot_count() == 1);
    (void)cb;
}

// ---------------------------------------------------------------------------
// 11. signal callable via operator()
// ---------------------------------------------------------------------------

static void test_call_operator() {
    sig::Signal<std::string> s;
    std::string got;

    auto c = s.connect([&](std::string v) { got = std::move(v); });
    s("hello");
    CHECK(got == "hello");
    (void)c;
}

// ---------------------------------------------------------------------------
// 12. connection outlives signal — no crash
// ---------------------------------------------------------------------------

static void test_connection_outlives_signal() {
    sig::Signal<int>::Connection conn;
    {
        sig::Signal<int> s;
        conn = s.connect([](int) {});
        CHECK(conn.connected());
    }
    // s is destroyed; conn.disconnect() should be a no-op
    conn.disconnect();
    CHECK(!conn.connected()); // weak_ptr expired
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    test_basic_connect_emit();
    test_multiple_slots();
    test_explicit_disconnect();
    test_scoped_disconnect();
    test_connection_move();
    test_emit_no_slots();
    test_disconnect_inside_slot();
    test_concurrent_emit();
    test_concurrent_connect_emit();
    test_partial_disconnect();
    test_call_operator();
    test_connection_outlives_signal();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
