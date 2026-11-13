/* ht.h — open-addressing string hash table (C11)
 *
 * Hash:    FNV-1a 64-bit
 * Probing: linear
 * Delete:  tombstones
 * Resize:  double capacity when count+1 would exceed 70% load factor
 *
 * Keys are copied internally.  Values are caller-owned void pointers.
 * ht_free does NOT free stored values; that is the caller's responsibility.
 */

#ifndef HT_H
#define HT_H

#include <stddef.h>

/* ------------------------------------------------------------------ types */

typedef enum {
    HT_SLOT_EMPTY,
    HT_SLOT_OCCUPIED,
    HT_SLOT_TOMBSTONE
} HtSlotState;

typedef struct {
    HtSlotState  state;
    char        *key;
    void        *value;
} HtSlot;

typedef struct {
    HtSlot *slots;
    size_t  capacity;    /* total slot count, always a power of two */
    size_t  count;       /* live (occupied) entries */
    size_t  tombstones;  /* deleted-but-not-yet-cleared entries */
} HtTable;

typedef struct {
    const HtTable *table;
    size_t         index; /* next slot to examine */
} HtIter;

/* --------------------------------------------------------------- lifetime */

/*
 * ht_new — allocate and initialise an empty hash table.
 * Returns NULL on allocation failure.
 */
HtTable *ht_new(void);

/*
 * ht_free — release all memory owned by the table.
 * Stored keys are freed; stored values are NOT freed.
 * Passing NULL is a no-op.
 */
void ht_free(HtTable *ht);

/* --------------------------------------------------------------- mutation */

/*
 * ht_set — insert or update an entry.
 * The key string is copied; value is stored as-is.
 * Returns  0 on success.
 * Returns -1 on allocation failure.
 */
int ht_set(HtTable *ht, const char *key, void *value);

/*
 * ht_del — remove the entry for key.
 * Returns  0 on success.
 * Returns -1 if key is not present.
 */
int ht_del(HtTable *ht, const char *key);

/* ----------------------------------------------------------------- lookup */

/*
 * ht_get — retrieve the value stored under key.
 * Returns the value pointer, or NULL if the key is absent.
 * Note: NULL is also a valid stored value; prefer ht_get + ht_del patterns
 * carefully when NULL values are used.
 */
void *ht_get(const HtTable *ht, const char *key);

/* --------------------------------------------------------------- iteration */

/*
 * ht_iter — return an iterator positioned before the first live entry.
 * The table must not be mutated while iteration is in progress.
 */
HtIter ht_iter(const HtTable *ht);

/*
 * ht_iter_next — advance to the next live entry.
 * Writes the key and value to *key and *value (either pointer may be NULL
 * if the caller does not need that field).
 * Returns 1 if an entry was produced, 0 when the table is exhausted.
 */
int ht_iter_next(HtIter *iter, const char **key, void **value);

#endif /* HT_H */
