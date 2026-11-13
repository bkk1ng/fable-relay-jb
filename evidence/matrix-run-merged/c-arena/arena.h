/*
 * arena.h — bump allocator over a malloc'd block (C11)
 *
 * Usage:
 *   Arena *a = arena_new(1024 * 1024);
 *   void *p   = arena_alloc(a, 256, 16);
 *   ArenaPos mark = arena_mark(a);
 *   ...
 *   arena_reset(a, mark);
 *   arena_free(a);
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

/* Maximum supported alignment (must be a power of two, <= 64). */
#define ARENA_MAX_ALIGN 64

typedef struct Arena Arena;

/* Opaque position for mark/reset.  Store by value. */
typedef struct {
    size_t offset;
} ArenaPos;

/*
 * arena_new — allocate an arena backed by a malloc'd block of `size` bytes.
 * Returns NULL if malloc fails or size == 0.
 */
Arena *arena_new(size_t size);

/*
 * arena_alloc — bump-allocate `size` bytes aligned to `align`.
 * `align` must be a power of two in [1, ARENA_MAX_ALIGN].
 * Returns NULL on overflow or if `align` is invalid.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/*
 * arena_mark — capture the current allocation offset so it can be
 * restored later.
 */
ArenaPos arena_mark(const Arena *a);

/*
 * arena_reset — restore the arena to a previously captured position,
 * effectively freeing everything allocated after the mark.
 * Passing a position from a different arena is undefined behaviour.
 */
void arena_reset(Arena *a, ArenaPos pos);

/*
 * arena_free — release the backing block and the arena header.
 * The pointer is invalid after this call.
 */
void arena_free(Arena *a);

#endif /* ARENA_H */
