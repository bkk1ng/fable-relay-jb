/*
 * ringbuf.c — fixed 4096-byte SPSC ring buffer (C11, no dynamic allocation)
 *
 * Index arithmetic wraps naturally because the indices grow unbounded as
 * uint32_t values and RB_CAPACITY is a power of two, so (idx & (RB_CAPACITY-1))
 * always gives the correct slot without a branch.
 *
 * Memory-order rationale:
 *   - Producer loads tail with relaxed (owns it), loads head with acquire
 *     (needs to see the consumer's advance before claiming space).
 *   - Consumer loads head with relaxed (owns it), loads tail with acquire
 *     (needs to see the producer's write before reading data).
 *   - Stores to the index the caller owns use release so the other side
 *     sees the data/space update before the index update.
 */

#include "ringbuf.h"

#include <string.h>   /* memset */

/* Mask to convert an index to a buffer slot. */
#define RB_MASK (RB_CAPACITY - 1U)

int rb_init(ringbuf_t *rb)
{
    if (!rb) return -1;
    memset(rb->buf, 0, sizeof(rb->buf));
    atomic_store_explicit(&rb->head, 0U, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0U, memory_order_relaxed);
    return 0;
}

int rb_push(ringbuf_t *rb, uint8_t byte)
{
    if (!rb) return -1;

    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    /* Full when tail is exactly one full capacity ahead of head. */
    if ((tail - head) >= RB_CAPACITY) return -1;

    rb->buf[tail & RB_MASK] = byte;

    /* Release so the consumer sees the byte before it sees the new tail. */
    atomic_store_explicit(&rb->tail, tail + 1U, memory_order_release);
    return 0;
}

int rb_pop(ringbuf_t *rb, uint8_t *out)
{
    if (!rb || !out) return -1;

    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head == tail) return -1;   /* empty */

    *out = rb->buf[head & RB_MASK];

    /* Release so the producer sees the freed slot before it sees the new head. */
    atomic_store_explicit(&rb->head, head + 1U, memory_order_release);
    return 0;
}

int rb_peek(ringbuf_t *rb, uint8_t *out)
{
    if (!rb || !out) return -1;

    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head == tail) return -1;   /* empty */

    *out = rb->buf[head & RB_MASK];
    return 0;
    /* head is NOT advanced — this is a non-consuming read */
}

int rb_len(ringbuf_t *rb)
{
    if (!rb) return -1;

    /*
     * Load tail first (acquire) then head (relaxed) so we never see a tail
     * that is behind the head we observe — that would yield a spuriously
     * large count after wrapping.
     */
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);

    return (int)(tail - head);   /* always in [0, RB_CAPACITY] */
}

int rb_space(ringbuf_t *rb)
{
    if (!rb) return -1;

    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    return (int)(RB_CAPACITY - (tail - head));
}
