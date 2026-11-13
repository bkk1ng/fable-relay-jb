// test.cpp — LRU cache test harness
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -o lru_test test.cpp
// Run:    ./lru_test
// Exits 0 on success, non-zero on assertion failure.

#include "lru.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>

// tiny assert macro that prints the failing expression and aborts
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                \
        }                                                                \
    } while (0)

static int passed = 0;

static void test_basic_get_put() {
    LRU<int, std::string> cache(3);

    CHECK(cache.empty());
    CHECK(cache.size() == 0);
    CHECK(cache.capacity() == 3);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");

    CHECK(cache.size() == 3);
    CHECK(!cache.empty());

    // all three present
    CHECK(cache.get(1) != nullptr);
    CHECK(cache.get(2) != nullptr);
    CHECK(cache.get(3) != nullptr);
    CHECK(*cache.get(1) == "one");
    CHECK(*cache.get(2) == "two");
    CHECK(*cache.get(3) == "three");

    // miss
    CHECK(cache.get(99) == nullptr);

    ++passed;
    std::printf("ok: basic get/put\n");
}

static void test_eviction_order() {
    LRU<int, int> cache(3);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);

    // access 1 -> order is now 1,3,2 (MRU to LRU)
    CHECK(*cache.get(1) == 10);

    // insert 4 -> evict LRU which is 2
    cache.put(4, 40);

    CHECK(cache.size() == 3);
    CHECK(cache.get(2) == nullptr);          // evicted
    CHECK(cache.get(1) != nullptr);          // survived
    CHECK(cache.get(3) != nullptr);          // survived
    CHECK(cache.get(4) != nullptr);          // just inserted
    CHECK(*cache.get(4) == 40);

    // now MRU order is 4,3,1 — access 1 to make it MRU
    CHECK(*cache.get(1) == 10);
    // order now 1,4,3 -> insert 5 evicts 3
    cache.put(5, 50);
    CHECK(cache.get(3) == nullptr);
    CHECK(cache.get(1) != nullptr);
    CHECK(cache.get(4) != nullptr);
    CHECK(cache.get(5) != nullptr);
    CHECK(*cache.get(5) == 50);

    ++passed;
    std::printf("ok: eviction order\n");
}

static void test_update_promotes() {
    LRU<int, int> cache(2);

    cache.put(1, 100);
    cache.put(2, 200);

    // update existing key 1 — should promote it to MRU and change value
    cache.put(1, 999);
    CHECK(*cache.get(1) == 999);
    CHECK(cache.size() == 2);

    // MRU order: 1,2 -> insert 3 evicts 2
    cache.put(3, 300);
    CHECK(cache.get(2) == nullptr);
    CHECK(cache.get(1) != nullptr);
    CHECK(cache.get(3) != nullptr);
    CHECK(*cache.get(1) == 999);
    CHECK(*cache.get(3) == 300);

    ++passed;
    std::printf("ok: update promotes and changes value\n");
}

static void test_get_promotes() {
    LRU<int, char> cache(3);

    cache.put(1, 'a');
    cache.put(2, 'b');
    cache.put(3, 'c');

    // get touches key 2, promoting it: order 2,3,1
    CHECK(*cache.get(2) == 'b');

    // insert 4 -> evict LRU which is 1
    cache.put(4, 'd');
    CHECK(cache.get(1) == nullptr);
    CHECK(cache.get(2) != nullptr);
    CHECK(cache.get(3) != nullptr);
    CHECK(cache.get(4) != nullptr);

    ++passed;
    std::printf("ok: get promotes recency\n");
}

static void test_erase() {
    LRU<int, int> cache(4);

    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);

    CHECK(cache.erase(2));
    CHECK(cache.size() == 2);
    CHECK(cache.get(2) == nullptr);
    CHECK(!cache.erase(2));            // already gone
    CHECK(!cache.erase(999));          // never existed

    // remaining keys intact
    CHECK(*cache.get(1) == 1);
    CHECK(*cache.get(3) == 3);

    // erase then re-insert works
    cache.erase(1);
    CHECK(cache.get(1) == nullptr);
    cache.put(1, 11);
    CHECK(*cache.get(1) == 11);

    ++passed;
    std::printf("ok: erase\n");
}

static void test_move_semantics() {
    LRU<int, std::string> cache(2);

    std::string heavy(128, 'x');
    const char* heavy_addr = heavy.data();

    cache.put(1, std::move(heavy));
    // after move, source string is valid but unspecified; address stored in cache
    CHECK(cache.get(1) != nullptr);
    CHECK((*cache.get(1)).size() == 128);

    // move-construct a whole cache
    LRU<int, std::string> moved(std::move(cache));
    CHECK(moved.size() == 1);
    CHECK(*moved.get(1) == std::string(128, 'x'));
    CHECK(cache.empty() == false || cache.empty() == true); // moved-from size unspecified, just don't crash

    // move-assign
    LRU<int, std::string> other(5);
    other = std::move(moved);
    CHECK(other.get(1) != nullptr);
    CHECK((*other.get(1)).size() == 128);

    // heavy string data should have been transferred, not copied
    CHECK((*other.get(1)).data() == heavy_addr);

    ++passed;
    std::printf("ok: move semantics\n");
}

static void test_stress_10000() {
    LRU<int, int> cache(500);

    // insert 10000 distinct keys; cache holds last 500
    for (int i = 0; i < 10000; ++i) {
        cache.put(i, i * i);
    }

    // size should be capped at capacity
    CHECK(cache.size() == 500);

    // the last 500 keys (9500..9999) should still be resident
    for (int i = 9500; i < 10000; ++i) {
        CHECK(cache.get(i) != nullptr);
        CHECK(*cache.get(i) == i * i);
    }

    // everything before 9500 should have been evicted
    for (int i = 0; i < 9500; ++i) {
        CHECK(cache.get(i) == nullptr);
    }

    // mixed churn: interleave inserts and gets, verify recency correctness
    cache.put(9500, -1);                  // promote 9500 to MRU
    cache.put(20000, 7);                  // evict LRU (which is now 9501)
    CHECK(cache.get(20000) != nullptr);
    CHECK(cache.get(9500) != nullptr);
    CHECK(cache.get(9501) == nullptr);    // evicted by the insert of 20000

    ++passed;
    std::printf("ok: 10000-entry stress loop\n");
}

int main() {
    test_basic_get_put();
    test_eviction_order();
    test_update_promotes();
    test_get_promotes();
    test_erase();
    test_move_semantics();
    test_stress_10000();

    std::printf("\nall %d tests passed\n", passed);
    return 0;
}
