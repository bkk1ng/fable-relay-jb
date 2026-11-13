/*
 * ht.h — open-addressing string hash table
 *
 * Keys: NUL-terminated strings (copied on insert).
 * Values: void * (caller-managed lifetime).
 *
 * Collision resolution : linear probing
 * Hash function        : FNV-1a (64-bit)
 * Deletion             : tombstones
 * Resize trigger       : load factor 0.7 (count / capacity), doubling
 */
#ifndef HT_H
#define HT_H

#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ types */

/*
 * Individual table slot.  Internal; do not access fields directly.
 * key == NULL            → slot never used (probe stop)
 * key == HT_TOMBSTONE    → slot was deleted (probe continues)
 * otherwise              → live entry
 */
typedef struct {
    char *key;
    void *value;
} ht_slot_t;

typedef struct {
    ht_slot_t *slots;
    size_t     count;   /* number of live entries */
    size_t     cap;     /* total slot count, always a power of two */
} ht_t;

/*
 * Forward-cursor iterator.  Initialise with ht_iter(), step with
 * ht_iter_next().  key and value are valid until the next call to
 * ht_iter_next() or any mutating operation on the table.
 */
typedef struct {
    const char *key;    /* current entry key   (read-only) */
    void       *value;  /* current entry value (read-only) */
    size_t      _idx;   /* private — do not touch          */
} ht_iter_t;

/* --------------------------------------------------------- public functions */

/*
 * ht_new — allocate and return a new empty table.
 * Returns NULL on allocation failure.
 */
ht_t *ht_new(void);

/*
 * ht_set — insert or update key → value.
 * Copies key internally; caller may free or reuse the string afterwards.
 * Returns true on success, false on allocation failure.
 */
bool ht_set(ht_t *ht, const char *key, void *value);

/*
 * ht_get — look up key.
 * Returns the associated value, or NULL if key is not present.
 */
void *ht_get(const ht_t *ht, const char *key);

/*
 * ht_del — remove key from the table.
 * Returns the value that was associated with key, or NULL if absent.
 * Frees the internally-copied key string.
 */
void *ht_del(ht_t *ht, const char *key);

/*
 * ht_free — destroy the table and free all copied key strings.
 * Values are owned by the caller and are NOT freed.
 * Safe to call with NULL.
 */
void ht_free(ht_t *ht);

/*
 * ht_iter — return an iterator positioned before the first entry.
 */
ht_iter_t ht_iter(const ht_t *ht);

/*
 * ht_iter_next — advance the iterator to the next live entry.
 * Returns true and sets it->key / it->value on each live entry.
 * Returns false when all entries have been visited.
 */
bool ht_iter_next(const ht_t *ht, ht_iter_t *it);

#endif /* HT_H */
