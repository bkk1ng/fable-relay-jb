/**
 * strbuf.h — growable string builder (C11)
 *
 * All functions return 0 on success, -1 on allocation failure.
 * sb_cstr always returns a valid NUL-terminated string (never NULL
 * after a successful sb_init).
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *buf; /* heap-allocated buffer, always NUL-terminated */
    size_t  len; /* number of content bytes, excluding NUL        */
    size_t  cap; /* allocated bytes, including space for NUL      */
} StrBuf;

/**
 * sb_init — initialise a StrBuf with an optional pre-allocated capacity.
 *
 * Pass 0 for hint to use the default initial capacity.
 * Must be paired with sb_free.
 */
int  sb_init(StrBuf *sb, size_t hint);

/** sb_free — release all memory; leaves *sb zeroed. */
void sb_free(StrBuf *sb);

/** sb_clear — reset length to 0 without releasing the buffer. */
void sb_clear(StrBuf *sb);

/** sb_cstr — return the NUL-terminated contents (never NULL after sb_init). */
const char *sb_cstr(const StrBuf *sb);

/** sb_append — append a single byte. */
int sb_append(StrBuf *sb, char c);

/** sb_appends — append a NUL-terminated string. */
int sb_appends(StrBuf *sb, const char *s);

/**
 * sb_appendf — printf-style append.
 *
 * Calls vsnprintf twice: once to measure the required space, once to write.
 * Returns 0 on success, -1 if the format is invalid or allocation fails.
 */
int sb_appendf(StrBuf *sb, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* STRBUF_H */
