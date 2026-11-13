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
    char   *data;   /* NUL-terminated buffer, NULL before first alloc */
    size_t  len;    /* number of content bytes, excluding NUL         */
    size_t  cap;    /* allocated bytes, including room for NUL        */
} StrBuf;

/*
 * sb_init  — zero-initialise; no allocation until the first append.
 */
void sb_init(StrBuf *sb);

/*
 * sb_free  — release memory; leaves sb in a valid, re-usable zero state.
 */
void sb_free(StrBuf *sb);

/*
 * sb_append  — append a single byte.
 * Returns 0 on success, -1 on allocation failure.
 */
int sb_append(StrBuf *sb, char c);

/*
 * sb_appends — append a NUL-terminated string.
 * Returns 0 on success, -1 on allocation failure.
 */
int sb_appends(StrBuf *sb, const char *s);

/*
 * sb_appendf — printf-style formatted append.
 * Uses vsnprintf twice: once to measure, once to write.
 * Returns 0 on success, -1 on allocation failure or encode error.
 */
int sb_appendf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * sb_clear   — reset length to 0 (keeps allocated memory).
 */
void sb_clear(StrBuf *sb);

/*
 * sb_cstr    — return a pointer to the NUL-terminated contents.
 * Returns "" (a static empty string) if the buffer was never written to.
 * The pointer is valid until the next mutating call.
 */
const char *sb_cstr(const StrBuf *sb);

#endif /* STRBUF_H */
