#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Arena {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    int      owns;
} Arena;

static inline Arena arena_from(void *buf, size_t cap) {
    return (Arena){ .buf = (uint8_t *)buf, .cap = cap, .pos = 0, .owns = 0 };
}

static inline Arena arena_alloc(size_t cap) {
    void *buf = malloc(cap);
    return (Arena){ .buf = (uint8_t *)buf, .cap = cap, .pos = 0, .owns = 1 };
}

static inline void arena_free(Arena *a) {
    if (a->owns) free(a->buf);
    *a = (Arena){0};
}

static inline void *arena_push(Arena *a, size_t size, size_t align) {
    size_t pad = (-(uintptr_t)(a->buf + a->pos)) & (align - 1);
    if (a->pos + pad + size > a->cap) return NULL;
    void *ptr = a->buf + a->pos + pad;
    a->pos += pad + size;
    return ptr;
}

static inline void *arena_push_zero(Arena *a, size_t size, size_t align) {
    void *ptr = arena_push(a, size, align);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

static inline size_t arena_save(const Arena *a)         { return a->pos; }
static inline void   arena_restore(Arena *a, size_t pos) { a->pos = pos;  }
static inline void   arena_reset(Arena *a)               { a->pos = 0;    }

#define arena_new(a, T)       ((T *)arena_push_zero((a), sizeof(T),    _Alignof(T)))
#define arena_new_n(a, T, n)  ((T *)arena_push_zero((a), sizeof(T)*(n),_Alignof(T)))
