#include "ht.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a 64-bit constants */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

#define HT_LOAD_FACTOR 0.7
#define HT_INIT_CAP    16

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* strdup replacement (avoids POSIX dependency) */
static char *xstrdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static uint64_t fnv1a(const char *key)
{
    uint64_t hash = FNV_OFFSET;
    const unsigned char *p = (const unsigned char *)key;
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= FNV_PRIME;
    }
    return hash;
}

/*
 * Probe the entry array for key.
 *
 * When the key is found, returns 1 and sets *found_idx to the matching
 * slot (and *ins_idx to the same slot).
 *
 * When the key is absent, returns 0 and sets *ins_idx to the best slot
 * for insertion: the first tombstone seen during probing, or the first
 * empty slot if no tombstone was encountered.
 *
 * The table is guaranteed to contain at least one empty slot (the load
 * factor is kept below 0.7), so the probe always terminates.
 */
static int find_slot(const ht_entry *entries, size_t cap, const char *key,
                     size_t *found_idx, size_t *ins_idx)
{
    size_t start = (size_t)(fnv1a(key) % cap);
    size_t first_tomb = cap;   /* cap acts as "no tombstone seen yet" */
    size_t i = start;

    for (;;) {
        if (entries[i].key == NULL) {
            /* empty slot - key is absent */
            *ins_idx = (first_tomb < cap) ? first_tomb : i;
            return 0;
        }
        if (entries[i].key == HT_TOMBSTONE) {
            if (first_tomb == cap)
                first_tomb = i;
        } else if (strcmp(entries[i].key, key) == 0) {
            *found_idx = i;
            *ins_idx = i;
            return 1;
        }
        i = (i + 1) % cap;
    }
}

/*
 * Rehash all live entries into a new, zero-initialised array of new_cap
 * slots.  Tombstones are discarded.  Returns 1 on success, 0 on
 * allocation failure (the table is left unchanged on failure).
 */
static int ht_resize(ht_table *ht, size_t new_cap)
{
    ht_entry *old = ht->entries;
    size_t old_cap = ht->capacity;

    ht_entry *fresh = calloc(new_cap, sizeof(ht_entry));
    if (!fresh)
        return 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].key != NULL && old[i].key != HT_TOMBSTONE) {
            size_t fi, ii;
            find_slot(fresh, new_cap, old[i].key, &fi, &ii);
            fresh[ii].key = old[i].key;
            fresh[ii].val = old[i].val;
        }
    }

    free(old);
    ht->entries    = fresh;
    ht->capacity   = new_cap;
    ht->tombstones = 0;
    /* count is unchanged: the same live entries were moved */
    return 1;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

ht_table *ht_new(void)
{
    ht_table *ht = malloc(sizeof(*ht));
    if (!ht)
        return NULL;

    ht->entries = calloc(HT_INIT_CAP, sizeof(ht_entry));
    if (!ht->entries) {
        free(ht);
        return NULL;
    }

    ht->capacity   = HT_INIT_CAP;
    ht->count      = 0;
    ht->tombstones = 0;
    return ht;
}

int ht_set(ht_table *ht, const char *key, const char *val)
{
    if (!ht || !key || !val)
        return 0;

    /* Resize before inserting so a slot is always available.
     * Tombstones are counted because they also occupy space. */
    if ((double)(ht->count + ht->tombstones + 1)
        >= (double)ht->capacity * HT_LOAD_FACTOR)
    {
        if (!ht_resize(ht, ht->capacity * 2))
            return 0;
    }

    size_t fi, ii;
    if (find_slot(ht->entries, ht->capacity, key, &fi, &ii)) {
        /* key exists - update value in place */
        char *nv = xstrdup(val);
        if (!nv)
            return 0;
        free(ht->entries[fi].val);
        ht->entries[fi].val = nv;
        return 1;
    }

    /* insert into a fresh slot */
    char *nk = xstrdup(key);
    char *nv = xstrdup(val);
    if (!nk || !nv) {
        free(nk);
        free(nv);
        return 0;
    }

    if (ht->entries[ii].key == HT_TOMBSTONE)
        ht->tombstones--;

    ht->entries[ii].key = nk;
    ht->entries[ii].val = nv;
    ht->count++;
    return 1;
}

const char *ht_get(const ht_table *ht, const char *key)
{
    if (!ht || !key)
        return NULL;

    size_t i = (size_t)(fnv1a(key) % ht->capacity);
    for (;;) {
        if (ht->entries[i].key == NULL)
            return NULL;
        if (ht->entries[i].key != HT_TOMBSTONE
            && strcmp(ht->entries[i].key, key) == 0)
            return ht->entries[i].val;
        i = (i + 1) % ht->capacity;
    }
}

int ht_del(ht_table *ht, const char *key)
{
    if (!ht || !key)
        return 0;

    size_t i = (size_t)(fnv1a(key) % ht->capacity);
    for (;;) {
        if (ht->entries[i].key == NULL)
            return 0;
        if (ht->entries[i].key != HT_TOMBSTONE
            && strcmp(ht->entries[i].key, key) == 0)
        {
            free(ht->entries[i].key);
            free(ht->entries[i].val);
            ht->entries[i].key = HT_TOMBSTONE;
            ht->entries[i].val = NULL;
            ht->count--;
            ht->tombstones++;
            return 1;
        }
        i = (i + 1) % ht->capacity;
    }
}

int ht_iter(const ht_table *ht, size_t *cursor, ht_kv *out)
{
    if (!ht || !cursor || !out)
        return 0;

    while (*cursor < ht->capacity) {
        ht_entry *e = &ht->entries[*cursor];
        (*cursor)++;
        if (e->key != NULL && e->key != HT_TOMBSTONE) {
            out->key = e->key;
            out->val = e->val;
            return 1;
        }
    }
    return 0;
}

void ht_free(ht_table *ht)
{
    if (!ht)
        return;

    for (size_t i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].key != NULL && ht->entries[i].key != HT_TOMBSTONE) {
            free(ht->entries[i].key);
            free(ht->entries[i].val);
        }
    }
    free(ht->entries);
    free(ht);
}
