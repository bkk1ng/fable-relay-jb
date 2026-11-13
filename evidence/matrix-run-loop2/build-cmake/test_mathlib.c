#include "mathlib.h"

#include <stdio.h>

static int g_checks_run = 0;
static int g_checks_failed = 0;

#define CHECK(expr)                                      \
    do {                                                 \
        g_checks_run++;                                  \
        if (!(expr)) {                                   \
            g_checks_failed++;                           \
            fprintf(stderr, "FAIL: %s (%s:%d)\n",        \
                    #expr, __FILE__, __LINE__);           \
        }                                                \
    } while (0)

int main(void)
{
    /* add */
    CHECK(math_add(2, 3) == 5);
    CHECK(math_add(-1, 1) == 0);
    CHECK(math_add(0, 0) == 0);
    CHECK(math_add(-5, -7) == -12);

    /* mul */
    CHECK(math_mul(2, 3) == 6);
    CHECK(math_mul(-2, 3) == -6);
    CHECK(math_mul(0, 999) == 0);
    CHECK(math_mul(-4, -5) == 20);

    /* clamp */
    CHECK(math_clamp(5, 0, 10) == 5);
    CHECK(math_clamp(-1, 0, 10) == 0);
    CHECK(math_clamp(11, 0, 10) == 10);
    CHECK(math_clamp(0, 0, 10) == 0);
    CHECK(math_clamp(10, 0, 10) == 10);
    CHECK(math_clamp(7, 7, 7) == 7);

    printf("%d checks run, %d failed\n", g_checks_run, g_checks_failed);
    return g_checks_failed == 0 ? 0 : 1;
}
