#ifndef HT_H
#define HT_H

#include <stddef.h>

/*
 * ht.h - open-addressing string hash table
 *
 * Hashing:      FNV-1a (64-bit)
 * Collision:     linear probing
 * Deletion:      tombstones (reused on insert, cleared on resize)
 * Load factor:   0.7  (resize triggers before an insert would exceed it)
 * Growth:        capacity doubles when threshold is reached
 *
 * Keys and values are C strings (NUL-terminated).  The table owns
 * malloc'd copies of both; they are freed on ht_del and ht_free.
 *
 * The fields capacity, count and tombstones are readable for size
 * queries but must not be modified directly.
 */

/* Sentinel value that marks a deleted slot. */
#define HT_TOMBSTONE ((char *)1)

/* One slot in the table.
 *   key == NULL           -> empty
 *   key == HT_TOMBSTONE    -> deleted (tombstone)
 *   otherwise              -> occupied
 */
typedef struct {
    char *key;
    char *val;
} ht_entry;

typedef struct {
    ht_entry *entries;
    size_t capacity;    /* total slots, always a power of two */
    size_t count;       /* live entries */
    size_t tombstones;  /* deleted slots awaiting reuse or rehash */
} ht_table;

/* Key-value pair yielded by the iterator. */
typedef struct {
    const char *key;
    const char *val;
} ht_kv;

/*
 * Create a new hash table.
 * Returns NULL on allocation failure.
 */
ht_table *ht_new(void);

/*
 * Insert or update a key-value pair.
 * key and val must be non-NULL.
 * Returns 1 on success, 0 on allocation failure.
 */
int ht_set(ht_table *ht, const char *key, const char *val);

/*
 * Look up a key.
 * Returns the associated value, or NULL if the key is not present.
 */
const char *ht_get(const ht_table *ht, const char *key);

/*
 * Delete a key, freeing the stored copies.
 * Returns 1 if the key was found and removed, 0 if the key was absent.
 */
int ht_del(ht_table *ht, const char *key);

/*
 * Iterate over live entries.
 *
 * Initialise *cursor to 0 before the first call.  Each successful call
 * advances *cursor and fills *out with the next live key-value pair.
 * Returns 1 while an entry was produced, 0 when iteration is done.
 *
 * Do not modify the table (set/del) during iteration.
 */
int ht_iter(const ht_table *ht, size_t *cursor, ht_kv *out);

/*
 * Destroy the table and free every stored string.
 * Safe to call with NULL.
 */
void ht_free(ht_table *ht);

#endif /* HT_H */
