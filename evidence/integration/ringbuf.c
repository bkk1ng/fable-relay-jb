#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define RINGBUF_SIZE 1024

typedef struct {
    uint8_t  data[RINGBUF_SIZE];
    size_t   head;
    size_t   tail;
    size_t   len;
} ringbuf_t;

void ringbuf_init(ringbuf_t *rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->len  = 0;
}

size_t ringbuf_len(const ringbuf_t *rb) {
    return rb->len;
}

bool ringbuf_push(ringbuf_t *rb, uint8_t byte) {
    if (rb->len == RINGBUF_SIZE) {
        return false;
    }
    rb->data[rb->tail] = byte;
    rb->tail = (rb->tail + 1) % RINGBUF_SIZE;
    rb->len++;
    return true;
}

bool ringbuf_pop(ringbuf_t *rb, uint8_t *out) {
    if (rb->len == 0) {
        return false;
    }
    *out = rb->data[rb->head];
    rb->head = (rb->head + 1) % RINGBUF_SIZE;
    rb->len--;
    return true;
}
