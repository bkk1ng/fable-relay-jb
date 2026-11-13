/*
 * arena.h — bump allocator over a malloc'd block (C11)
 *
 * Usage:
 *   Arena *a = arena_new(1 << 20);       // 1 MiB block
 *   void  *p = arena_alloc(a, 64, 8);    // 64 bytes, 8-byte aligned
 *   ArenaMarker m = arena_mark(a);       // save position
 *   ...
 *   arena_reset(a, m);                   // restore position
 *   arena_free(a);                       // release everything
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

/* Opaque allocator handle. */
typedef struct Arena Arena;

/*
 * A saved position inside an arena, returned by arena_mark().
 * Treat as an opaque cookie; do not arithmetic on it yourself.
 */
typedef size_t ArenaMarker;

/*
 * arena_new — allocate a new arena backed by a malloc'd block of `size` bytes.
 * Returns NULL if malloc fails or size is 0.
 */
Arena *arena_new(size_t size);

/*
 * arena_alloc — bump-allocate `size` bytes aligned to `align` bytes.
 *
 * `align` must be a power of two in the range [1, 64].
 * Returns NULL on overflow, or if `size` is 0, or if `align` is out of range.
 * The returned pointer is valid until the arena is reset past this point
 * or arena_free() is called.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/*
 * arena_mark — return a marker for the current allocation position.
 * Pass the returned value to arena_reset() to free all allocations
 * made after this point.
 */
ArenaMarker arena_mark(const Arena *a);

/*
 * arena_reset — restore the allocation position to `mark`.
 * Any pointers allocated after the mark was taken become invalid.
 * No-op if mark is out of range.
 */
void arena_reset(Arena *a, ArenaMarker mark);

/*
 * arena_free — release the backing block and the Arena struct itself.
 * All pointers into the arena become invalid after this call.
 * Safe to call with NULL.
 */
void arena_free(Arena *a);

#endif /* ARENA_H */
