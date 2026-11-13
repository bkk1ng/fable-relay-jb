/*
 * strbuf_test.c — basic tests for strbuf (C11, no external test framework)
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
 *      strbuf.c strbuf_test.c -o strbuf_test
 * Run:
 *   ./strbuf_test
 */

#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Minimal test harness
 * -------------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label)                                          \
    do {                                                            \
        if (cond) {                                                 \
            printf("  PASS  %s\n", label);                         \
            ++g_pass;                                               \
        } else {                                                    \
            printf("  FAIL  %s  (line %d)\n", label, __LINE__);    \
            ++g_fail;                                               \
        }                                                           \
    } while (0)

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

static void test_init_and_cstr(void)
{
    puts("--- init / cstr");
    StrBuf sb;
    sb_init(&sb);

    /* Before any append, cstr must return a valid empty string, not NULL. */
    const char *s = sb_cstr(&sb);
    CHECK(s != NULL,          "cstr non-null on empty buffer");
    CHECK(s[0] == '\0',       "cstr is empty string on fresh buffer");
    CHECK(sb.len == 0,        "len == 0 after init");

    sb_free(&sb);
}

static void test_appends_basic(void)
{
    puts("--- appends basic");
    StrBuf sb;
    sb_init(&sb);

    CHECK(sb_appends(&sb, "hello") == 0,          "appends hello ok");
    CHECK(sb.len == 5,                             "len == 5");
    CHECK(strcmp(sb_cstr(&sb), "hello") == 0,     "content == hello");

    CHECK(sb_appends(&sb, ", world") == 0,         "appends , world ok");
    CHECK(sb.len == 12,                            "len == 12");
    CHECK(strcmp(sb_cstr(&sb), "hello, world") == 0, "content == hello, world");

    sb_free(&sb);
}

static void test_append_binary(void)
{
    puts("--- append with embedded NUL");
    StrBuf sb;
    sb_init(&sb);

    /* Append 5 bytes: 'a', NUL, 'b', NUL, 'c' */
    const char data[] = { 'a', '\0', 'b', '\0', 'c' };
    CHECK(sb_append(&sb, data, 5) == 0, "append binary ok");
    CHECK(sb.len == 5,                  "len == 5");
    /* Verify individual bytes. */
    CHECK(sb.data[0] == 'a',  "byte 0 == a");
    CHECK(sb.data[1] == '\0', "byte 1 == NUL");
    CHECK(sb.data[2] == 'b',  "byte 2 == b");
    CHECK(sb.data[4] == 'c',  "byte 4 == c");
    /* Sentinel NUL must follow the payload. */
    CHECK(sb.data[5] == '\0', "sentinel NUL present");

    sb_free(&sb);
}

static void test_appendf(void)
{
    puts("--- appendf");
    StrBuf sb;
    sb_init(&sb);

    CHECK(sb_appendf(&sb, "%d + %d = %d", 1, 2, 3) == 0, "appendf ok");
    CHECK(strcmp(sb_cstr(&sb), "1 + 2 = 3") == 0,         "content == 1 + 2 = 3");

    /* Chain: append more after appendf. */
    CHECK(sb_appends(&sb, "!") == 0,                      "appends ! ok");
    CHECK(strcmp(sb_cstr(&sb), "1 + 2 = 3!") == 0,        "chained content ok");

    sb_free(&sb);
}

static void test_appendf_large(void)
{
    puts("--- appendf large (> initial capacity)");
    StrBuf sb;
    sb_init(&sb);

    /* Build a string well over 16 bytes to exercise the growth path. */
    int rc = sb_appendf(&sb, "%-40s|%-40s", "left-padded", "right-padded");
    CHECK(rc == 0,          "appendf large ok");
    CHECK(sb.len == 81,     "len == 81");
    CHECK(sb.cap > 81,      "cap > len");

    sb_free(&sb);
}

static void test_clear(void)
{
    puts("--- clear");
    StrBuf sb;
    sb_init(&sb);

    sb_appends(&sb, "some content");
    size_t old_cap = sb.cap;

    sb_clear(&sb);
    CHECK(sb.len == 0,                  "len == 0 after clear");
    CHECK(sb.cap == old_cap,            "cap unchanged after clear");
    CHECK(sb_cstr(&sb)[0] == '\0',      "cstr empty after clear");

    /* Must be reusable after clear. */
    CHECK(sb_appends(&sb, "new") == 0,  "appends after clear ok");
    CHECK(strcmp(sb_cstr(&sb), "new") == 0, "content == new after clear");

    sb_free(&sb);
}

static void test_growth_doubling(void)
{
    puts("--- growth / doubling");
    StrBuf sb;
    sb_init(&sb);

    /*
     * Append one byte at a time; verify len, cap ≥ len+1,
     * and that the string is always correctly NUL-terminated.
     */
    for (int i = 0; i < 128; ++i) {
        char c = (char)('a' + (i % 26));
        int rc = sb_append(&sb, &c, 1);
        if (rc != 0) {
            printf("  FAIL  growth step %d returned -1\n", i);
            ++g_fail;
            sb_free(&sb);
            return;
        }
    }

    CHECK(sb.len == 128,             "len == 128");
    CHECK(sb.cap >= 129,             "cap >= 129");
    CHECK(sb.data[128] == '\0',      "sentinel NUL at len");

    /* Capacity should be a power-of-two (doubling from 16). */
    CHECK((sb.cap & (sb.cap - 1)) == 0, "cap is power of two");

    sb_free(&sb);
}

static void test_free_idempotent(void)
{
    puts("--- free idempotent");
    StrBuf sb;
    sb_init(&sb);
    sb_appends(&sb, "data");
    sb_free(&sb);

    CHECK(sb.data == NULL, "data NULL after free");
    CHECK(sb.len  == 0,    "len  == 0 after free");
    CHECK(sb.cap  == 0,    "cap  == 0 after free");

    /* Second free must not crash (safe because data is NULL). */
    sb_free(&sb);
    CHECK(1, "double free did not crash");
}

static void test_null_appends(void)
{
    puts("--- appends(NULL) is a no-op");
    StrBuf sb;
    sb_init(&sb);
    sb_appends(&sb, "hi");

    CHECK(sb_appends(&sb, NULL) == 0,           "appends(NULL) returns 0");
    CHECK(strcmp(sb_cstr(&sb), "hi") == 0,      "content unchanged");

    sb_free(&sb);
}

static void test_zero_length_append(void)
{
    puts("--- append zero bytes is a no-op");
    StrBuf sb;
    sb_init(&sb);
    sb_appends(&sb, "abc");

    CHECK(sb_append(&sb, "xyz", 0) == 0,       "append 0 bytes returns 0");
    CHECK(sb.len == 3,                          "len unchanged");
    CHECK(strcmp(sb_cstr(&sb), "abc") == 0,    "content unchanged");

    sb_free(&sb);
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */

int main(void)
{
    puts("=== strbuf tests ===");

    test_init_and_cstr();
    test_appends_basic();
    test_append_binary();
    test_appendf();
    test_appendf_large();
    test_clear();
    test_growth_doubling();
    test_free_idempotent();
    test_null_appends();
    test_zero_length_append();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
