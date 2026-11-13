/*
 * arena.c — bump allocator implementation (C11)
 */

#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

/* Maximum supported alignment. */
#define ARENA_MAX_ALIGN 64u

struct Arena {
    unsigned char *base;   /* start of the backing block           */
    size_t         cap;    /* total capacity in bytes               */
    size_t         offset; /* current bump pointer, relative to base */
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Returns 1 if x is a power of two (and non-zero). */
static int is_pow2(size_t x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

/*
 * Align `ptr` up to the next multiple of `align`.
 * `align` must be a power of two.
 */
static size_t align_up(size_t offset, size_t align)
{
    return (offset + (align - 1u)) & ~(align - 1u);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

Arena *arena_new(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    Arena *a = malloc(sizeof(Arena));
    if (!a) {
        return NULL;
    }

    a->base = malloc(size);
    if (!a->base) {
        free(a);
        return NULL;
    }

    a->cap    = size;
    a->offset = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    if (!a || size == 0) {
        return NULL;
    }

    /* Validate alignment: must be a power of two, 1–64. */
    if (!is_pow2(align) || align > ARENA_MAX_ALIGN) {
        return NULL;
    }

    /* Align the current offset first. */
    size_t aligned_offset = align_up(a->offset, align);

    /* Check for overflow in the aligned offset itself. */
    if (aligned_offset < a->offset) {
        return NULL; /* size_t overflow */
    }

    /* Check that the allocation fits within the remaining capacity. */
    if (size > a->cap - aligned_offset) {
        return NULL; /* out of space */
    }

    void *ptr    = a->base + aligned_offset;
    a->offset    = aligned_offset + size;
    return ptr;
}

ArenaMarker arena_mark(const Arena *a)
{
    if (!a) {
        return 0;
    }
    return a->offset;
}

void arena_reset(Arena *a, ArenaMarker mark)
{
    if (!a) {
        return;
    }
    /* Silently ignore an out-of-range marker. */
    if (mark > a->cap) {
        return;
    }
    a->offset = mark;
}

void arena_free(Arena *a)
{
    if (!a) {
        return;
    }
    free(a->base);
    free(a);
}
