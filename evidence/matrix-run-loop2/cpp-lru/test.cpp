// test.cpp — exercises LRU<K,V> from lru.hpp
//
// Build:  g++ -std=c++17 -Wall -Wextra -O2 test.cpp -o lru_test
// Run:    ./lru_test
//
// Exits non-zero on any assertion failure.

#include "lru.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// tiny assert harness
// ---------------------------------------------------------------------------
static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        ++g_checks;                                              \
        if (!(cond)) {                                           \
            ++g_failures;                                        \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ \
                      << "  " #cond "\n";                        \
        }                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                           \
    do {                                                         \
        ++g_checks;                                              \
        auto _a = (a);                                           \
        auto _b = (b);                                           \
        if (!(_a == _b)) {                                       \
            ++g_failures;                                        \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ \
                      << "  " #a " == " #b "\n"                  \
                      << "    got    " << _a << "\n"             \
                      << "    expect " << _b << "\n";            \
        }                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// 1. basic eviction order
// ---------------------------------------------------------------------------
static void test_eviction_order()
{
    LRU<int, int> cache(3);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);
    CHECK_EQ(cache.size(), std::size_t{3});

    // touch key 1 -> now MRU; order back-to-front: 2, 3, 1
    CHECK_EQ(cache.get(1).value_or(-1), 10);

    // inserting 4 evicts LRU (key 2)
    cache.put(4, 40);
    CHECK_EQ(cache.size(), std::size_t{3});
    CHECK(!cache.get(2).has_value());
    CHECK_EQ(cache.get(1).value_or(-1), 10);
    CHECK_EQ(cache.get(3).value_or(-1), 30);
    CHECK_EQ(cache.get(4).value_or(-1), 40);
}

// ---------------------------------------------------------------------------
// 2. put on existing key updates value and promotes to MRU
// ---------------------------------------------------------------------------
static void test_update_promotes()
{
    LRU<int, std::string> cache(2);

    cache.put(1, "a");
    cache.put(2, "b");

    // update key 1 -> now MRU, key 2 is LRU
    cache.put(1, "aa");
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK_EQ(cache.get(1).value_or(""), std::string{"aa"});

    // inserting 3 evicts key 2
    cache.put(3, "c");
    CHECK(!cache.get(2).has_value());
    CHECK_EQ(cache.get(1).value_or(""), std::string{"aa"});
    CHECK_EQ(cache.get(3).value_or(""), std::string{"c"});
}

// ---------------------------------------------------------------------------
// 3. erase
// ---------------------------------------------------------------------------
static void test_erase()
{
    LRU<int, int> cache(4);

    cache.put(1, 1);
    cache.put(2, 2);
    cache.put(3, 3);

    CHECK_EQ(cache.erase(2), true);
    CHECK_EQ(cache.erase(2), false);   // already gone
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK(!cache.get(2).has_value());
    CHECK_EQ(cache.get(1).value_or(-1), 1);
    CHECK_EQ(cache.get(3).value_or(-1), 3);

    // freed slot: insert 4 without triggering eviction
    cache.put(4, 4);
    CHECK_EQ(cache.size(), std::size_t{3});
    CHECK_EQ(cache.get(4).value_or(-1), 4);
}

// ---------------------------------------------------------------------------
// 4. cache miss leaves order undisturbed
// ---------------------------------------------------------------------------
static void test_miss_does_not_disturb()
{
    LRU<int, int> cache(2);

    cache.put(1, 1);
    cache.put(2, 2);

    // miss on absent key — order stays: LRU=1, MRU=2
    CHECK(!cache.get(99).has_value());

    // inserting 3 must evict 1 (still LRU)
    cache.put(3, 3);
    CHECK(!cache.get(1).has_value());
    CHECK_EQ(cache.get(2).value_or(-1), 2);
    CHECK_EQ(cache.get(3).value_or(-1), 3);
}

// ---------------------------------------------------------------------------
// 5. move semantics
// ---------------------------------------------------------------------------
static void test_move_semantics()
{
    LRU<std::string, std::string> cache(2);

    std::string k = "k1";
    std::string v = "a string long enough to avoid SSO on most platforms";

    cache.put(std::move(k), std::move(v));
    CHECK_EQ(cache.get("k1").value_or(""), std::string{"a string long enough to avoid SSO on most platforms"});

    // move-construct the cache itself
    LRU<std::string, std::string> other(std::move(cache));
    CHECK_EQ(other.size(), std::size_t{1});
    CHECK_EQ(other.get("k1").value_or(""), std::string{"a string long enough to avoid SSO on most platforms"});
}

// ---------------------------------------------------------------------------
// 6. stress — 10 000 sequential inserts into capacity-1000 cache
// ---------------------------------------------------------------------------
static void test_stress()
{
    constexpr std::size_t CAP    = 1000;
    constexpr int         TOTAL  = 10000;

    LRU<int, int> cache(CAP);

    // fill to capacity
    for (int i = 0; i < static_cast<int>(CAP); ++i)
        cache.put(i, i * 10);

    CHECK_EQ(cache.size(), CAP);

    for (int i = 0; i < static_cast<int>(CAP); ++i)
        CHECK_EQ(cache.get(i).value_or(-1), i * 10);

    // stream in keys CAP..TOTAL-1; each evicts the oldest untouched key
    for (int k = static_cast<int>(CAP); k < TOTAL; ++k) {
        cache.put(k, k * 10);
        CHECK(cache.size() <= CAP);
    }

    CHECK_EQ(cache.size(), CAP);

    // only keys 9000..9999 should survive
    CHECK(!cache.get(8999).has_value());
    CHECK_EQ(cache.get(9000).value_or(-1), 90000);
    CHECK_EQ(cache.get(9999).value_or(-1), 99990);

    // touch every 3rd key in 9000..9999, then insert one more
    // -> the first untouched key (9001) must be evicted
    for (int i = 9000; i < TOTAL; i += 3)
        cache.get(i);

    cache.put(100000, -1);
    CHECK(!cache.get(9001).has_value());
    CHECK_EQ(cache.get(9000).value_or(-1), 90000);  // was touched, survives

    // erase everything and verify size tracking
    std::size_t erased = 0;
    for (int i = 9000; i <= TOTAL; ++i)
        erased += cache.erase(i) ? 1u : 0u;
    erased += cache.erase(100000) ? 1u : 0u;

    CHECK_EQ(cache.size(), CAP - erased);
}

// ---------------------------------------------------------------------------
// runner
// ---------------------------------------------------------------------------
int main()
{
    test_eviction_order();
    test_update_promotes();
    test_erase();
    test_miss_does_not_disturb();
    test_move_semantics();
    test_stress();

    std::cout << "checks: " << g_checks
              << "  failures: " << g_failures << '\n';

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
