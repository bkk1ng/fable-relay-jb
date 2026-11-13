/**
 * ringbuf.h - Fixed 4096-byte single-producer/single-consumer ring buffer
 *
 * No dynamic allocation. All storage is embedded in the struct.
 * Safe for one producer thread and one consumer thread without locking,
 * provided the compiler respects the volatile qualifiers on head/tail.
 *
 * Status codes returned by all functions:
 *   0  success
 *  -1  buffer full  (rb_push)
 *  -2  buffer empty (rb_pop, rb_peek)
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_CAPACITY 4096

typedef struct {
    unsigned char  data[RB_CAPACITY];
    volatile size_t head;   /* producer writes here */
    volatile size_t tail;   /* consumer reads here  */
} ringbuf_t;

/**
 * rb_init - Initialise a ring buffer.
 * @rb: pointer to an uninitialized ringbuf_t
 * Returns 0 on success, -1 if rb is NULL.
 */
int rb_init(ringbuf_t *rb);

/**
 * rb_push - Write one byte into the buffer.
 * @rb:   ring buffer
 * @byte: value to write
 * Returns 0 on success, -1 if the buffer is full.
 */
int rb_push(ringbuf_t *rb, unsigned char byte);

/**
 * rb_pop - Read and remove one byte from the buffer.
 * @rb:   ring buffer
 * @out:  destination for the byte
 * Returns 0 on success, -2 if the buffer is empty.
 */
int rb_pop(ringbuf_t *rb, unsigned char *out);

/**
 * rb_peek - Read one byte without removing it.
 * @rb:   ring buffer
 * @out:  destination for the byte
 * Returns 0 on success, -2 if the buffer is empty.
 */
int rb_peek(const ringbuf_t *rb, unsigned char *out);

/**
 * rb_len - Number of bytes currently stored.
 * @rb: ring buffer
 * Returns the byte count (0 .. RB_CAPACITY-1), or -1 if rb is NULL.
 */
int rb_len(const ringbuf_t *rb);

/**
 * rb_space - Number of bytes that can still be written.
 * @rb: ring buffer
 * Returns the available space (0 .. RB_CAPACITY-1), or -1 if rb is NULL.
 */
int rb_space(const ringbuf_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
