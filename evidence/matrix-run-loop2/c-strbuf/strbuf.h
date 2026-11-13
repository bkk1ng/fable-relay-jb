/*
 * strbuf.h — growable string builder (C11)
 *
 * All functions return 0 on success, -1 on allocation failure.
 * The buffer is always null-terminated after every mutating call.
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>  /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *data;   /* null-terminated character buffer  */
    size_t  len;    /* number of characters (excl. '\0') */
    size_t  cap;    /* allocated bytes (always > len)    */
} strbuf_t;

/*
 * sb_init  — allocate an empty buffer with an initial capacity.
 *            Must be paired with sb_free.
 */
int         sb_init(strbuf_t *sb);

/*
 * sb_free  — release the buffer.  Safe to call on a zero-initialised
 *            strbuf_t that was never passed to sb_init.
 */
void        sb_free(strbuf_t *sb);

/*
 * sb_append  — append a single character.
 */
int         sb_append(strbuf_t *sb, char c);

/*
 * sb_appends — append a null-terminated string.
 *              Passing NULL is a no-op (returns 0).
 */
int         sb_appends(strbuf_t *sb, const char *s);

/*
 * sb_appendf — printf-style append.
 *              Uses vsnprintf twice: first to measure, then to write.
 */
int         sb_appendf(strbuf_t *sb, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
            __attribute__((format(printf, 2, 3)))
#endif
            ;

/*
 * sb_clear — reset length to 0 without freeing memory.
 *            The existing capacity is retained.
 */
void        sb_clear(strbuf_t *sb);

/*
 * sb_cstr  — return the current null-terminated string.
 *            The pointer is valid until the next mutating call.
 *            Returns "" (not NULL) for a just-initialised buffer.
 */
const char *sb_cstr(const strbuf_t *sb);

#ifdef __cplusplus
}
#endif

#endif /* STRBUF_H */
