/**
 * strbuf.c — growable string builder (C11)
 *
 * Build:  cc -std=c11 -Wall -Wextra -O2 -c strbuf.c
 * Test:   cc -std=c11 -Wall -Wextra -O2 strbuf.c test_strbuf.c -o strbuf_test
 */
#include "strbuf.h"

#include <stdio.h>   /* vsnprintf       */
#include <stdlib.h>  /* malloc, realloc, free */
#include <string.h>  /* strlen, memcpy  */
#include <stdarg.h>  /* va_list family  */
#include <stdint.h>  /* SIZE_MAX         */

/* Initial capacity when sb_init is called with hint == 0. */
#define SB_DEFAULT_CAP 16

/* ── internal: ensure cap >= need + 1 (content bytes + NUL) ─────────────── */

static int sb_ensure(StrBuf *sb, size_t need)
{
    /* Guard against need + 1 overflowing SIZE_MAX. */
    if (need > SIZE_MAX - 1)
        return -1;

    size_t target = need + 1;          /* total bytes required */

    if (target <= sb->cap)
        return 0;                       /* already big enough */

    /* Doubling growth: start from current cap (or default if zero). */
    size_t newcap = sb->cap ? sb->cap : SB_DEFAULT_CAP;

    while (newcap < target) {
        if (newcap > (SIZE_MAX / 2)) {  /* doubling would overflow */
            newcap = target;
            break;
        }
        newcap *= 2;
    }

    char *nb = realloc(sb->buf, newcap);
    if (!nb)
        return -1;

    sb->buf = nb;
    sb->cap = newcap;
    return 0;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int sb_init(StrBuf *sb, size_t hint)
{
    if (!sb)
        return -1;

    size_t cap = hint > 0 ? hint : SB_DEFAULT_CAP;
    if (cap < 1)
        cap = 1;                        /* at least room for NUL */

    char *buf = malloc(cap);
    if (!buf) {
        sb->buf = NULL;
        sb->len = 0;
        sb->cap = 0;
        return -1;
    }

    buf[0]   = '\0';
    sb->buf  = buf;
    sb->len  = 0;
    sb->cap  = cap;
    return 0;
}

void sb_free(StrBuf *sb)
{
    if (!sb)
        return;
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_clear(StrBuf *sb)
{
    if (!sb || !sb->buf)
        return;
    sb->len    = 0;
    sb->buf[0] = '\0';
}

const char *sb_cstr(const StrBuf *sb)
{
    if (!sb || !sb->buf)
        return NULL;
    return sb->buf;
}

int sb_append(StrBuf *sb, char c)
{
    if (!sb || !sb->buf)
        return -1;

    if (sb_ensure(sb, sb->len + 1) < 0)
        return -1;

    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
    return 0;
}

int sb_appends(StrBuf *sb, const char *s)
{
    if (!sb || !sb->buf || !s)
        return -1;

    size_t slen = strlen(s);

    if (sb_ensure(sb, sb->len + slen) < 0)
        return -1;

    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
    return 0;
}

int sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    if (!sb || !sb->buf || !fmt)
        return -1;

    va_list ap, ap_copy;
    va_start(ap, fmt);
    va_copy(ap_copy, ap);

    /* First vsnprintf: measure required length (excludes NUL). */
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        va_end(ap_copy);
        return -1;
    }

    size_t need = (size_t)n;

    /* Make room for need content bytes + the existing NUL is already
       counted by sb_ensure (it reserves need + 1 total).  */
    if (sb_ensure(sb, sb->len + need) < 0) {
        va_end(ap_copy);
        return -1;
    }

    /* Second vsnprintf: write for real into the reserved slot. */
    int w = vsnprintf(sb->buf + sb->len, need + 1, fmt, ap_copy);
    va_end(ap_copy);

    if (w < 0)
        return -1;

    sb->len += (size_t)w;
    /* NUL already placed by vsnprintf. */
    return 0;
}
