/*
 * arena.h — bump allocator over a malloc'd block (C11)
 *
 * Usage:
 *   Arena *a = arena_new(1024 * 1024);
 *   void *p   = arena_alloc(a, 256, 8);
 *   ArenaPos mark = arena_mark(a);
 *   // ... more allocations ...
 *   arena_reset(a, mark);   // rewind to mark
 *   arena_free(a);          // release everything
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque allocator handle. */
typedef struct Arena Arena;

/*
 * A saved position for partial rewind.
 * Treat as an opaque value — do not inspect the field directly.
 */
typedef struct {
    size_t offset; /* byte offset into the arena buffer */
} ArenaPos;

/*
 * arena_new(size)
 *   Allocate a new arena backed by a malloc'd block of at least `size` bytes.
 *   Returns NULL if the system allocation fails or size == 0.
 */
Arena *arena_new(size_t size);

/*
 * arena_alloc(arena, size, align)
 *   Bump-allocate `size` bytes aligned to `align` bytes.
 *   `align` must be a power of two in the range [1, 64].
 *   Returns NULL on overflow or if align is invalid.
 *   The returned pointer is valid until the next arena_reset or arena_free.
 */
void *arena_alloc(Arena *arena, size_t size, size_t align);

/*
 * arena_mark(arena)
 *   Capture the current allocation cursor so it can be restored later.
 */
ArenaPos arena_mark(const Arena *arena);

/*
 * arena_reset(arena, pos)
 *   Rewind the allocation cursor to `pos`, effectively freeing everything
 *   allocated after the corresponding arena_mark call.
 *   Behaviour is undefined if `pos` did not come from the same arena.
 */
void arena_reset(Arena *arena, ArenaPos pos);

/*
 * arena_free(arena)
 *   Release the backing buffer and the Arena struct itself.
 *   Passing NULL is a no-op.
 */
void arena_free(Arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* ARENA_H */
