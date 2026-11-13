/*
 * ht.h — open-addressing string hash table (C11)
 *
 * Hashing : FNV-1a 32-bit
 * Collision: linear probing
 * Deletion : tombstones
 * Resize   : double capacity when (live + dead) / capacity >= 0.7
 */

#ifndef HT_H
#define HT_H

#include <stddef.h>
#include <stdbool.h>

/*
 * A single slot in the backing array.
 *   key == NULL              — empty slot
 *   key == internal sentinel — deleted (tombstone)
 *   otherwise                — live entry
 *
 * The table owns the key string (strdup'd on insert).
 * It does NOT own the value pointer.
 */
typedef struct {
    char *key;
    void *value;
} ht_entry;

/*
 * The hash table itself.
 * capacity is always a power of two.
 */
typedef struct {
    ht_entry *entries;
    size_t    capacity; /* total allocated slots        */
    size_t    count;    /* live key-value pairs         */
    size_t    dead;     /* tombstone slots (deletions)  */
} ht;

/*
 * Forward-only iterator.  Invalidated by any mutation of the table
 * (ht_set, ht_del) after the iterator is created.
 *
 * Usage:
 *   ht_iter it = ht_iter_new(t);
 *   const char *k; void *v;
 *   while (ht_iter_next(&it, &k, &v)) { ... }
 */
typedef struct {
    ht    *table;
    size_t index;
} ht_iter;

/* Allocate a new, empty hash table.  Returns NULL on allocation failure. */
ht      *ht_new(void);

/*
 * Insert or update the entry for key.
 * The table copies the key string; the caller retains ownership of value.
 * Returns true on success, false on allocation failure.
 */
bool     ht_set(ht *t, const char *key, void *value);

/*
 * Look up key.
 * Returns the associated value pointer, or NULL if not found.
 * (NULL is also a valid stored value, so absence is unambiguous only
 *  if the caller never stores NULL values, or uses ht_del to confirm.)
 */
void    *ht_get(const ht *t, const char *key);

/*
 * Delete the entry for key.
 * Leaves a tombstone so existing probe chains remain valid.
 * Returns true if the key was present, false otherwise.
 */
bool     ht_del(ht *t, const char *key);

/*
 * Free all memory owned by the table, including the table itself.
 * Safe to call with NULL.
 */
void     ht_free(ht *t);

/* Create an iterator positioned before the first entry. */
ht_iter  ht_iter_new(ht *t);

/*
 * Advance the iterator to the next live entry.
 * On success, writes the key and value to *key and *value and returns true.
 * Returns false when the table has been fully traversed.
 */
bool     ht_iter_next(ht_iter *it, const char **key, void **value);

#endif /* HT_H */
