/*
 * ringbuf.h — lock-free SPSC ring buffer (single producer, single consumer)
 *
 * C11, no dynamic allocation. 4096-byte fixed capacity.
 * Uses C11 atomics for lock-free synchronization between exactly one
 * producer thread and one consumer thread.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>

#define RB_CAPACITY 4096  /* must be a power of two */

/*
 * The buffer is fully defined so the caller can place it on the stack,
 * in static storage, or inside another struct — no malloc needed.
 *
 * head  — write cursor, owned and advanced by the producer
 * tail  — read  cursor, owned and advanced by the consumer
 *
 * Both grow monotonically and wrap via unsigned size_t arithmetic.
 * Occupied bytes   = head - tail
 * Free bytes       = RB_CAPACITY - (head - tail)
 * Buffer index      = cursor & (RB_CAPACITY - 1)
 */
struct ringbuf {
    unsigned char    buf[RB_CAPACITY];
    atomic_size_t    head;
    atomic_size_t    tail;
};

/* Initialize head and tail to zero. Returns 0 on success, -1 on NULL. */
int rb_init(struct ringbuf *rb);

/*
 * Push up to len bytes from data into the buffer.
 * Writes as many bytes as free space allows (partial push is OK).
 * Returns the number of bytes written (0 if full), or -1 on error.
 */
int rb_push(struct ringbuf *rb, const void *data, int len);

/*
 * Pop up to len bytes from the buffer into out, advancing tail.
 * Reads as many bytes as available (partial pop is OK).
 * Returns the number of bytes read (0 if empty), or -1 on error.
 */
int rb_pop(struct ringbuf *rb, void *out, int len);

/*
 * Copy up to len bytes from the buffer into out without advancing tail.
 * Returns the number of bytes copied (0 if empty), or -1 on error.
 */
int rb_peek(struct ringbuf *rb, void *out, int len);

/* Return the number of bytes currently available to read, or -1 on error. */
int rb_len(struct ringbuf *rb);

/* Return the number of bytes currently free for writing, or -1 on error. */
int rb_space(struct ringbuf *rb);

#endif /* RINGBUF_H */
