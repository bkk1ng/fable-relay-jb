/**
 * arena.h — bump allocator over a malloc'd block
 *
 * Usage:
 *   Arena *a = arena_new(1024 * 1024);
 *   void *p   = arena_alloc(a, 256, 8);
 *   ArenaPos mark = arena_mark(a);
 *   // ... temporary allocations ...
 *   arena_reset(a, mark);
 *   arena_free(a);
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/* Maximum supported alignment for arena_alloc(). */
#define ARENA_MAX_ALIGN 64

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque arena handle.
 * Allocate with arena_new(); free with arena_free().
 */
typedef struct Arena Arena;

/**
 * Save/restore position within the arena.
 * Treat this as an opaque value — do not manipulate it directly.
 */
typedef struct {
    size_t offset;
} ArenaPos;

/**
 * arena_new – create a new arena backed by a malloc'd block of `size` bytes.
 *
 * Returns NULL if `size` is 0 or if the system allocation fails.
 */
Arena *arena_new(size_t size);

/**
 * arena_alloc – allocate `size` bytes with `align`-byte alignment.
 *
 * `align` must be a power of two and at most ARENA_MAX_ALIGN.
 * Returns NULL on overflow or if `size` is 0 or if `align` is invalid.
 * The returned pointer is valid until the arena is freed or reset past it.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/**
 * arena_mark – capture the current allocation offset for later rollback.
 */
ArenaPos arena_mark(const Arena *a);

/**
 * arena_reset – roll back all allocations made after `pos` was captured.
 *
 * Passing a position from a different arena is undefined behaviour.
 */
void arena_reset(Arena *a, ArenaPos pos);

/**
 * arena_free – release the backing block and invalidate the arena handle.
 *
 * All pointers previously returned by arena_alloc() become invalid.
 */
void arena_free(Arena *a);

#ifdef __cplusplus
}
#endif

#endif /* ARENA_H */
