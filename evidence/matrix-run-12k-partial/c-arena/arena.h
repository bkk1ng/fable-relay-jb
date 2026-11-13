/*
 * arena.h — bump allocator over a malloc'd block (C11)
 *
 * Typical use:
 *   Arena *a = arena_new(1 << 20);
 *   ArenaMark m = arena_mark(a);
 *   Foo *f = arena_alloc(a, sizeof *f, _Alignof(Foo));
 *   ...
 *   arena_reset(a, m);  // reclaim everything after m
 *   arena_free(a);
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/* Opaque arena handle. */
typedef struct Arena Arena;

/*
 * Saved allocation position.  Treat as an opaque value: obtain one with
 * arena_mark(), pass it back to arena_reset(), do not modify it directly.
 */
typedef size_t ArenaMark;

/*
 * arena_new — create an arena backed by a malloc'd block of `size` bytes.
 * Returns NULL if malloc fails or size is 0.
 */
Arena *arena_new(size_t size);

/*
 * arena_alloc — bump-allocate `size` bytes aligned to `align` bytes.
 *
 * `align` must be a power of two in [1, 64].  Passing 0 is treated as 1.
 * Returns NULL on overflow, or if `align` is not a power of two, or if
 * `align` exceeds 64.  A size of 0 returns a valid aligned pointer but
 * advances the cursor by 0.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/*
 * arena_mark — snapshot the current allocation position.
 * Cheap: just returns a size_t.
 */
ArenaMark arena_mark(const Arena *a);

/*
 * arena_reset — restore the allocation position to `mark`, logically freeing
 * every allocation made after that mark.  The underlying memory is not
 * zeroed; callers must not read freed allocations.
 *
 * Passing a mark that is beyond the current position or beyond the arena
 * capacity is a no-op.
 */
void arena_reset(Arena *a, ArenaMark mark);

/*
 * arena_free — release all memory owned by the arena, including the arena
 * object itself.  Passing NULL is safe.
 */
void arena_free(Arena *a);

#endif /* ARENA_H */
