/*
 * strbuf.c — growable string builder (C11)
 */

#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimum initial capacity, including the null terminator. */
#define STRBUF_INIT_CAP 64

/* --------------------------------------------------------------------------
 * Internal helper: ensure at least `needed` bytes of free space (not
 * counting what is already used).  Grows by doubling.  Returns 0 on success
 * or -1 if realloc fails (the original buffer is left intact).
 * -------------------------------------------------------------------------- */
static int sb_grow(strbuf_t *sb, size_t needed)
{
    /* total bytes required: existing length + new data + null terminator */
    size_t required = sb->len + needed + 1;
    if (required <= sb->cap)
        return 0;

    size_t new_cap = sb->cap ? sb->cap : STRBUF_INIT_CAP;
    while (new_cap < required)
        new_cap *= 2;

    char *p = realloc(sb->data, new_cap);
    if (!p)
        return -1;

    sb->data = p;
    sb->cap  = new_cap;
    return 0;
}

/* --------------------------------------------------------------------------
 * sb_init — allocate an empty buffer.
 * -------------------------------------------------------------------------- */
int sb_init(strbuf_t *sb)
{
    sb->data = malloc(STRBUF_INIT_CAP);
    if (!sb->data) {
        sb->len = 0;
        sb->cap = 0;
        return -1;
    }
    sb->data[0] = '\0';
    sb->len     = 0;
    sb->cap     = STRBUF_INIT_CAP;
    return 0;
}

/* --------------------------------------------------------------------------
 * sb_free — release memory and zero the struct so it is safe to inspect.
 * -------------------------------------------------------------------------- */
void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

/* --------------------------------------------------------------------------
 * sb_append — append a single character.
 * -------------------------------------------------------------------------- */
int sb_append(strbuf_t *sb, char c)
{
    if (sb_grow(sb, 1) != 0)
        return -1;

    sb->data[sb->len]     = c;
    sb->data[sb->len + 1] = '\0';
    sb->len++;
    return 0;
}

/* --------------------------------------------------------------------------
 * sb_appends — append a null-terminated string.
 * -------------------------------------------------------------------------- */
int sb_appends(strbuf_t *sb, const char *s)
{
    if (!s)
        return 0;

    size_t slen = strlen(s);
    if (slen == 0)
        return 0;

    if (sb_grow(sb, slen) != 0)
        return -1;

    memcpy(sb->data + sb->len, s, slen + 1); /* +1 copies the '\0' */
    sb->len += slen;
    return 0;
}

/* --------------------------------------------------------------------------
 * sb_appendf — append a printf-style formatted string.
 *
 * Uses vsnprintf twice:
 *   1. With a NULL buffer and size 0 to measure the number of bytes needed.
 *   2. After growing the buffer, to write the formatted output.
 * -------------------------------------------------------------------------- */
int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;

    /* First pass: measure. */
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0)
        return -1; /* encoding error */

    if (n == 0)
        return 0;  /* nothing to append */

    if (sb_grow(sb, (size_t)n) != 0)
        return -1;

    /* Second pass: write. */
    va_start(ap, fmt);
    int written = vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);

    if (written < 0)
        return -1;

    sb->len += (size_t)written;
    return 0;
}

/* --------------------------------------------------------------------------
 * sb_clear — reset length to 0, keep the allocation.
 * -------------------------------------------------------------------------- */
void sb_clear(strbuf_t *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

/* --------------------------------------------------------------------------
 * sb_cstr — return a pointer to the null-terminated contents.
 * -------------------------------------------------------------------------- */
const char *sb_cstr(const strbuf_t *sb)
{
    return sb->data;
}
