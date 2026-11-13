// test.cpp — tests for signals.hpp
//
// Build (C++17):
//   g++ -std=c++17 -pthread -Wall -Wextra -o test test.cpp && ./test
//   clang++ -std=c++17 -pthread -Wall -Wextra -o test test.cpp && ./test

#include "signals.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (expr) {                                                            \
            ++g_pass;                                                          \
            std::cout << "  PASS  " #expr "\n";                               \
        } else {                                                               \
            ++g_fail;                                                          \
            std::cout << "  FAIL  " #expr "  (" __FILE__ ":" +               \
                             std::to_string(__LINE__) + ")\n";                \
        }                                                                      \
    } while (false)

// ---------------------------------------------------------------------------
// 1. basic connect / emit
// ---------------------------------------------------------------------------
static void test_basic_emit() {
    std::cout << "\n[basic connect/emit]\n";

    sig::Signal<int> s;
    int received = 0;

    auto conn = s.connect([&](int v) { received = v; });

    s.emit(42);
    CHECK(received == 42);

    s.emit(99);
    CHECK(received == 99);

    CHECK(s.slot_count() == 1);
    (void)conn;
}

// ---------------------------------------------------------------------------
// 2. multiple slots all fire
// ---------------------------------------------------------------------------
static void test_multiple_slots() {
    std::cout << "\n[multiple slots]\n";

    sig::Signal<> s;
    int a = 0, b = 0, c = 0;

    auto ca = s.connect([&] { ++a; });
    auto cb = s.connect([&] { ++b; });
    auto cc = s.connect([&] { ++c; });

    s.emit();
    CHECK(a == 1 && b == 1 && c == 1);
    CHECK(s.slot_count() == 3);
    (void)ca; (void)cb; (void)cc;
}

// ---------------------------------------------------------------------------
// 3. disconnect via Connection
// ---------------------------------------------------------------------------
static void test_disconnect() {
    std::cout << "\n[disconnect via Connection]\n";

    sig::Signal<int> s;
    int count = 0;

    auto conn = s.connect([&](int) { ++count; });
    s.emit(1);
    CHECK(count == 1);

    conn.disconnect();
    CHECK(!conn.connected());
    CHECK(s.slot_count() == 0);

    s.emit(2);
    CHECK(count == 1); // should not have incremented
}

// ---------------------------------------------------------------------------
// 4. disconnect_all
// ---------------------------------------------------------------------------
static void test_disconnect_all() {
    std::cout << "\n[disconnect_all]\n";

    sig::Signal<> s;
    int x = 0;

    auto c1 = s.connect([&] { ++x; });
    auto c2 = s.connect([&] { ++x; });
    s.disconnect_all();

    CHECK(s.slot_count() == 0);
    s.emit();
    CHECK(x == 0);
    (void)c1; (void)c2;
}

// ---------------------------------------------------------------------------
// 5. RAII ScopedConnection
// ---------------------------------------------------------------------------
static void test_scoped_connection() {
    std::cout << "\n[ScopedConnection RAII]\n";

    sig::Signal<> s;
    int count = 0;

    {
        sig::ScopedConnection sc = s.connect([&] { ++count; });
        CHECK(sc.connected());
        s.emit();
        CHECK(count == 1);
    } // sc destroyed here — slot should be gone

    s.emit();
    CHECK(count == 1); // no second increment
    CHECK(s.slot_count() == 0);
}

// ---------------------------------------------------------------------------
// 6. ScopedConnection release
// ---------------------------------------------------------------------------
static void test_scoped_release() {
    std::cout << "\n[ScopedConnection::release]\n";

    sig::Signal<> s;
    int count = 0;

    sig::Connection kept;
    {
        sig::ScopedConnection sc = s.connect([&] { ++count; });
        kept = sc.release(); // ownership moves out
    } // sc destroyed but slot is still alive via `kept`

    s.emit();
    CHECK(count == 1);
    CHECK(kept.connected());

    kept.disconnect();
    s.emit();
    CHECK(count == 1);
    CHECK(s.slot_count() == 0);
}

// ---------------------------------------------------------------------------
// 7. slot destroyed before signal (dangling-signal safety)
// ---------------------------------------------------------------------------
static void test_signal_outlives_connection() {
    std::cout << "\n[signal outlives connection]\n";

    sig::Signal<> s;
    int count = 0;

    {
        sig::ScopedConnection sc = s.connect([&] { ++count; });
        s.emit();
        CHECK(count == 1);
        // sc destroyed
    }

    s.emit(); // must not crash
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// 8. disconnect from within a slot (copy-on-emit safety)
// ---------------------------------------------------------------------------
static void test_disconnect_during_emit() {
    std::cout << "\n[disconnect during emit]\n";

    sig::Signal<> s;
    int count = 0;
    sig::Connection self_conn;

    self_conn = s.connect([&] {
        ++count;
        self_conn.disconnect(); // remove ourselves mid-emit
    });

    s.emit(); // must not crash, slot fires exactly once
    CHECK(count == 1);
    CHECK(!self_conn.connected());

    s.emit(); // slot is gone
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// 9. connect from within a slot (copy-on-emit safety)
// ---------------------------------------------------------------------------
static void test_connect_during_emit() {
    std::cout << "\n[connect during emit]\n";

    sig::Signal<> s;
    int count = 0;
    sig::Connection late;

    auto early = s.connect([&] {
        // Add a second slot during emission — it must NOT fire this round.
        late = s.connect([&] { ++count; });
    });

    s.emit(); // early fires, late is added but should not fire yet
    CHECK(count == 0);
    CHECK(s.slot_count() == 2);

    s.emit(); // now both fire
    CHECK(count == 1);
    (void)early; (void)late;
}

// ---------------------------------------------------------------------------
// 10. operator() alias
// ---------------------------------------------------------------------------
static void test_call_operator() {
    std::cout << "\n[operator() alias]\n";

    sig::Signal<std::string> s;
    std::string last;

    auto c = s.connect([&](std::string v) { last = std::move(v); });
    s("hello");
    CHECK(last == "hello");
    (void)c;
}

// ---------------------------------------------------------------------------
// 11. concurrent emit from multiple threads
// ---------------------------------------------------------------------------
static void test_concurrent_emit() {
    std::cout << "\n[concurrent emit — " << std::thread::hardware_concurrency()
              << " threads]\n";

    constexpr int kThreads    = 8;
    constexpr int kEmitsEach  = 10'000;

    sig::Signal<int> s;
    std::atomic<long long> total{0};

    auto c1 = s.connect([&](int v) { total.fetch_add(v, std::memory_order_relaxed); });
    auto c2 = s.connect([&](int v) { total.fetch_add(v, std::memory_order_relaxed); });

    // Each thread emits 1 kEmitsEach times; two slots each add 1 → expected:
    // kThreads * kEmitsEach * 2 * 1
    const long long expected =
        static_cast<long long>(kThreads) * kEmitsEach * 2 * 1;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kEmitsEach; ++i) s.emit(1);
        });
    }
    for (auto& th : threads) th.join();

    CHECK(total.load() == expected);
    (void)c1; (void)c2;
}

// ---------------------------------------------------------------------------
// 12. concurrent connect/disconnect + emit
// ---------------------------------------------------------------------------
static void test_concurrent_connect_disconnect() {
    std::cout << "\n[concurrent connect/disconnect + emit]\n";

    constexpr int kRounds = 5'000;

    sig::Signal<> s;
    std::atomic<int> fire_count{0};

    // Writer thread: repeatedly connects and disconnects a slot.
    std::thread writer([&] {
        for (int i = 0; i < kRounds; ++i) {
            auto c = s.connect([&] { fire_count.fetch_add(1, std::memory_order_relaxed); });
            // yield so emitters get a chance to see the slot
            std::this_thread::yield();
            c.disconnect();
        }
    });

    // Emitter threads: hammer emit concurrently with the writer.
    constexpr int kEmitters = 4;
    std::vector<std::thread> emitters;
    emitters.reserve(kEmitters);
    for (int t = 0; t < kEmitters; ++t) {
        emitters.emplace_back([&] {
            for (int i = 0; i < kRounds * 2; ++i) {
                s.emit();
                std::this_thread::yield();
            }
        });
    }

    writer.join();
    for (auto& th : emitters) th.join();

    // We can't assert an exact count because the slot may or may not be present
    // at any given emit, but the test must complete without crashing or data
    // races (run under ThreadSanitizer to verify).
    CHECK(fire_count.load() >= 0); // trivially true — the real check is "no crash"
    std::cout << "    (slot fired " << fire_count.load()
              << " times across concurrent connect/disconnect/emit)\n";
}

// ---------------------------------------------------------------------------
// 13. signal destroyed while Connection is still alive
// ---------------------------------------------------------------------------
static void test_connection_outlives_signal() {
    std::cout << "\n[connection outlives signal]\n";

    sig::Connection orphan;
    {
        sig::Signal<> s;
        orphan = s.connect([] {});
        CHECK(orphan.connected());
        // s destroyed here
    }

    // After signal is gone, disconnect must be a no-op — not a crash.
    orphan.disconnect();
    CHECK(!orphan.connected());
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== signals.hpp test suite ===\n";

    test_basic_emit();
    test_multiple_slots();
    test_disconnect();
    test_disconnect_all();
    test_scoped_connection();
    test_scoped_release();
    test_signal_outlives_connection();
    test_disconnect_during_emit();
    test_connect_during_emit();
    test_call_operator();
    test_concurrent_emit();
    test_concurrent_connect_disconnect();
    test_connection_outlives_signal();

    std::cout << "\n=== results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
