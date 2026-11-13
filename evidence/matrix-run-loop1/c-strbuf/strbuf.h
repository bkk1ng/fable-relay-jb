/*
 * strbuf.h — growable string builder (C11)
 *
 * Usage:
 *   StrBuf sb;
 *   sb_init(&sb);
 *   sb_appends(&sb, "hello");
 *   sb_appendf(&sb, ", %s!", "world");
 *   printf("%s\n", sb_cstr(&sb));
 *   sb_free(&sb);
 */

#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

typedef struct {
    char   *data;   /* NUL-terminated buffer, NULL before first allocation */
    size_t  len;    /* number of bytes written, excluding NUL              */
    size_t  cap;    /* allocated capacity, including space for NUL         */
} StrBuf;

/*
 * sb_init  — zero-initialise; no allocation until first append.
 */
void sb_init(StrBuf *sb);

/*
 * sb_free  — release memory; leaves sb in a safe zero state.
 */
void sb_free(StrBuf *sb);

/*
 * sb_clear — reset length to zero without freeing memory; content is gone.
 */
void sb_clear(StrBuf *sb);

/*
 * sb_cstr  — return a NUL-terminated C string.
 *            Returns "" (a static empty string) on a zero-length / uninitialised buffer.
 */
const char *sb_cstr(const StrBuf *sb);

/*
 * sb_append  — append 'len' bytes from 'data' (may contain embedded NULs).
 * Returns 0 on success, -1 on allocation failure.
 */
int sb_append(StrBuf *sb, const char *data, size_t len);

/*
 * sb_appends — append a NUL-terminated string.
 * Returns 0 on success, -1 on allocation failure.
 */
int sb_appends(StrBuf *sb, const char *s);

/*
 * sb_appendf — append a printf-style formatted string.
 * Returns 0 on success, -1 on allocation failure or encode error.
 */
int sb_appendf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* STRBUF_H */
