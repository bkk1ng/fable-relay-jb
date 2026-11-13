/*
 * ht.c — open-addressing string hash table (C11)
 *
 * Hashing : FNV-1a 32-bit
 * Collision: linear probing
 * Deletion : tombstones
 * Resize   : double capacity when load (live+dead)/capacity >= 0.7
 */

#include "ht.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define HT_INITIAL_CAPACITY  8u
#define HT_LOAD_NUMERATOR    7u    /* 0.7 expressed as a fraction */
#define HT_LOAD_DENOMINATOR 10u

/* Sentinel pointer used as the key of a tombstone slot.
 * We pick a static char so it has a unique address that no strdup'd
 * string can ever equal.                                                  */
static const char TOMBSTONE_KEY = '\0';
#define TOMBSTONE ((char *)&TOMBSTONE_KEY)

/* -------------------------------------------------------------------------
 * FNV-1a 32-bit hash
 * ---------------------------------------------------------------------- */

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Returns true when the slot is completely empty (not a tombstone). */
static inline bool slot_empty(const ht_entry *e)
{
    return e->key == NULL;
}

/* Returns true when the slot is a tombstone. */
static inline bool slot_dead(const ht_entry *e)
{
    return e->key == TOMBSTONE;
}

/* Returns true when the slot holds a live entry. */
static inline bool slot_live(const ht_entry *e)
{
    return e->key != NULL && e->key != TOMBSTONE;
}

/*
 * Find the slot for key inside a given backing array.
 *
 * Returns the slot where the key lives (if found) or the best slot for
 * insertion.  "Best" means the first tombstone encountered — inserting
 * there keeps the probe chain intact for entries that were placed after
 * the gap.
 *
 * Returns NULL only when capacity is 0 (should never happen in practice).
 */
static ht_entry *find_slot(ht_entry *entries, size_t capacity,
                            const char *key)
{
    if (capacity == 0) return NULL;

    uint32_t hash  = fnv1a(key);
    size_t   index = (size_t)(hash & (uint32_t)(capacity - 1));
    ht_entry *tombstone = NULL;

    for (;;) {
        ht_entry *e = &entries[index];

        if (slot_empty(e)) {
            /* Empty slot ends the chain.  Return the first tombstone if
             * we passed one; otherwise return this empty slot.           */
            return tombstone ? tombstone : e;
        }

        if (slot_dead(e)) {
            /* Record the first tombstone but keep probing — the key
             * might still be alive later in the chain.                   */
            if (!tombstone) tombstone = e;
        } else if (strcmp(e->key, key) == 0) {
            return e;   /* Found the live entry. */
        }

        index = (index + 1) & (capacity - 1);
    }
}

/*
 * Rehash all live entries from old_entries into new_entries.
 * Tombstones are dropped — they are not meaningful after a resize.
 */
static void rehash(ht_entry *new_entries, size_t new_cap,
                   const ht_entry *old_entries, size_t old_cap)
{
    for (size_t i = 0; i < old_cap; i++) {
        const ht_entry *src = &old_entries[i];
        if (!slot_live(src)) continue;

        uint32_t hash  = fnv1a(src->key);
        size_t   index = (size_t)(hash & (uint32_t)(new_cap - 1));

        for (;;) {
            ht_entry *dst = &new_entries[index];
            if (slot_empty(dst)) {
                dst->key   = src->key;   /* ownership transfers */
                dst->value = src->value;
                break;
            }
            index = (index + 1) & (new_cap - 1);
        }
    }
}

/*
 * Grow the backing array to new_cap (must be a power of two and > current).
 * Returns false on allocation failure.
 */
static bool resize(ht *t, size_t new_cap)
{
    ht_entry *new_entries = calloc(new_cap, sizeof(ht_entry));
    if (!new_entries) return false;

    rehash(new_entries, new_cap, t->entries, t->capacity);

    free(t->entries);
    t->entries  = new_entries;
    t->capacity = new_cap;
    t->dead     = 0;   /* tombstones were dropped */
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

ht *ht_new(void)
{
    ht *t = malloc(sizeof(ht));
    if (!t) return NULL;

    t->entries = calloc(HT_INITIAL_CAPACITY, sizeof(ht_entry));
    if (!t->entries) {
        free(t);
        return NULL;
    }

    t->capacity = HT_INITIAL_CAPACITY;
    t->count    = 0;
    t->dead     = 0;
    return t;
}

bool ht_set(ht *t, const char *key, void *value)
{
    if (!t || !key) return false;

    /* Check load factor before inserting.  We count both live entries and
     * tombstones because they both consume probe-chain slots.             */
    if ((t->count + t->dead + 1) * HT_LOAD_DENOMINATOR
            >= t->capacity * HT_LOAD_NUMERATOR) {
        if (!resize(t, t->capacity * 2)) return false;
    }

    ht_entry *e = find_slot(t->entries, t->capacity, key);
    if (!e) return false;

    if (slot_live(e)) {
        /* Update existing entry — key already owned, don't duplicate. */
        e->value = value;
        return true;
    }

    /* New entry — either an empty slot or a tombstone. */
    char *k = strdup(key);
    if (!k) return false;

    if (slot_dead(e)) {
        /* Reusing a tombstone slot: dead count goes down. */
        t->dead--;
    }

    e->key   = k;
    e->value = value;
    t->count++;
    return true;
}

void *ht_get(const ht *t, const char *key)
{
    if (!t || !key || t->capacity == 0) return NULL;

    ht_entry *e = find_slot(t->entries, t->capacity, key);
    if (!e || !slot_live(e)) return NULL;
    return e->value;
}

bool ht_del(ht *t, const char *key)
{
    if (!t || !key || t->count == 0) return false;

    ht_entry *e = find_slot(t->entries, t->capacity, key);
    if (!e || !slot_live(e)) return false;

    free(e->key);
    e->key   = TOMBSTONE;
    e->value = NULL;
    t->count--;
    t->dead++;
    return true;
}

void ht_free(ht *t)
{
    if (!t) return;

    for (size_t i = 0; i < t->capacity; i++) {
        if (slot_live(&t->entries[i])) {
            free(t->entries[i].key);
        }
    }
    free(t->entries);
    free(t);
}

ht_iter ht_iter_new(ht *t)
{
    return (ht_iter){ .table = t, .index = 0 };
}

bool ht_iter_next(ht_iter *it, const char **key, void **value)
{
    if (!it || !it->table) return false;

    ht *t = it->table;

    while (it->index < t->capacity) {
        ht_entry *e = &t->entries[it->index++];
        if (slot_live(e)) {
            if (key)   *key   = e->key;
            if (value) *value = e->value;
            return true;
        }
    }

    return false;
}
