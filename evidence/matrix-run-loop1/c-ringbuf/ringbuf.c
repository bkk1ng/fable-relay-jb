/*
 * ringbuf.c — fixed 4096-byte SPSC ring buffer (C11, no dynamic allocation)
 *
 * Uses C11 _Atomic size_t for head and tail so the producer and consumer
 * can run on separate threads without a mutex.  The buffer capacity is
 * RINGBUF_SIZE - 1 bytes; one slot is kept empty to distinguish full from
 * empty without a separate flag.
 */

#include "ringbuf.h"

#include <stdatomic.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static inline size_t next(size_t idx)
{
    /* Mask works because RINGBUF_SIZE is a power of two. */
    return (idx + 1u) & (RINGBUF_SIZE - 1u);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int rb_init(ringbuf_t *rb)
{
    if (!rb) return -1;

    memset(rb->buf, 0, sizeof rb->buf);
    atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);
    return 0;
}

/*
 * rb_push — called only from the producer thread.
 *
 * Load tail with relaxed (we own it), load head with acquire so we see
 * the latest consumer position before deciding whether space exists.
 */
int rb_push(ringbuf_t *rb, uint8_t byte)
{
    if (!rb) return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (next(tail) == head) {
        /* Buffer full — one slot always stays empty. */
        return -1;
    }

    rb->buf[tail] = byte;

    /* Release so the consumer sees the written byte when it reads head. */
    atomic_store_explicit(&rb->tail, next(tail), memory_order_release);
    return 0;
}

/*
 * rb_pop — called only from the consumer thread.
 *
 * Load head with relaxed (we own it), load tail with acquire so we see
 * the latest producer write before reading the byte.
 */
int rb_pop(ringbuf_t *rb, uint8_t *out)
{
    if (!rb || !out) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head == tail) {
        /* Buffer empty. */
        return -1;
    }

    *out = rb->buf[head];

    /* Release so the producer sees the freed slot when it reads tail. */
    atomic_store_explicit(&rb->head, next(head), memory_order_release);
    return 0;
}

/*
 * rb_peek — same as rb_pop but does not advance head.
 */
int rb_peek(ringbuf_t *rb, uint8_t *out)
{
    if (!rb || !out) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head == tail) {
        return -1;
    }

    *out = rb->buf[head];
    return 0;
}

/*
 * rb_len — bytes available to read.
 *
 * Both indices are loaded with acquire/acquire for a consistent snapshot
 * when called from outside the strict producer/consumer roles (e.g. a
 * monitor thread).  The result is a lower-bound estimate in a live system.
 */
int rb_len(ringbuf_t *rb)
{
    if (!rb) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    /* Subtraction wraps correctly because both indices are mod RINGBUF_SIZE. */
    return (int)((tail - head) & (RINGBUF_SIZE - 1u));
}

/*
 * rb_space — bytes free for writing.
 *
 * RINGBUF_SIZE - 1 is the true capacity (one slot reserved as sentinel).
 */
int rb_space(ringbuf_t *rb)
{
    if (!rb) return -1;

    int len = rb_len(rb);
    if (len < 0) return -1;

    return (int)(RINGBUF_SIZE - 1u) - len;
}
