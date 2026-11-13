/*
 * ringbuf.c — lock-free SPSC ring buffer implementation
 *
 * Synchronization model (C11 atomics):
 *
 *   Producer:  load  tail (acquire)  →  write buf[]  →  store head (release)
 *   Consumer:  load  head (acquire)  →  read  buf[]  →  store tail (release)
 *
 * The release store on head guarantees the consumer sees the data bytes
 * before it sees the updated head. The acquire load on head pairs with that
 * release, establishing the happens-before edge. Symmetric for tail.
 *
 * Only one producer and one consumer may access the buffer concurrently.
 * rb_len / rb_space are safe to call from the consumer / producer side
 * respectively (they read the opposing cursor with acquire).
 */
#include "ringbuf.h"

#include <string.h>

/* Mask a cursor down to a valid buffer index (RB_CAPACITY is power of two). */
#define RB_MASK(c) ((c) & (RB_CAPACITY - 1))

int
rb_init(struct ringbuf *rb)
{
    if (rb == NULL)
        return -1;

    atomic_store_explicit(&rb->head, (size_t)0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, (size_t)0, memory_order_relaxed);
    return 0;
}

int
rb_space(struct ringbuf *rb)
{
    if (rb == NULL)
        return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    return (int)(RB_CAPACITY - (head - tail));
}

int
rb_len(struct ringbuf *rb)
{
    if (rb == NULL)
        return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    return (int)(head - tail);
}

int
rb_push(struct ringbuf *rb, const void *data, int len)
{
    if (rb == NULL || data == NULL || len < 0)
        return -1;
    if (len == 0)
        return 0;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    int space = (int)(RB_CAPACITY - (head - tail));
    if (space <= 0)
        return 0;

    int to_write = (len < space) ? len : space;
    size_t idx    = RB_MASK(head);
    int    chunk  = (int)((size_t)(RB_CAPACITY - idx) < (size_t)to_write
                          ? (RB_CAPACITY - idx)
                          : (size_t)to_write);

    memcpy(&rb->buf[idx], data, (size_t)chunk);
    if (to_write > chunk)
        memcpy(&rb->buf[0], (const char *)data + chunk,
               (size_t)(to_write - chunk));

    atomic_store_explicit(&rb->head, head + (size_t)to_write,
                          memory_order_release);
    return to_write;
}

int
rb_pop(struct ringbuf *rb, void *out, int len)
{
    if (rb == NULL || out == NULL || len < 0)
        return -1;
    if (len == 0)
        return 0;

    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    int avail = (int)(head - tail);
    if (avail <= 0)
        return 0;

    int to_read = (len < avail) ? len : avail;
    size_t idx   = RB_MASK(tail);
    int    chunk = (int)((size_t)(RB_CAPACITY - idx) < (size_t)to_read
                         ? (RB_CAPACITY - idx)
                         : (size_t)to_read);

    memcpy(out, &rb->buf[idx], (size_t)chunk);
    if (to_read > chunk)
        memcpy((char *)out + chunk, &rb->buf[0],
               (size_t)(to_read - chunk));

    atomic_store_explicit(&rb->tail, tail + (size_t)to_read,
                          memory_order_release);
    return to_read;
}

int
rb_peek(struct ringbuf *rb, void *out, int len)
{
    if (rb == NULL || out == NULL || len < 0)
        return -1;
    if (len == 0)
        return 0;

    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    int avail = (int)(head - tail);
    if (avail <= 0)
        return 0;

    int to_read = (len < avail) ? len : avail;
    size_t idx   = RB_MASK(tail);
    int    chunk = (int)((size_t)(RB_CAPACITY - idx) < (size_t)to_read
                         ? (RB_CAPACITY - idx)
                         : (size_t)to_read);

    memcpy(out, &rb->buf[idx], (size_t)chunk);
    if (to_read > chunk)
        memcpy((char *)out + chunk, &rb->buf[0],
               (size_t)(to_read - chunk));

    /* No tail update — peek does not consume. */
    return to_read;
}
