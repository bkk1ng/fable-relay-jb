/*
 * ht.c — open-addressing string hash table
 *
 * Hash:    FNV-1a (64-bit)
 * Probe:   linear
 * Delete:  tombstones
 * Resize:  double capacity when load > 0.7
 *
 * C11, no external dependencies.
 * Build:   cc -std=c11 -Wall -Wextra -O2 -c ht.c
 */

#include "ht.h"

#include <stdlib.h>
#include <string.h>

/* ── sentinel ─────────────────────────────────────────────────────────── *
 * Distinct address used as the tombstone marker in ht_entry_t.key.
 * Never dereferenced; it exists only so its address is unique.
 */
char _ht_tombstone_sentinel;

#define HT_IS_TOMBSTONE(e) ((e).key == HT_TOMBSTONE)
#define HT_IS_EMPTY(e)     ((e).key == NULL)

/* ── constants ────────────────────────────────────────────────────────── */

#define HT_INITIAL_CAPACITY 16u          /* must be a power of two */
#define HT_LOAD_FACTOR_NUM  7u           /* load > 7/10 triggers resize */
#define HT_LOAD_FACTOR_DEN  10u

/* ── FNV-1a 64-bit ────────────────────────────────────────────────────── */

static uint64_t ht_hash(const char *key)
{
    /* FNV-1a: xor then multiply, byte by byte.
     * Good distribution for short strings, cheap to compute. */
    static const uint64_t fnv_offset = 1469598103934665603ULL;
    static const uint64_t fnv_prime  = 1099511628211ULL;

    uint64_t hash = fnv_offset;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= fnv_prime;
    }
    return hash;
}

/* ── helpers ──────────────────────────────────────────────────────────── */

/* Returns true if n is a power of two (and n != 0). */
static bool ht_is_power_of_two(size_t n)
{
    return (n != 0) && ((n & (n - 1)) == 0);
}

/* Duplicate a key string. Returns NULL on allocation failure. */
static char *ht_dup_key(const char *key)
{
    size_t len = strlen(key) + 1;
    char  *copy = (char *)malloc(len);
    if (copy) memcpy(copy, key, len);
    return copy;
}

/* ── resize ───────────────────────────────────────────────────────────── */

/* Grow the table to new_capacity, reinserting every live entry.
 * Tombstones are dropped during rehash, which also resets probe lengths.
 * Returns true on success, false on allocation failure (table untouched). */
static bool ht_resize(ht_t *ht, size_t new_capacity)
{
    ht_entry_t *old_entries = ht->entries;
    size_t      old_capacity = ht->capacity;

    ht_entry_t *new_entries =
        (ht_entry_t *)calloc(new_capacity, sizeof(ht_entry_t));
    if (!new_entries) return false;

    ht->entries    = new_entries;
    ht->capacity   = new_capacity;
    ht->count      = 0;
    ht->tombstones = 0;

    for (size_t i = 0; i < old_capacity; ++i) {
        ht_entry_t *e = &old_entries[i];
        if (HT_IS_EMPTY(*e) || HT_IS_TOMBSTONE(*e)) continue;

        /* Reinsert live entry into the fresh table. Linear probe. */
        size_t idx = (size_t)ht_hash(e->key) & (new_capacity - 1);
        while (!HT_IS_EMPTY(new_entries[idx]))
            idx = (idx + 1) & (new_capacity - 1);

        new_entries[idx] = *e;
        ht->count++;
    }

    free(old_entries);
    return true;
}

/* Double the table capacity. Computed load includes tombstones so that
 * a table full of nothing but tombstones still triggers a rehash. */
static bool ht_grow(ht_t *ht)
{
    return ht_resize(ht, ht->capacity * 2);
}

/* ── public API ───────────────────────────────────────────────────────── */

ht_t *ht_new(void)
{
    ht_t *ht = (ht_t *)malloc(sizeof(ht_t));
    if (!ht) return NULL;

    ht->entries = (ht_entry_t *)calloc(HT_INITIAL_CAPACITY,
                                        sizeof(ht_entry_t));
    if (!ht->entries) {
        free(ht);
        return NULL;
    }

    ht->capacity   = HT_INITIAL_CAPACITY;
    ht->count      = 0;
    ht->tombstones = 0;
    return ht;
}

void ht_free(ht_t *ht)
{
    if (!ht) return;

    /* Free copied key strings for live entries only.
     * Value pointers are caller-owned and never freed here. */
    for (size_t i = 0; i < ht->capacity; ++i) {
        ht_entry_t *e = &ht->entries[i];
        if (!HT_IS_EMPTY(*e) && !HT_IS_TOMBSTONE(*e))
            free(e->key);
    }
    free(ht->entries);
    free(ht);
}

bool ht_set(ht_t *ht, const char *key, void *value)
{
    if (!ht || !key) return false;

    /* Resize if the load (count + tombstones) would exceed the factor. */
    size_t occupied = ht->count + ht->tombstones;
    if (occupied + 1 > (ht->capacity * HT_LOAD_FACTOR_NUM)
                        / HT_LOAD_FACTOR_DEN) {
        if (!ht_grow(ht)) return false;
    }

    uint64_t hash = ht_hash(key);
    size_t   idx  = (size_t)hash & (ht->capacity - 1);

    /* Linear probe, remember the first tombstone we see so we can
     * reuse it rather than appending at the end of the probe chain. */
    size_t first_tomb = (size_t)-1;

    for (;;) {
        ht_entry_t *e = &ht->entries[idx];

        if (HT_IS_EMPTY(*e)) {
            /* Key not present: insert here, or at the first tombstone. */
            size_t target = (first_tomb != (size_t)-1) ? first_tomb : idx;

            char *new_key = ht_dup_key(key);
            if (!new_key) return false;

            ht->entries[target].key   = new_key;
            ht->entries[target].value = value;

            if (target == first_tomb) {
                /* Reusing a tombstone slot. */
                ht->tombstones--;
            }
            ht->count++;
            return true;
        }

        if (HT_IS_TOMBSTONE(*e)) {
            if (first_tomb == (size_t)-1) first_tomb = idx;
        } else if (strcmp(e->key, key) == 0) {
            /* Update existing entry. Old value replaced (not freed). */
            e->value = value;
            return true;
        }

        idx = (idx + 1) & (ht->capacity - 1);
    }
}

void *ht_get(const ht_t *ht, const char *key)
{
    if (!ht || !key) return NULL;

    uint64_t hash = ht_hash(key);
    size_t   idx  = (size_t)hash & (ht->capacity - 1);

    for (size_t probes = 0; probes < ht->capacity; ++probes) {
        ht_entry_t *e = &ht->entries[idx];

        if (HT_IS_EMPTY(*e))
            return NULL;                       /* not found */
        if (!HT_IS_TOMBSTONE(*e) && strcmp(e->key, key) == 0)
            return e->value;
        /* tombstone or mismatch: keep probing */

        idx = (idx + 1) & (ht->capacity - 1);
    }
    return NULL;
}

bool ht_del(ht_t *ht, const char *key)
{
    if (!ht || !key) return false;

    uint64_t hash = ht_hash(key);
    size_t   idx  = (size_t)hash & (ht->capacity - 1);

    for (size_t probes = 0; probes < ht->capacity; ++probes) {
        ht_entry_t *e = &ht->entries[idx];

        if (HT_IS_EMPTY(*e))
            return false;                       /* not present */
        if (!HT_IS_TOMBSTONE(*e) && strcmp(e->key, key) == 0) {
            free(e->key);
            e->key   = HT_TOMBSTONE;
            e->value = NULL;
            ht->count--;
            ht->tombstones++;
            return true;
        }

        idx = (idx + 1) & (ht->capacity - 1);
    }
    return false;
}

size_t ht_count(const ht_t *ht)
{
    return ht ? ht->count : 0;
}

/* ── iteration ────────────────────────────────────────────────────────── */

ht_iter_t ht_iter(const ht_t *ht)
{
    ht_iter_t iter;
    iter._ht  = ht;
    iter._idx = 0;
    iter.key  = NULL;
    iter.value = NULL;
    return iter;
}

bool ht_iter_next(ht_iter_t *iter)
{
    if (!iter || !iter->_ht) return false;

    const ht_t *ht = iter->_ht;

    while (iter->_idx < ht->capacity) {
        const ht_entry_t *e = &ht->entries[iter->_idx];
        iter->_idx++;

        if (!HT_IS_EMPTY(*e) && !HT_IS_TOMBSTONE(*e)) {
            iter->key   = e->key;
            iter->value = e->value;
            return true;
        }
    }

    iter->key   = NULL;
    iter->value = NULL;
    return false;
}
