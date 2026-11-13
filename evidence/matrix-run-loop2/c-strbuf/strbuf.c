/*
 * strbuf.c — growable string builder (C11)
 */
#include "strbuf.h"

#include <stdarg.h>   /* va_list, va_start, va_end, va_copy */
#include <stdio.h>    /* vsnprintf                          */
#include <stdlib.h>   /* malloc, realloc, free              */
#include <string.h>   /* memcpy                             */

/* Initial allocation: one byte for the null terminator is the floor;
 * we start with 32 bytes of usable space to avoid trivial reallocations. */
#define SB_INIT_CAP  32u

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * sb_reserve — ensure the buffer can hold (sb->len + extra) characters plus
 * a null terminator.  Returns 0 on success, -1 on allocation failure.
 * Uses doubling growth so amortised cost stays O(1) per character.
 */
static int sb_reserve(strbuf_t *sb, size_t extra)
{
    /* required capacity: existing content + new chars + '\0' */
    size_t needed = sb->len + extra + 1u;
    if (needed <= sb->cap) {
        return 0;
    }

    /* Double until we fit; guard against overflow. */
    size_t newcap = sb->cap ? sb->cap : SB_INIT_CAP;
    while (newcap < needed) {
        if (newcap > (size_t)-1 / 2u) {
            /* overflow: try the exact fit as a last resort */
            newcap = needed;
            break;
        }
        newcap *= 2u;
    }

    char *p = (char *)realloc(sb->data, newcap);
    if (!p) {
        return -1;
    }
    sb->data = p;
    sb->cap  = newcap;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int sb_init(strbuf_t *sb)
{
    sb->data = (char *)malloc(SB_INIT_CAP);
    if (!sb->data) {
        sb->len = sb->cap = 0u;
        return -1;
    }
    sb->data[0] = '\0';
    sb->len = 0u;
    sb->cap = SB_INIT_CAP;
    return 0;
}

void sb_free(strbuf_t *sb)
{
    free(sb->data);   /* free(NULL) is defined no-op in C11 */
    sb->data = NULL;
    sb->len  = 0u;
    sb->cap  = 0u;
}

int sb_append(strbuf_t *sb, char c)
{
    if (sb_reserve(sb, 1u) != 0) {
        return -1;
    }
    sb->data[sb->len]     = c;
    sb->data[sb->len + 1] = '\0';
    sb->len++;
    return 0;
}

int sb_appends(strbuf_t *sb, const char *s)
{
    if (!s) {
        return 0;
    }
    size_t slen = strlen(s);
    if (slen == 0u) {
        return 0;
    }
    if (sb_reserve(sb, slen) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, s, slen + 1u); /* +1: copy the '\0' */
    sb->len += slen;
    return 0;
}

int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;

    /* --- pass 1: measure the formatted string ----------------------------- */
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0) {
        /* encoding error from vsnprintf */
        return -1;
    }

    size_t extra = (size_t)need;
    if (sb_reserve(sb, extra) != 0) {
        return -1;
    }

    /* --- pass 2: write into the reserved space ---------------------------- */
    va_start(ap, fmt);
    int written = vsnprintf(sb->data + sb->len, extra + 1u, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written != extra) {
        /* Shouldn't happen on a standards-conforming implementation, but
         * null-terminate defensively and report failure. */
        sb->data[sb->len] = '\0';
        return -1;
    }

    sb->len += extra;
    return 0;
}

void sb_clear(strbuf_t *sb)
{
    sb->len = 0u;
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

const char *sb_cstr(const strbuf_t *sb)
{
    /* Return a pointer to a static empty string when no allocation exists
     * (e.g. zero-initialised strbuf_t that was never passed to sb_init). */
    static const char empty = '\0';
    return sb->data ? sb->data : &empty;
}
