/**
 * arena.c — bump allocator implementation (C11)
 */

#include "arena.h"

#include <stdint.h>   /* uintptr_t, SIZE_MAX          */
#include <stdlib.h>   /* malloc, free                 */

/* Internal layout: the Arena header lives just before the data block so
 * that we only make one malloc call and one free call per arena. */
struct Arena {
    unsigned char *base;   /* first byte of the data block        */
    size_t         cap;    /* total usable bytes in the block      */
    size_t         offset; /* next free byte (cursor)              */
};

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Round `n` up to the nearest multiple of `align` (power-of-two align). */
static inline size_t align_up(size_t n, size_t align)
{
    return (n + (align - 1u)) & ~(align - 1u);
}

/* Return 1 if `v` is a non-zero power of two, 0 otherwise. */
static inline int is_pow2(size_t v)
{
    return (v != 0) && ((v & (v - 1u)) == 0);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

Arena *arena_new(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* Allocate the header and the data block in one shot.
     * We align the header size to 16 bytes so the first user allocation
     * starts on a natural boundary regardless of the struct size. */
    size_t header_size = align_up(sizeof(Arena), 16u);

    /* Guard against overflow when combining header and user sizes. */
    if (size > SIZE_MAX - header_size) {
        return NULL;
    }

    unsigned char *raw = (unsigned char *)malloc(header_size + size);
    if (!raw) {
        return NULL;
    }

    Arena *a   = (Arena *)(void *)raw;
    a->base    = raw + header_size;
    a->cap     = size;
    a->offset  = 0;

    return a;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    if (!a || size == 0) {
        return NULL;
    }

    /* Alignment must be a power of two in [1, 64]. */
    if (!is_pow2(align) || align > 64u) {
        return NULL;
    }

    /* Align the current cursor. */
    uintptr_t cur     = (uintptr_t)(a->base + a->offset);
    uintptr_t aligned = (cur + (align - 1u)) & ~(uintptr_t)(align - 1u);
    size_t    padding = (size_t)(aligned - cur);

    /* Check for overflow: padding + size must not exceed remaining space. */
    size_t remaining = a->cap - a->offset;
    if (padding > remaining || size > remaining - padding) {
        return NULL; /* overflow */
    }

    a->offset += padding + size;
    return (void *)aligned;
}

ArenaMark arena_mark(const Arena *a)
{
    if (!a) {
        return 0;
    }
    return a->offset;
}

void arena_reset(Arena *a, ArenaMark mark)
{
    if (!a || mark > a->cap) {
        return;
    }
    a->offset = mark;
}

void arena_free(Arena *a)
{
    if (!a) {
        return;
    }
    /* The header and data block are one allocation; free the header ptr. */
    free(a);
}
