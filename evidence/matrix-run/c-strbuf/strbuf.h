/*
 * strbuf.h — growable string builder (C11)
 *
 * Usage:
 *   strbuf sb;
 *   sb_init(&sb);
 *   sb_appends(&sb, "hello, ");
 *   sb_appendf(&sb, "%s!\n", "world");
 *   printf("%s", sb_cstr(&sb));
 *   sb_free(&sb);
 */

#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *data;   /* null-terminated buffer              */
    size_t  len;    /* number of bytes written (not incl. NUL) */
    size_t  cap;    /* allocated capacity (always > len)   */
} strbuf;

/*
 * sb_init  — initialise a zero-length buffer.
 *            Returns 0 on success, -1 on allocation failure.
 */
int  sb_init(strbuf *sb);

/*
 * sb_free  — release memory and reset the struct.
 *            Safe to call on an already-freed buffer.
 */
void sb_free(strbuf *sb);

/*
 * sb_clear — reset length to zero without freeing memory.
 *            The existing capacity is preserved.
 */
void sb_clear(strbuf *sb);

/*
 * sb_append  — append 'len' bytes from 'data' (may contain NUL bytes).
 *              Returns 0 on success, -1 on allocation failure.
 */
int  sb_append(strbuf *sb, const char *data, size_t len);

/*
 * sb_appends — append a NUL-terminated string.
 *              Returns 0 on success, -1 on allocation failure.
 */
int  sb_appends(strbuf *sb, const char *str);

/*
 * sb_appendf — printf-style append.
 *              Returns 0 on success, -1 on allocation failure or encode error.
 */
int  sb_appendf(strbuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * sb_cstr — return the current null-terminated contents.
 *           The pointer is valid until the next mutation.
 *           Never returns NULL (returns "" for an empty buffer).
 */
const char *sb_cstr(const strbuf *sb);

#ifdef __cplusplus
}
#endif

#endif /* STRBUF_H */
