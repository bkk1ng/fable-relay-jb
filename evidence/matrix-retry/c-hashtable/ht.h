/*
 * ht.h — open-addressing string hash table
 *
 * Hash:    FNV-1a 64-bit
 * Probing: linear
 * Deleted: tombstone markers
 * Resize:  double capacity when (live + dead + 1) / capacity > 0.70
 *
 * Keys and values are NUL-terminated strings owned by the table.
 * NULL is a valid value.
 */

#ifndef HT_H
#define HT_H

#include <stdbool.h>
#include <stddef.h>

/* ── types ──────────────────────────────────────────────────────────── */

typedef struct {
    char *key;    /* NULL  → slot never used          */
    char *value;  /* owned copy; NULL is a valid value */
    bool  dead;   /* true  → tombstone (key was deleted) */
} HtEntry;

typedef struct {
    HtEntry *entries;
    size_t   capacity;  /* total allocated slots  */
    size_t   count;     /* live entries           */
    size_t   n_dead;    /* tombstone slots        */
} HtTable;

typedef struct {
    HtTable    *_ht;
    size_t      _idx;
    const char *key;    /* current key   (valid after ht_iter_next → true) */
    char       *value;  /* current value (valid after ht_iter_next → true) */
} HtIter;

/* ── API ────────────────────────────────────────────────────────────── */

/*
 * ht_new — allocate an empty table.
 * Returns NULL on allocation failure.
 */
HtTable *ht_new(void);

/*
 * ht_set — insert or update key→value.
 * Both strings are copied; the caller retains ownership of the originals.
 * Returns true on success, false on allocation failure.
 */
bool ht_set(HtTable *ht, const char *key, const char *value);

/*
 * ht_get — look up key.
 * Returns the stored value string, or NULL if not found.
 * (NULL also means a key was stored with a NULL value; use ht_has if needed.)
 */
char *ht_get(HtTable *ht, const char *key);

/*
 * ht_del — remove key from the table.
 * Returns true if the key existed and was removed, false if not found.
 */
bool ht_del(HtTable *ht, const char *key);

/*
 * ht_free — release all memory owned by the table, including the table itself.
 * Passing NULL is safe.
 */
void ht_free(HtTable *ht);

/*
 * ht_iter — create an iterator positioned before the first entry.
 * Advance with ht_iter_next.
 */
HtIter ht_iter(HtTable *ht);

/*
 * ht_iter_next — step to the next live entry.
 * Returns true and sets it->key / it->value, or returns false when exhausted.
 * Do not modify the table during iteration.
 */
bool ht_iter_next(HtIter *it);

#endif /* HT_H */
