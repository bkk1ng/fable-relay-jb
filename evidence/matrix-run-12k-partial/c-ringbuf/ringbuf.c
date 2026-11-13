/**
 * ringbuf.c — Fixed 4096-byte single-producer/single-consumer ring buffer.
 *
 * C11, no dynamic allocation.
 *
 * Build:   cc -std=c11 -c ringbuf.c
 * Link:    cc -std=c11 ringbuf.c <main.c> -o ringbuf_demo
 */

#include "ringbuf.h"

/*
 * The buffer is considered full when exactly one slot is unused, so that a
 * full state (head + 1 == tail, modulo capacity) is distinct from an empty
 * state (head == tail).  Maximum usable capacity is RINGBUF_CAPACITY - 1.
 */
#define RINGBUF_MAX_LEN ((int)(RINGBUF_CAPACITY - 1u))

int rb_init(ringbuf_t *rb)
{
    if (rb == NULL) {
        return -1;
    }

    rb->head = 0;
    rb->tail = 0;
    return 0;
}

int rb_push(ringbuf_t *rb, unsigned char byte)
{
    if (rb == NULL) {
        return -1;
    }

    size_t next = (rb->head + 1u) % RINGBUF_CAPACITY;
    if (next == rb->tail) {
        /* Buffer is full — one slot reserved to distinguish full from empty. */
        return -1;
    }

    rb->buf[rb->head] = byte;
    rb->head = next;  /* publish: visible to the consumer after this store */
    return 0;
}

int rb_pop(ringbuf_t *rb, unsigned char *out)
{
    if (rb == NULL || out == NULL) {
        return -1;
    }

    if (rb->head == rb->tail) {
        /* Empty. */
        return -1;
    }

    *out = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1u) % RINGBUF_CAPACITY;  /* publish to producer */
    return 0;
}

int rb_peek(ringbuf_t *rb, unsigned char *out)
{
    if (rb == NULL || out == NULL) {
        return -1;
    }

    if (rb->head == rb->tail) {
        /* Empty. */
        return -1;
    }

    *out = rb->buf[rb->tail];
    return 0;
}

int rb_len(ringbuf_t *rb)
{
    if (rb == NULL) {
        return -1;
    }

    /*
     * size_t subtraction wraps cleanly: when head < tail the modular
     * arithmetic still produces the correct byte count.  The result is
     * guaranteed to fit in [0, RINGBUF_MAX_LEN].
     */
    size_t len = (rb->head + RINGBUF_CAPACITY - rb->tail) % RINGBUF_CAPACITY;
    return (int)len;
}

int rb_space(ringbuf_t *rb)
{
    if (rb == NULL) {
        return -1;
    }

    size_t len = (rb->head + RINGBUF_CAPACITY - rb->tail) % RINGBUF_CAPACITY;
    return RINGBUF_MAX_LEN - (int)len;
}
