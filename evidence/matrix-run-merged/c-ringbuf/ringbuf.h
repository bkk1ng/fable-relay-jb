/*
 * ringbuf.h — fixed 4096-byte SPSC ring buffer, C11, no dynamic allocation.
 *
 * All functions return 0 on success, -1 on error (null pointer, full, empty).
 * rb_len and rb_space return the byte count / free space as a non-negative int,
 * or -1 on a null pointer.
 *
 * Thread safety: single-producer / single-consumer only.
 * rb_push must be called from one thread; rb_pop and rb_peek from one other.
 * rb_len and rb_space are safe to call from either end but give a snapshot.
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RB_CAPACITY 4096U

/*
 * Embed head and tail in separate cache-line-sized regions to avoid false
 * sharing between producer and consumer cores.  The 64-byte alignment covers
 * every common architecture; adjust RB_CACHELINE if yours differs.
 */
#define RB_CACHELINE 64U

struct ringbuf {
    uint8_t buf[RB_CAPACITY];

    /* consumer advances head; align to its own cache line */
    _Alignas(RB_CACHELINE) _Atomic size_t head;

    /* producer advances tail; align to its own cache line */
    _Alignas(RB_CACHELINE) _Atomic size_t tail;
};

/*
 * rb_init  — zero the buffer and reset indices.
 * rb_push  — write one byte; fails when the buffer is full.
 * rb_pop   — read and remove one byte; fails when empty.
 * rb_peek  — read without removing; fails when empty.
 * rb_len   — return bytes available to consume, or -1 on null.
 * rb_space — return bytes available to produce, or -1 on null.
 */
int rb_init(struct ringbuf *rb);
int rb_push(struct ringbuf *rb, uint8_t byte);
int rb_pop(struct ringbuf *rb, uint8_t *out);
int rb_peek(struct ringbuf *rb, uint8_t *out);
int rb_len(struct ringbuf *rb);
int rb_space(struct ringbuf *rb);

#endif /* RINGBUF_H */
