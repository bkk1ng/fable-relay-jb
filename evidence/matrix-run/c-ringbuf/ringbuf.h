/*
 * ringbuf.h — fixed 4096-byte SPSC ring buffer (C11, no dynamic allocation)
 *
 * Thread safety: rb_push is safe from one producer thread;
 *                rb_pop / rb_peek are safe from one consumer thread.
 *                rb_len / rb_space give a consistent snapshot but may be
 *                stale by the time the caller acts on them.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stdint.h>

#define RB_CAPACITY 4096U   /* must stay a power of two */

typedef struct {
    uint8_t           buf[RB_CAPACITY];
    _Atomic uint32_t  head;   /* read  index — consumer advances */
    _Atomic uint32_t  tail;   /* write index — producer advances */
} ringbuf_t;

/*
 * rb_init  — zero the struct and reset head/tail.
 *            Returns 0 on success, -1 if rb is NULL.
 */
int rb_init(ringbuf_t *rb);

/*
 * rb_push  — write one byte (producer side).
 *            Returns  0 on success,
 *                    -1 if rb is NULL or the buffer is full.
 */
int rb_push(ringbuf_t *rb, uint8_t byte);

/*
 * rb_pop   — read and remove one byte (consumer side).
 *            Returns  0 on success, *out is set,
 *                    -1 if rb or out is NULL or the buffer is empty.
 */
int rb_pop(ringbuf_t *rb, uint8_t *out);

/*
 * rb_peek  — read one byte without consuming it (consumer side).
 *            Returns  0 on success, *out is set,
 *                    -1 if rb or out is NULL or the buffer is empty.
 */
int rb_peek(ringbuf_t *rb, uint8_t *out);

/*
 * rb_len   — number of bytes currently available to read.
 *            Returns count [0, RB_CAPACITY] or -1 if rb is NULL.
 */
int rb_len(ringbuf_t *rb);

/*
 * rb_space — number of bytes that can still be written.
 *            Returns count [0, RB_CAPACITY] or -1 if rb is NULL.
 */
int rb_space(ringbuf_t *rb);

#endif /* RINGBUF_H */
