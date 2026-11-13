// test.cpp — LRU<K,V> test harness (C++17, no external dependencies)
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra -o test test.cpp && ./test
//   clang++ -std=c++17 -O2 -Wall -Wextra -o test test.cpp && ./test

#include "lru.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Minimal assert harness
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                    \
    do {                                                               \
        if (expr) {                                                    \
            ++g_pass;                                                  \
        } else {                                                       \
            ++g_fail;                                                  \
            std::fprintf(stderr, "FAIL  %s:%d  %s\n",                 \
                         __FILE__, __LINE__, #expr);                   \
        }                                                              \
    } while (0)

static void section(const char* name) {
    std::printf("\n--- %s ---\n", name);
}

static void summary() {
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) std::exit(1);
}

// ---------------------------------------------------------------------------
// 1. Basic get / put
// ---------------------------------------------------------------------------

static void test_basic() {
    section("basic get/put");

    LRU<int, int> c(3);
    CHECK(c.empty());
    CHECK(c.size() == 0);
    CHECK(c.capacity() == 3);

    c.put(1, 10);
    c.put(2, 20);
    c.put(3, 30);
    CHECK(c.size() == 3);

    auto* v = c.get(2);
    CHECK(v != nullptr && *v == 20);

    CHECK(c.get(99) == nullptr);   // miss
}

// ---------------------------------------------------------------------------
// 2. Eviction order
//    After put(1), put(2), put(3), get(1) makes 2 the LRU.
//    put(4) must evict 2.
// ---------------------------------------------------------------------------

static void test_eviction_order() {
    section("eviction order");

    LRU<int, int> c(3);
    c.put(1, 10);
    c.put(2, 20);
    c.put(3, 30);

    // Access 1 → order is now 1(MRU), 3, 2(LRU)
    c.get(1);

    // Insert 4 → 2 must be evicted
    c.put(4, 40);
    CHECK(c.size() == 3);
    CHECK(c.get(2) == nullptr);    // evicted
    CHECK(c.get(1) != nullptr);
    CHECK(c.get(3) != nullptr);
    CHECK(c.get(4) != nullptr);
}

// ---------------------------------------------------------------------------
// 3. Update (put on existing key) refreshes recency and changes value
// ---------------------------------------------------------------------------

static void test_update() {
    section("update existing key");

    LRU<int, std::string> c(3);
    c.put(1, "a");
    c.put(2, "b");
    c.put(3, "c");

    // Update key 1 — it becomes MRU; 2 is now LRU
    c.put(1, "A");
    CHECK(*c.get(1) == "A");

    // Insert 4 — evicts 2
    c.put(4, "d");
    CHECK(c.get(2) == nullptr);
    CHECK(c.get(1) != nullptr);
    CHECK(c.get(3) != nullptr);
    CHECK(c.get(4) != nullptr);
}

// ---------------------------------------------------------------------------
// 4. Erase
// ---------------------------------------------------------------------------

static void test_erase() {
    section("erase");

    LRU<int, int> c(4);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);

    CHECK(c.erase(2) == true);
    CHECK(c.size() == 2);
    CHECK(c.get(2) == nullptr);

    // Erasing a missing key returns false
    CHECK(c.erase(99) == false);

    // After erase, the remaining LRU order is still correct.
    // 1 was inserted first, 3 second; inserting two more should evict 1.
    c.put(4, 4);
    c.put(5, 5);
    CHECK(c.get(1) == nullptr);   // evicted
    CHECK(c.get(3) != nullptr);
    CHECK(c.get(4) != nullptr);
    CHECK(c.get(5) != nullptr);
}

// ---------------------------------------------------------------------------
// 5. Single-entry cache (capacity == 1)
// ---------------------------------------------------------------------------

static void test_capacity_one() {
    section("capacity 1");

    LRU<int, int> c(1);
    c.put(1, 100);
    CHECK(*c.get(1) == 100);

    c.put(2, 200);
    CHECK(c.get(1) == nullptr);   // evicted
    CHECK(*c.get(2) == 200);
}

// ---------------------------------------------------------------------------
// 6. Move semantics — values should not be copied when moving into the cache
// ---------------------------------------------------------------------------

static void test_move_semantics() {
    section("move semantics");

    LRU<int, std::string> c(2);

    std::string s1 = "hello";
    std::string s2 = "world";

    // move rvalues into the cache — no copy
    c.put(1, std::move(s1));
    c.put(2, std::move(s2));

    // s1/s2 are in a valid-but-unspecified state; cache must have the values
    CHECK(*c.get(1) == "hello");
    CHECK(*c.get(2) == "world");

    // Update with another move
    std::string s3 = "updated";
    c.put(1, std::move(s3));
    CHECK(*c.get(1) == "updated");
}

// ---------------------------------------------------------------------------
// 7. contains / peek (do not update recency)
// ---------------------------------------------------------------------------

static void test_contains_peek() {
    section("contains and peek");

    LRU<int, int> c(2);
    c.put(1, 10);
    c.put(2, 20);

    CHECK(c.contains(1));
    CHECK(!c.contains(99));

    // peek must not update recency: 1 is currently LRU; peek it, then insert
    // 3 — 1 should still be evicted.
    c.peek(1);
    c.put(3, 30);
    CHECK(c.get(1) == nullptr);   // still evicted — peek didn't refresh
    CHECK(c.get(2) != nullptr);
    CHECK(c.get(3) != nullptr);
}

// ---------------------------------------------------------------------------
// 8. Clear
// ---------------------------------------------------------------------------

static void test_clear() {
    section("clear");

    LRU<int, int> c(3);
    c.put(1, 1);
    c.put(2, 2);
    c.clear();
    CHECK(c.empty());
    CHECK(c.size() == 0);

    // Reuse after clear
    c.put(10, 100);
    CHECK(*c.get(10) == 100);
}

// ---------------------------------------------------------------------------
// 9. String keys
// ---------------------------------------------------------------------------

static void test_string_keys() {
    section("string keys");

    LRU<std::string, int> c(3);
    c.put("alpha",   1);
    c.put("beta",    2);
    c.put("gamma",   3);

    CHECK(*c.get("alpha") == 1);
    CHECK(*c.get("beta")  == 2);

    // gamma is LRU after two gets above; inserting delta evicts gamma
    c.put("delta", 4);
    CHECK(c.get("gamma") == nullptr);
    CHECK(c.get("delta") != nullptr);
}

// ---------------------------------------------------------------------------
// 10. Zero-capacity throws
// ---------------------------------------------------------------------------

static void test_zero_capacity() {
    section("zero capacity throws");

    bool threw = false;
    try {
        LRU<int, int> c(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// 11. Stress — 10 000-entry loop
//
//     Cache capacity 100.  Write keys 0..9999 in order, then verify:
//       - The last 100 keys are present.
//       - Keys before that are absent (evicted).
//     Then read the last 100 back through get() (updating recency) and
//     overwrite with a second pass to confirm values are consistent.
// ---------------------------------------------------------------------------

static void test_stress() {
    section("stress 10000");

    const int   N    = 10000;
    const std::size_t CAP  = 100;

    LRU<int, int> c(CAP);

    // Fill
    for (int i = 0; i < N; ++i)
        c.put(i, i * 2);

    CHECK(c.size() == CAP);

    // Last CAP keys must be present
    for (int i = N - static_cast<int>(CAP); i < N; ++i) {
        auto* v = c.get(i);
        CHECK(v != nullptr);
        if (v) CHECK(*v == i * 2);
    }

    // Keys before that must have been evicted
    for (int i = 0; i < N - static_cast<int>(CAP); ++i)
        CHECK(c.get(i) == nullptr);

    // Now access the surviving keys in reverse to change their recency,
    // then do a second overwrite pass and verify values update.
    for (int i = N - 1; i >= N - static_cast<int>(CAP); --i)
        c.get(i);   // refreshes recency

    for (int i = N - static_cast<int>(CAP); i < N; ++i)
        c.put(i, i * 3);

    for (int i = N - static_cast<int>(CAP); i < N; ++i) {
        auto* v = c.get(i);
        CHECK(v != nullptr);
        if (v) CHECK(*v == i * 3);
    }

    // Size must still be exactly CAP
    CHECK(c.size() == CAP);
}

// ---------------------------------------------------------------------------
// 12. Stress — mixed put / erase / get with rolling window
// ---------------------------------------------------------------------------

static void test_stress_mixed() {
    section("stress mixed put/erase/get");

    const int         N   = 10000;
    const std::size_t CAP = 64;

    LRU<int, int> c(CAP);

    // Simple LCG to vary the access pattern
    unsigned rng = 0xdeadbeef;
    auto next = [&]() -> int {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<int>(rng & 0x7fffffff);
    };

    for (int i = 0; i < N; ++i) {
        int key = next() % (static_cast<int>(CAP) * 4);
        int op  = next() % 3;

        if (op == 0) {
            c.erase(key);
        } else if (op == 1) {
            c.put(key, key);
        } else {
            c.get(key);   // may miss — that's fine
        }

        // Invariant: size never exceeds capacity
        CHECK(c.size() <= CAP);
    }

    // After all operations, every key reported by contains must be gettable
    // We can't enumerate keys directly, so we do a probe on a known range.
    for (int k = 0; k < static_cast<int>(CAP) * 4; ++k) {
        bool present  = c.contains(k);
        bool gettable = (c.get(k) != nullptr);
        CHECK(present == gettable);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    test_basic();
    test_eviction_order();
    test_update();
    test_erase();
    test_capacity_one();
    test_move_semantics();
    test_contains_peek();
    test_clear();
    test_string_keys();
    test_zero_capacity();
    test_stress();
    test_stress_mixed();

    summary();
}
