/**
 * arena.h — bump allocator over a malloc'd block (C11)
 *
 * Typical use:
 *   Arena *a = arena_new(1 << 20);
 *   void  *p = arena_alloc(a, 256, 16);
 *   ArenaMark m = arena_mark(a);
 *   ...
 *   arena_reset(a, m);   // roll back to m
 *   arena_free(a);       // release everything
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct Arena Arena;

/**
 * Opaque cursor snapshot.  Obtain with arena_mark(), pass to
 * arena_reset() to free everything allocated after that point.
 */
typedef size_t ArenaMark;

/**
 * arena_new – create an arena backed by a malloc'd block of `size` bytes.
 * Returns NULL if `size` is 0 or the system allocation fails.
 */
Arena *arena_new(size_t size);

/**
 * arena_alloc – bump-allocate `size` bytes aligned to `align` bytes.
 *
 * `align` must be a power of two in [1, 64].
 * Returns NULL on overflow, bad arguments, or NULL arena.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/**
 * arena_mark – snapshot the current allocation cursor.
 * Returns 0 if `a` is NULL (safe to pass to arena_reset).
 */
ArenaMark arena_mark(const Arena *a);

/**
 * arena_reset – roll the cursor back to a previously obtained mark,
 * effectively freeing every allocation made after it.
 * No-op if `a` is NULL or `mark` is out of range.
 */
void arena_reset(Arena *a, ArenaMark mark);

/**
 * arena_free – release the backing block and the Arena struct itself.
 * No-op on NULL.
 */
void arena_free(Arena *a);

#endif /* ARENA_H */
