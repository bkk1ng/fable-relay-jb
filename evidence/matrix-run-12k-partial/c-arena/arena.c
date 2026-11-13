/*
 * arena.c — bump allocator implementation (C11)
 */

#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Internal layout: the Arena header lives at the start of the malloc'd block,
 * followed immediately by the user-facing memory region.  Keeping them in one
 * allocation means a single free() tears everything down cleanly. */
struct Arena {
    size_t capacity; /* total bytes available after the header */
    size_t offset;   /* next free byte relative to the data region */
    /* data region follows immediately in memory */
};

/* Pointer to the first byte of the data region. */
static inline unsigned char *arena_data(Arena *a)
{
    return (unsigned char *)(a + 1);
}

/* Returns 1 if x is a power of two (including 1), 0 otherwise. */
static inline int is_pow2(size_t x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

/* Round `offset` up to the next multiple of `align`. */
static inline size_t align_up(size_t offset, size_t align)
{
    /* align is a power of two, so (align - 1) is a valid mask. */
    return (offset + align - 1) & ~(align - 1);
}

/* ---- public API ---- */

Arena *arena_new(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* Allocate header + data in one block. */
    Arena *a = malloc(sizeof(Arena) + size);
    if (!a) {
        return NULL;
    }

    a->capacity = size;
    a->offset   = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    if (!a) {
        return NULL;
    }

    /* Treat align == 0 as 1 (no alignment requirement). */
    if (align == 0) {
        align = 1;
    }

    /* Reject non-power-of-two or oversized alignment. */
    if (!is_pow2(align) || align > 64) {
        return NULL;
    }

    /* Compute the aligned start offset. */
    size_t aligned = align_up(a->offset, align);

    /* Check for size_t overflow before checking capacity. */
    if (size > 0 && aligned > SIZE_MAX - size) {
        return NULL;
    }

    size_t end = aligned + size;
    if (end > a->capacity) {
        return NULL; /* overflow */
    }

    a->offset = end;
    return (void *)(arena_data(a) + aligned);
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
    if (!a) {
        return;
    }
    /* Silently ignore marks that are out of range. */
    if (mark > a->capacity || mark > a->offset) {
        return;
    }
    a->offset = mark;
}

void arena_free(Arena *a)
{
    /* free(NULL) is defined behaviour in C11, but we document it too. */
    free(a);
}
