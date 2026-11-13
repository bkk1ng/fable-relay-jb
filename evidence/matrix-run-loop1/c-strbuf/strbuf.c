/*
 * strbuf.c — growable string builder (C11)
 */

#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimum initial capacity (including NUL). */
#define STRBUF_MIN_CAP 16

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/*
 * grow_to — ensure at least `needed` bytes of capacity (including NUL).
 * Uses doubling growth; jumps straight to `needed` on first allocation.
 * Returns 0 on success, -1 on allocation failure.
 */
static int grow_to(StrBuf *sb, size_t needed)
{
    if (needed <= sb->cap) {
        return 0;
    }

    /* Choose new capacity: start at minimum, then double until large enough. */
    size_t newcap = (sb->cap == 0) ? STRBUF_MIN_CAP : sb->cap;
    while (newcap < needed) {
        /* Guard against overflow: if doubling would wrap, go straight to needed. */
        if (newcap > (size_t)-1 / 2) {
            newcap = needed;
            break;
        }
        newcap *= 2;
    }

    char *newdata = realloc(sb->data, newcap);
    if (!newdata) {
        return -1;
    }

    sb->data = newdata;
    sb->cap  = newcap;
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    /* Keep the buffer; just NUL-terminate in place if allocated. */
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

const char *sb_cstr(const StrBuf *sb)
{
    if (!sb->data || sb->len == 0) {
        return "";
    }
    return sb->data;
}

int sb_append(StrBuf *sb, const char *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    /* Need sb->len + len bytes of content plus one NUL. */
    if (grow_to(sb, sb->len + len + 1) != 0) {
        return -1;
    }

    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_appends(StrBuf *sb, const char *s)
{
    if (!s) {
        return 0;
    }
    return sb_append(sb, s, strlen(s));
}

int sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;

    /* First pass: measure the formatted output length. */
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) {
        /* Encoding error. */
        return -1;
    }

    /* Grow to fit sb->len + needed chars + NUL. */
    if (grow_to(sb, sb->len + (size_t)needed + 1) != 0) {
        return -1;
    }

    /* Second pass: write directly into the buffer. */
    va_start(ap, fmt);
    int written = vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);

    if (written < 0) {
        return -1;
    }

    sb->len += (size_t)written;
    /* vsnprintf already NUL-terminated. */
    return 0;
}
