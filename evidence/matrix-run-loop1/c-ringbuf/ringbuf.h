/*
 * ringbuf.h — fixed 4096-byte SPSC ring buffer (C11, no dynamic allocation)
 *
 * Thread safety: rb_push may be called from one producer thread only;
 * rb_pop and rb_peek may be called from one consumer thread only.
 * rb_init, rb_len, and rb_space are safe to call from either thread.
 *
 * All functions return 0 on success, -1 on failure.
 * rb_len and rb_space return the byte count (>= 0) or -1 on bad input.
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RINGBUF_SIZE 4096u  /* must be a power of two */

typedef struct ringbuf {
    uint8_t        buf[RINGBUF_SIZE];
    _Atomic size_t head;  /* consumer index — written only by consumer */
    _Atomic size_t tail;  /* producer index — written only by producer */
} ringbuf_t;

/*
 * rb_init — zero the buffer and reset both indices.
 * Must be called before any other operation.
 * Returns 0 on success, -1 if rb is NULL.
 */
int rb_init(ringbuf_t *rb);

/*
 * rb_push — write one byte into the buffer.
 * Returns 0 on success, -1 if the buffer is full or rb is NULL.
 */
int rb_push(ringbuf_t *rb, uint8_t byte);

/*
 * rb_pop — read and remove one byte from the buffer.
 * Returns 0 on success, -1 if the buffer is empty or either argument is NULL.
 */
int rb_pop(ringbuf_t *rb, uint8_t *out);

/*
 * rb_peek — read one byte without removing it.
 * Returns 0 on success, -1 if the buffer is empty or either argument is NULL.
 */
int rb_peek(ringbuf_t *rb, uint8_t *out);

/*
 * rb_len — return the number of bytes available to read.
 * Returns the count (0..RINGBUF_SIZE) or -1 if rb is NULL.
 */
int rb_len(ringbuf_t *rb);

/*
 * rb_space — return the number of bytes free for writing.
 * Returns the count (0..RINGBUF_SIZE) or -1 if rb is NULL.
 */
int rb_space(ringbuf_t *rb);

#endif /* RINGBUF_H */
