// test.cpp — LRU<K,V> test harness, C++17
// Build: g++ -std=c++17 -O2 -Wall -Wextra -o test test.cpp && ./test

#include "lru.hpp"

#include <cassert>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Minimal assert harness
// ---------------------------------------------------------------------------

static int passed = 0;
static int failed = 0;

#define CHECK(expr)                                                   \
    do {                                                               \
        if (expr) {                                                    \
            ++passed;                                                  \
        } else {                                                       \
            ++failed;                                                  \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                 \
                         __FILE__, __LINE__, #expr);                   \
        }                                                              \
    } while (false)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Dereference an optional<reference_wrapper<V>> safely for comparisons.
template <typename V>
static V opt_val(std::optional<std::reference_wrapper<V>> o) {
    assert(o.has_value());
    return o->get();
}

// ---------------------------------------------------------------------------
// 1. Basic capacity and eviction order
// ---------------------------------------------------------------------------
static void test_eviction_order() {
    LRU<int, std::string> c(3);

    c.put(1, "one");
    c.put(2, "two");
    c.put(3, "three");
    CHECK(c.size() == 3);

    // 4 evicts LRU which is key 1 (oldest, never touched since insert).
    c.put(4, "four");
    CHECK(c.size() == 3);
    CHECK(!c.contains(1));
    CHECK(c.contains(2));
    CHECK(c.contains(3));
    CHECK(c.contains(4));

    // Access key 2 — promotes it; next eviction should take key 3.
    auto v = c.get(2);
    CHECK(v.has_value());
    CHECK(opt_val(v) == "two");

    c.put(5, "five");   // evicts key 3
    CHECK(!c.contains(3));
    CHECK(c.contains(2));
    CHECK(c.contains(4));
    CHECK(c.contains(5));
}

// ---------------------------------------------------------------------------
// 2. Update existing key (no eviction, promoted to MRU)
// ---------------------------------------------------------------------------
static void test_update() {
    LRU<std::string, int> c(2);

    c.put("a", 1);
    c.put("b", 2);

    // Update "a" — it becomes MRU; "b" is now LRU.
    c.put("a", 99);
    CHECK(opt_val(c.get("a")) == 99);
    CHECK(c.size() == 2);

    // Insert "c" — evicts "b" (LRU), not "a".
    c.put("c", 3);
    CHECK(!c.contains("b"));
    CHECK(c.contains("a"));
    CHECK(c.contains("c"));
}

// ---------------------------------------------------------------------------
// 3. Erase
// ---------------------------------------------------------------------------
static void test_erase() {
    LRU<int, int> c(4);
    for (int i = 0; i < 4; ++i) c.put(i, i * 10);

    CHECK(c.erase(2));
    CHECK(c.size() == 3);
    CHECK(!c.contains(2));
    CHECK(!c.get(2).has_value());

    // Double erase returns false.
    CHECK(!c.erase(2));

    // After erasing, we can insert a new key without bumping an existing one.
    c.put(10, 100);
    CHECK(c.size() == 4);
    CHECK(c.contains(10));

    // Now full — inserting another should evict LRU (key 0, oldest untouched).
    c.put(11, 110);
    CHECK(!c.contains(0));
    CHECK(c.size() == 4);
}

// ---------------------------------------------------------------------------
// 4. get() promotes to MRU
// ---------------------------------------------------------------------------
static void test_get_promotes() {
    LRU<int, int> c(3);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);

    // Touch key 1 — it becomes MRU; LRU order becomes: 2 < 3 < 1.
    // Wait, insertion order was 1→2→3, so before touch:
    //   MRU→3, 2, 1←LRU
    // After touching 1:
    //   MRU→1, 3, 2←LRU
    c.get(1);

    // Insert 4 — evicts 2 (new LRU).
    c.put(4, 4);
    CHECK(!c.contains(2));
    CHECK(c.contains(1));
    CHECK(c.contains(3));
    CHECK(c.contains(4));
}

// ---------------------------------------------------------------------------
// 5. Size and capacity
// ---------------------------------------------------------------------------
static void test_size_capacity() {
    LRU<int, int> c(5);
    CHECK(c.capacity() == 5);
    CHECK(c.size() == 0);
    CHECK(c.empty());

    for (int i = 0; i < 10; ++i) c.put(i, i);
    CHECK(c.size() == 5);
    CHECK(!c.empty());
}

// ---------------------------------------------------------------------------
// 6. Move semantics — values should survive a cache move
// ---------------------------------------------------------------------------
static void test_move_semantics() {
    LRU<int, std::string> c1(3);
    c1.put(1, "alpha");
    c1.put(2, "beta");

    LRU<int, std::string> c2(std::move(c1));
    CHECK(c2.size() == 2);
    CHECK(opt_val(c2.get(1)) == "alpha");
    CHECK(opt_val(c2.get(2)) == "beta");

    // Move-assign
    LRU<int, std::string> c3(10);
    c3 = std::move(c2);
    CHECK(c3.size() == 2);
    CHECK(opt_val(c3.get(1)) == "alpha");
}

// ---------------------------------------------------------------------------
// 7. Capacity-1 edge case
// ---------------------------------------------------------------------------
static void test_capacity_one() {
    LRU<int, int> c(1);
    c.put(1, 100);
    CHECK(opt_val(c.get(1)) == 100);

    c.put(2, 200);
    CHECK(!c.contains(1));
    CHECK(opt_val(c.get(2)) == 200);
    CHECK(c.size() == 1);
}

// ---------------------------------------------------------------------------
// 8. Stress — 10 000 sequential inserts into a cache of size 100.
//    Verifies that only the last 100 keys survive eviction.
// ---------------------------------------------------------------------------
static void test_stress() {
    constexpr int N    = 10'000;
    constexpr int CAP  = 100;

    LRU<int, int> c(CAP);

    for (int i = 0; i < N; ++i)
        c.put(i, i * 2);

    CHECK(c.size() == CAP);

    // Keys [N-CAP, N) must all be present.
    for (int i = N - CAP; i < N; ++i) {
        CHECK(c.contains(i));
        CHECK(opt_val(c.get(i)) == i * 2);
    }

    // Keys before the window must be evicted.
    for (int i = 0; i < N - CAP; ++i)
        CHECK(!c.contains(i));
}

// ---------------------------------------------------------------------------
// 9. Stress — interleaved gets and puts, verifying no corruption.
// ---------------------------------------------------------------------------
static void test_stress_interleaved() {
    constexpr int N   = 10'000;
    constexpr int CAP = 256;

    LRU<int, int> c(CAP);

    // Fill halfway.
    for (int i = 0; i < CAP / 2; ++i)
        c.put(i, i);

    // Interleave accesses and new inserts.
    for (int i = CAP / 2; i < N; ++i) {
        // Touch a key in the lower half to keep it alive.
        int touch = i % (CAP / 2);
        auto v = c.get(touch);
        CHECK(v.has_value());
        CHECK(opt_val(v) == touch);

        c.put(i, i);
    }

    CHECK(c.size() == CAP);

    // Every key in the lower half should still be present (we kept touching them).
    for (int i = 0; i < CAP / 2; ++i)
        CHECK(c.contains(i));
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
int main() {
    test_eviction_order();
    test_update();
    test_erase();
    test_get_promotes();
    test_size_capacity();
    test_move_semantics();
    test_capacity_one();
    test_stress();
    test_stress_interleaved();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
