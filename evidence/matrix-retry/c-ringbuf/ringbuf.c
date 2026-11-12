#include "ringbuf.h"

#include <string.h>

int rb_init(ringbuf_t *rb)
{
    if (!rb)
        return -1;

    atomic_init(&rb->head, (size_t)0);
    atomic_init(&rb->tail, (size_t)0);
    return 0;
}

int rb_space(ringbuf_t *rb)
{
    if (!rb)
        return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (int)(RB_CAPACITY - (head - tail));
}

int rb_len(ringbuf_t *rb)
{
    if (!rb)
        return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    return (int)(head - tail);
}

int rb_push(ringbuf_t *rb, const void *src, size_t len)
{
    if (!rb || !src || len == 0)
        return -1;
    if (len > RB_CAPACITY)
        return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    /* Available space = capacity - (head - tail). */
    if (RB_CAPACITY - (head - tail) < len)
        return -1;

    /* Copy in one or two segments to handle wraparound. */
    size_t idx   = head % RB_CAPACITY;
    size_t first = RB_CAPACITY - idx;
    if (first > len)
        first = len;

    memcpy(rb->data + idx, src, first);
    if (len > first)
        memcpy(rb->data, (const unsigned char *)src + first, len - first);

    atomic_store_explicit(&rb->head, head + len, memory_order_release);
    return 0;
}

int rb_pop(ringbuf_t *rb, void *dst, size_t len)
{
    if (!rb || !dst || len == 0)
        return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (head - tail < len)
        return -1;

    size_t idx   = tail % RB_CAPACITY;
    size_t first = RB_CAPACITY - idx;
    if (first > len)
        first = len;

    memcpy(dst, rb->data + idx, first);
    if (len > first)
        memcpy((unsigned char *)dst + first, rb->data, len - first);

    atomic_store_explicit(&rb->tail, tail + len, memory_order_release);
    return 0;
}

int rb_peek(ringbuf_t *rb, void *dst, size_t len)
{
    if (!rb || !dst || len == 0)
        return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (head - tail < len)
        return -1;

    size_t idx   = tail % RB_CAPACITY;
    size_t first = RB_CAPACITY - idx;
    if (first > len)
        first = len;

    memcpy(dst, rb->data + idx, first);
    if (len > first)
        memcpy((unsigned char *)dst + first, rb->data, len - first);

    return 0;
}
