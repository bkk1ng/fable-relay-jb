/*
 * test_mathlib.c — minimal self-checking test suite for mathlib
 * Exits non-zero on any assertion failure so CTest picks it up via FAIL_REGULAR_EXPRESSION.
 */
#include "mathlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, fmt, ...)                                  \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d — " fmt "\n",                   \
                   __FILE__, __LINE__, ##__VA_ARGS__);          \
            g_failures++;                                      \
        }                                                      \
    } while (0)

static void test_add(void)
{
    CHECK(ml_add(2, 3) == 5, "2 + 3 should be 5, got %d", ml_add(2, 3));
    CHECK(ml_add(-1, 1) == 0, "-1 + 1 should be 0");
    CHECK(ml_add(0, 0) == 0, "0 + 0 should be 0");
    CHECK(ml_add(-5, -7) == -12, "-5 + -7 should be -12");
}

static void test_mul(void)
{
    CHECK(ml_mul(2, 3) == 6, "2 * 3 should be 6");
    CHECK(ml_mul(0, 42) == 0, "0 * 42 should be 0");
    CHECK(ml_mul(-3, 4) == -12, "-3 * 4 should be -12");
    CHECK(ml_mul(-2, -2) == 4, "-2 * -2 should be 4");
}

static void test_clamp(void)
{
    CHECK(ml_clamp(5, 0, 10) == 5, "5 in [0,10] should stay 5");
    CHECK(ml_clamp(-1, 0, 10) == 0, "-1 in [0,10] should clamp to 0");
    CHECK(ml_clamp(15, 0, 10) == 10, "15 in [0,10] should clamp to 10");
    CHECK(ml_clamp(0, 0, 10) == 0, "0 at lower bound should stay 0");
    CHECK(ml_clamp(10, 0, 10) == 10, "10 at upper bound should stay 10");
    CHECK(ml_clamp(-5, -10, -1) == -5, "-5 in [-10,-1] should stay -5");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    test_add();
    test_mul();
    test_clamp();

    if (g_failures > 0) {
        printf("FAIL: %d assertion(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    printf("All mathlib tests passed\n");
    return EXIT_SUCCESS;
}
