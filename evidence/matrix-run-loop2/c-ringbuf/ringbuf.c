/**
 * ringbuf.c - Fixed 4096-byte single-producer/single-consumer ring buffer
 *
 * Uses a "leave one slot empty" scheme so full and empty are unambiguous
 * without an extra flag. Usable capacity is therefore RB_CAPACITY - 1
 * (4095 bytes).
 *
 * head  - next write position (producer owns this)
 * tail  - next read  position (consumer owns this)
 * empty - head == tail
 * full  - (head + 1) % RB_CAPACITY == tail
 */

#include "ringbuf.h"

#include <string.h>  /* memset */

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static size_t next_idx(size_t idx)
{
    return (idx + 1u) % RB_CAPACITY;
}

/* Snapshot-safe length: read tail first (consumer side), then head. */
static size_t rb_len_internal(const ringbuf_t *rb)
{
    size_t tail = rb->tail;
    size_t head = rb->head;
    if (head >= tail)
        return head - tail;
    return RB_CAPACITY - tail + head;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * rb_init - Initialise a ring buffer.
 * Returns 0 on success, -1 if rb is NULL.
 */
int rb_init(ringbuf_t *rb)
{
    if (!rb)
        return -1;

    memset(rb->data, 0, sizeof(rb->data));
    rb->head = 0;
    rb->tail = 0;
    return 0;
}

/**
 * rb_push - Write one byte.
 * Returns 0 on success, -1 if the buffer is full.
 */
int rb_push(ringbuf_t *rb, unsigned char byte)
{
    if (!rb)
        return -1;

    size_t head = rb->head;
    size_t nxt  = next_idx(head);

    /* Full: the slot ahead of head is tail. */
    if (nxt == rb->tail)
        return -1;

    rb->data[head] = byte;

    /* Publish: advance head AFTER writing the data. */
    rb->head = nxt;
    return 0;
}

/**
 * rb_pop - Read and remove one byte.
 * Returns 0 on success, -2 if the buffer is empty.
 */
int rb_pop(ringbuf_t *rb, unsigned char *out)
{
    if (!rb || !out)
        return -2;

    size_t tail = rb->tail;

    /* Empty: tail has caught up to head. */
    if (tail == rb->head)
        return -2;

    *out = rb->data[tail];

    /* Publish: advance tail AFTER reading the data. */
    rb->tail = next_idx(tail);
    return 0;
}

/**
 * rb_peek - Read one byte without consuming it.
 * Returns 0 on success, -2 if the buffer is empty.
 */
int rb_peek(const ringbuf_t *rb, unsigned char *out)
{
    if (!rb || !out)
        return -2;

    size_t tail = rb->tail;

    if (tail == rb->head)
        return -2;

    *out = rb->data[tail];
    return 0;
}

/**
 * rb_len - Bytes currently stored.
 * Returns count on success, -1 if rb is NULL.
 */
int rb_len(const ringbuf_t *rb)
{
    if (!rb)
        return -1;

    return (int)rb_len_internal(rb);
}

/**
 * rb_space - Bytes that can still be written.
 * Returns available space on success, -1 if rb is NULL.
 */
int rb_space(const ringbuf_t *rb)
{
    if (!rb)
        return -1;

    /* One slot is permanently reserved to distinguish full from empty. */
    return (int)(RB_CAPACITY - 1u - rb_len_internal(rb));
}
