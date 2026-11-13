/* ht.c — open-addressing string hash table (C11)
 *
 * Hash:    FNV-1a 64-bit
 * Probing: linear
 * Delete:  tombstones
 * Resize:  double capacity when (count + tombstones + 1) / capacity
 *          would exceed 0.7 on the next insert
 */

#include "ht.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a 64-bit constants */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

#define HT_INIT_CAPACITY 16

/* ---------------------------------------------------------------- helpers */

static uint64_t fnv1a_hash(const char *key) {
    uint64_t hash = FNV_OFFSET;
    const unsigned char *p = (const unsigned char *)key;
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= FNV_PRIME;
    }
    return hash;
}

/* Duplicate a C string. Returns NULL on failure. */
static char *ht_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* Returns nonzero when the table should grow before another insert.
 * Tombstones are counted alongside live entries so that probing is
 * guaranteed to find an empty slot. */
static int ht_should_grow(const HtTable *ht) {
    /* (count + tombstones + 1) / capacity > 0.7
     *  ==> (count + tombstones + 1) * 10 > capacity * 7 */
    return (ht->count + ht->tombstones + 1) * 10 > ht->capacity * 7;
}

/* Allocate a fresh slots array, all EMPTY. Returns NULL on failure. */
static HtSlot *ht_alloc_slots(size_t capacity) {
    HtSlot *slots = calloc(capacity, sizeof(HtSlot));
    if (!slots) return NULL;
    for (size_t i = 0; i < capacity; i++)
        slots[i].state = HT_SLOT_EMPTY;
    return slots;
}

/* Rehash all occupied entries into a new array of new_capacity.
 * Tombstones are discarded (their keys were already freed by ht_del).
 * Frees the old slots array; key pointers are moved, not copied.
 * Returns 0 on success, -1 on allocation failure (table unchanged). */
static int ht_resize(HtTable *ht, size_t new_capacity) {
    HtSlot *old_slots = ht->slots;
    size_t old_capacity = ht->capacity;

    HtSlot *new_slots = ht_alloc_slots(new_capacity);
    if (!new_slots) return -1;

    size_t mask = new_capacity - 1;

    for (size_t i = 0; i < old_capacity; i++) {
        if (old_slots[i].state == HT_SLOT_OCCUPIED) {
            size_t idx = fnv1a_hash(old_slots[i].key) & mask;
            while (new_slots[idx].state == HT_SLOT_OCCUPIED)
                idx = (idx + 1) & mask;
            new_slots[idx] = old_slots[i];
        }
    }

    free(old_slots);
    ht->slots = new_slots;
    ht->capacity = new_capacity;
    ht->tombstones = 0;
    /* count is unchanged — only occupied entries were moved */
    return 0;
}

/* --------------------------------------------------------------- lifetime */

HtTable *ht_new(void) {
    HtTable *ht = malloc(sizeof(HtTable));
    if (!ht) return NULL;

    ht->capacity = HT_INIT_CAPACITY;
    ht->count = 0;
    ht->tombstones = 0;
    ht->slots = ht_alloc_slots(ht->capacity);
    if (!ht->slots) {
        free(ht);
        return NULL;
    }
    return ht;
}

void ht_free(HtTable *ht) {
    if (!ht) return;

    for (size_t i = 0; i < ht->capacity; i++) {
        if (ht->slots[i].state == HT_SLOT_OCCUPIED)
            free(ht->slots[i].key);
    }
    free(ht->slots);
    free(ht);
}

/* --------------------------------------------------------------- mutation */

int ht_set(HtTable *ht, const char *key, void *value) {
    if (!ht || !key) return -1;

    if (ht_should_grow(ht)) {
        if (ht_resize(ht, ht->capacity * 2) != 0)
            return -1;
    }

    size_t mask = ht->capacity - 1;
    size_t idx = fnv1a_hash(key) & mask;
    size_t first_tombstone = SIZE_MAX;

    while (ht->slots[idx].state != HT_SLOT_EMPTY) {
        if (ht->slots[idx].state == HT_SLOT_OCCUPIED) {
            if (strcmp(ht->slots[idx].key, key) == 0) {
                /* key exists — replace value */
                ht->slots[idx].value = value;
                return 0;
            }
        } else { /* tombstone */
            if (first_tombstone == SIZE_MAX)
                first_tombstone = idx;
        }
        idx = (idx + 1) & mask;
    }

    /* Key not found. Insert at the first tombstone if one was seen,
     * otherwise at the empty slot we landed on. */
    size_t insert_idx = (first_tombstone != SIZE_MAX) ? first_tombstone : idx;

    char *new_key = ht_strdup(key);
    if (!new_key) return -1;

    ht->slots[insert_idx].state = HT_SLOT_OCCUPIED;
    ht->slots[insert_idx].key = new_key;
    ht->slots[insert_idx].value = value;

    if (first_tombstone != SIZE_MAX)
        ht->tombstones--;
    ht->count++;

    return 0;
}

int ht_del(HtTable *ht, const char *key) {
    if (!ht || !key) return -1;

    size_t mask = ht->capacity - 1;
    size_t idx = fnv1a_hash(key) & mask;

    while (ht->slots[idx].state != HT_SLOT_EMPTY) {
        if (ht->slots[idx].state == HT_SLOT_OCCUPIED &&
            strcmp(ht->slots[idx].key, key) == 0) {
            free(ht->slots[idx].key);
            ht->slots[idx].key = NULL;
            ht->slots[idx].value = NULL;
            ht->slots[idx].state = HT_SLOT_TOMBSTONE;
            ht->count--;
            ht->tombstones++;
            return 0;
        }
        idx = (idx + 1) & mask;
    }

    return -1; /* not found */
}

/* ----------------------------------------------------------------- lookup */

void *ht_get(const HtTable *ht, const char *key) {
    if (!ht || !key) return NULL;

    size_t mask = ht->capacity - 1;
    size_t idx = fnv1a_hash(key) & mask;

    while (ht->slots[idx].state != HT_SLOT_EMPTY) {
        if (ht->slots[idx].state == HT_SLOT_OCCUPIED &&
            strcmp(ht->slots[idx].key, key) == 0) {
            return ht->slots[idx].value;
        }
        idx = (idx + 1) & mask;
    }

    return NULL;
}

/* --------------------------------------------------------------- iteration */

HtIter ht_iter(const HtTable *ht) {
    HtIter iter;
    iter.table = ht;
    iter.index = 0;
    return iter;
}

int ht_iter_next(HtIter *iter, const char **key, void **value) {
    if (!iter || !iter->table) return 0;

    const HtTable *ht = iter->table;
    while (iter->index < ht->capacity) {
        size_t i = iter->index++;
        if (ht->slots[i].state == HT_SLOT_OCCUPIED) {
            if (key)   *key   = ht->slots[i].key;
            if (value) *value = ht->slots[i].value;
            return 1;
        }
    }
    return 0;
}
