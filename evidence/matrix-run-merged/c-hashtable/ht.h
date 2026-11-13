/*
 * ht.h — open-addressing string hash table
 *
 * Hash:    FNV-1a 64-bit
 * Probing: linear
 * Delete:  tombstone markers
 * Resize:  double capacity when live load > 0.7;
 *          rehash in-place (clears tombstones) when total load > 0.7
 *          but live entries alone would fit at current capacity.
 *
 * Keys are copied on insert; values are unmanaged void pointers.
 */

#ifndef HT_H
#define HT_H

#include <stdbool.h>
#include <stddef.h>

/* Opaque table handle. */
typedef struct ht ht;

/*
 * Iterator — stack-allocated, valid until the next ht_set / ht_del / ht_free.
 *
 * Usage:
 *     ht_iter it = ht_iter_new(t);
 *     const char *k;
 *     void       *v;
 *     while (ht_iter_next(&it, &k, &v)) { ... }
 */
typedef struct {
    ht    *table;
    size_t index;
} ht_iter;

/*
 * ht_new — allocate and return a new, empty table.
 * Returns NULL on allocation failure.
 */
ht    *ht_new(void);

/*
 * ht_set — insert or update key with value.
 * The key string is copied internally.
 * Returns true on success, false on allocation failure.
 */
bool   ht_set(ht *t, const char *key, void *value);

/*
 * ht_get — look up key.
 * Returns the stored value pointer, or NULL if not found.
 * Note: NULL is also a valid stored value; use ht_has() if disambiguation
 * matters, or simply avoid storing NULL values.
 */
void  *ht_get(ht *t, const char *key);

/*
 * ht_del — remove key from the table.
 * Returns true if the key was present and removed, false if not found.
 */
bool   ht_del(ht *t, const char *key);

/*
 * ht_free — release all memory owned by the table (including copied keys).
 * Safe to call with NULL.
 */
void   ht_free(ht *t);

/* Number of live (non-deleted) entries. */
size_t ht_count(const ht *t);

/* Total allocated slot capacity. */
size_t ht_capacity(const ht *t);

/* Number of tombstone (deleted but not yet reclaimed) slots. */
size_t ht_tombstones(const ht *t);

/*
 * ht_iter_new — initialise an iterator over t.
 * Returns the iterator by value; no allocation.
 */
ht_iter ht_iter_new(ht *t);

/*
 * ht_iter_next — advance to the next live entry.
 * Writes the key and value through the out-pointers and returns true.
 * Returns false when the table is exhausted.
 */
bool ht_iter_next(ht_iter *it, const char **key, void **value);

#endif /* HT_H */
