/*
 * arena.c — bump allocator over a malloc'd block (C11)
 *
 * Build: cc -std=c11 -Wall -Wextra -O2 -c arena.c
 */

#include "arena.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Layout:
 *   - One malloc for the Arena control struct.
 *   - One malloc for the raw capacity buffer.
 * Keeping them separate makes the buffer address stable for the
 * caller's pointers across arena_reset calls.
 */
struct Arena {
    unsigned char *buf;   /* backing memory                  */
    size_t        cap;     /* total bytes in buf              */
    size_t        off;     /* next free byte (bump cursor)    */
};

/* ---- helpers --------------------------------------------------------- */

/*
 * is_pow2(x): true when x is a positive power of two.
 * A power of two has exactly one bit set, so (x & (x - 1)) clears it
 * and yields zero.  We also reject x == 0 up front.
 */
static inline bool is_pow2(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/*
 * align_up(n, align): round n up to the next multiple of align.
 * align must be a power of two.  The mask trick avoids the division
 * that the modulo form would require.
 *
 *   align_up(n, a) = (n + a - 1) & ~(a - 1)
 *
 * The addition can overflow, so callers must guard against that.
 */
static inline size_t align_up(size_t n, size_t align)
{
    size_t mask = align - 1;
    return (n + mask) & ~mask;
}

/* ---- public API ------------------------------------------------------- */

Arena *arena_new(size_t size)
{
    if (size == 0)
        return NULL;

    Arena *a = malloc(sizeof *a);
    if (!a)
        return NULL;

    a->buf = malloc(size);
    if (!a->buf) {
        free(a);
        return NULL;
    }

    a->cap = size;
    a->off = 0;
    return a;
}

void *arena_alloc(Arena *arena, size_t size, size_t align)
{
    if (!arena || !is_pow2(align) || align > 64)
        return NULL;

    /* Zero-size allocations are allowed; return a valid (aligned) pointer. */
    if (size == 0)
        size = 1;

    /* Bump the cursor up to the requested alignment. */
    size_t base = align_up(arena->off, align);
    if (base < arena->off)          /* overflow in align_up */
        return NULL;

    /* Check that base + size fits. Do it in a way that can't overflow. */
    if (base > arena->cap || size > arena->cap - base)
        return NULL;

    void *ptr = arena->buf + base;
    arena->off = base + size;
    return ptr;
}

ArenaPos arena_mark(const Arena *arena)
{
    ArenaPos pos;
    pos.offset = arena ? arena->off : 0;
    return pos;
}

void arena_reset(Arena *arena, ArenaPos pos)
{
    if (!arena)
        return;

    /* Clamp to valid range so a stale/wild mark can't corrupt state. */
    if (pos.offset > arena->cap)
        pos.offset = arena->cap;

    arena->off = pos.offset;
}

void arena_free(Arena *arena)
{
    if (!arena)
        return;

    free(arena->buf);
    free(arena);
}
