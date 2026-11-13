/**
 * ringbuf.h — Fixed 4096-byte single-producer/single-consumer ring buffer.
 *
 * Thread-safety: safe for one writer thread and one reader thread with no
 * additional locking, provided the compiler and CPU respect the sequencing
 * implied by the volatile accesses.  Not safe for multiple producers or
 * multiple consumers.
 *
 * All functions return 0 on success, -1 on error (bad pointer, buffer full,
 * or buffer empty as appropriate).
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>  /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

#define RINGBUF_CAPACITY 4096u

/**
 * Ring buffer control structure.
 *
 * head  — index of the next byte to write (producer-owned).
 * tail  — index of the next byte to read  (consumer-owned).
 *
 * The buffer holds at most RINGBUF_CAPACITY - 1 bytes so that a full state
 * (head + 1 == tail) is distinct from an empty state (head == tail).
 */
typedef struct {
    unsigned char buf[RINGBUF_CAPACITY];
    volatile size_t head; /* written by producer only */
    volatile size_t tail; /* written by consumer only */
} ringbuf_t;

/**
 * rb_init — Initialise a ring buffer.
 *
 * @rb   Pointer to an uninitialised ringbuf_t.
 * @return 0 on success, -1 if rb is NULL.
 */
int rb_init(ringbuf_t *rb);

/**
 * rb_push — Write one byte into the buffer.
 *
 * @rb   Ring buffer.
 * @byte Byte to push.
 * @return 0 on success, -1 if rb is NULL or the buffer is full.
 */
int rb_push(ringbuf_t *rb, unsigned char byte);

/**
 * rb_pop — Remove and return one byte from the buffer.
 *
 * @rb   Ring buffer.
 * @out  Destination for the byte read.  Must not be NULL.
 * @return 0 on success, -1 if rb or out is NULL or the buffer is empty.
 */
int rb_pop(ringbuf_t *rb, unsigned char *out);

/**
 * rb_peek — Read one byte without removing it.
 *
 * @rb   Ring buffer.
 * @out  Destination for the byte.  Must not be NULL.
 * @return 0 on success, -1 if rb or out is NULL or the buffer is empty.
 */
int rb_peek(ringbuf_t *rb, unsigned char *out);

/**
 * rb_len — Number of bytes currently stored.
 *
 * @rb   Ring buffer.
 * @return Byte count on success, -1 if rb is NULL.
 */
int rb_len(ringbuf_t *rb);

/**
 * rb_space — Number of additional bytes that can be pushed before the buffer
 *            is full.
 *
 * @rb   Ring buffer.
 * @return Available bytes on success, -1 if rb is NULL.
 */
int rb_space(ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
