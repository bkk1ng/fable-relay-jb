/*
 * strbuf.h — growable string builder (C11)
 *
 * All mutating functions return 0 on success, -1 on allocation failure.
 * The buffer is always null-terminated after every successful operation.
 */

#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

typedef struct {
    char   *data;   /* null-terminated character buffer          */
    size_t  len;    /* number of characters, not counting '\0'   */
    size_t  cap;    /* allocated bytes (always > len when valid) */
} strbuf_t;

/*
 * sb_init   — initialise *sb and allocate an empty buffer.
 * sb_free   — release memory; leaves *sb in a safe zeroed state.
 * sb_append — append a single character.
 * sb_appends— append a null-terminated string.
 * sb_appendf— append a printf-style formatted string;
 *              calls vsnprintf twice: once to measure, once to write.
 * sb_clear  — reset length to 0, keep the allocation.
 * sb_cstr   — return a pointer to the null-terminated contents.
 */
int         sb_init(strbuf_t *sb);
void        sb_free(strbuf_t *sb);
int         sb_append(strbuf_t *sb, char c);
int         sb_appends(strbuf_t *sb, const char *s);
int         sb_appendf(strbuf_t *sb, const char *fmt, ...);
void        sb_clear(strbuf_t *sb);
const char *sb_cstr(const strbuf_t *sb);

#endif /* STRBUF_H */
