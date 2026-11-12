#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>

#define RB_CAPACITY 4096

/*
 * Single-producer / single-consumer ring buffer with a fixed 4096-byte backing
 * array.  No dynamic allocation.  Uses monotonically increasing head/tail
 * counters (never wrapping) so that available-space arithmetic is plain
 * subtraction and ABA is impossible.  The actual slot index is counter %
 * RB_CAPACITY.
 *
 * Memory ordering follows the standard SPSC acquire/release pattern:
 *   - Producer owns head, loads tail with acquire, stores head with release.
 *   - Consumer owns tail, loads head with acquire, stores tail with release.
 */
typedef struct {
    unsigned char data[RB_CAPACITY];
    _Atomic size_t head;          /* producer write counter */
    _Atomic size_t tail;          /* consumer read counter  */
} ringbuf_t;

/* Zero the counters.  Returns 0 on success, -1 on NULL. */
int rb_init(ringbuf_t *rb);

/* Copy len bytes from src into the buffer (all-or-nothing).
 * Returns 0 on success, -1 if NULL or len exceeds available space. */
int rb_push(ringbuf_t *rb, const void *src, size_t len);

/* Copy len bytes out of the buffer into dst and consume them.
 * Returns 0 on success, -1 if NULL or fewer than len bytes are available. */
int rb_pop(ringbuf_t *rb, void *dst, size_t len);

/* Copy len bytes out of the buffer into dst without consuming them.
 * Returns 0 on success, -1 if NULL or fewer than len bytes are available. */
int rb_peek(ringbuf_t *rb, void *dst, size_t len);

/* Number of bytes currently available to read. */
int rb_len(ringbuf_t *rb);

/* Number of bytes currently available to write. */
int rb_space(ringbuf_t *rb);

#endif /* RINGBUF_H */
