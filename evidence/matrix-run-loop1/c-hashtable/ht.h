/*
 * ht.h — open-addressing string hash table
 *
 * Hash:    FNV-1a (64-bit)
 * Probe:   linear
 * Delete:  tombstones
 * Resize:  double capacity when load > 0.7
 *
 * Keys are NUL-terminated strings (copied on insert).
 * Values are void pointers (caller-managed memory).
 *
 * C11, no external dependencies.
 */

#ifndef HT_H
#define HT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── types ───────────────────────────────────────────────────────────── */

typedef struct ht_entry {
    char       *key;        /* NULL → empty; HT_TOMBSTONE → deleted */
    void       *value;
} ht_entry_t;

typedef struct ht {
    ht_entry_t *entries;
    size_t      capacity;   /* always a power of two */
    size_t      count;      /* live entries */
    size_t      tombstones; /* deleted slots still occupying load */
} ht_t;

/* Sentinel pointer stored in ht_entry_t.key to mark a tombstone slot.
 * Never compare user keys against this address; use ht__is_tombstone(). */
extern char _ht_tombstone_sentinel;
#define HT_TOMBSTONE (&_ht_tombstone_sentinel)

/* ── iterator ────────────────────────────────────────────────────────── */

typedef struct ht_iter {
    const ht_t *_ht;
    size_t      _idx;
    /* public (read-only after ht_iter_next) */
    const char *key;
    void       *value;
} ht_iter_t;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * ht_new – allocate and initialise a new hash table.
 * Returns NULL on allocation failure.
 */
ht_t *ht_new(void);

/*
 * ht_free – release all internal memory.
 * Does NOT free the values stored by the caller.
 * After this call *ht is invalid; the pointer itself is freed.
 */
void ht_free(ht_t *ht);

/*
 * ht_set – insert or update the entry for `key`.
 * The key string is copied internally.
 * Returns true on success, false on allocation failure.
 * On update the old value is overwritten (not freed).
 */
bool ht_set(ht_t *ht, const char *key, void *value);

/*
 * ht_get – look up `key`.
 * Returns the stored value, or NULL if not found.
 * (NULL is also a valid stored value; use ht_has() if you need to
 *  distinguish "absent" from "present with value NULL".)
 */
void *ht_get(const ht_t *ht, const char *key);

/*
 * ht_del – remove the entry for `key`.
 * Returns true if the key was present and removed, false if absent.
 * Does NOT free the stored value.
 */
bool ht_del(ht_t *ht, const char *key);

/*
 * ht_count – number of live (non-deleted) entries.
 */
size_t ht_count(const ht_t *ht);

/* ── iteration ───────────────────────────────────────────────────────── */

/*
 * ht_iter – initialise an iterator over `ht`.
 * Call ht_iter_next() to advance to the first entry before reading
 * iter.key / iter.value.
 */
ht_iter_t ht_iter(const ht_t *ht);

/*
 * ht_iter_next – advance the iterator.
 * Returns true and sets iter->key / iter->value when an entry is found.
 * Returns false when the table is exhausted.
 *
 * Modifying the table during iteration produces undefined behaviour.
 */
bool ht_iter_next(ht_iter_t *iter);

#ifdef __cplusplus
}
#endif

#endif /* HT_H */
