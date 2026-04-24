/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cache.c
    TTL cache with LRU eviction.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-cache.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-time.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("data");

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

#define CACHE_KEY_MAX  128

typedef struct {
    char     key[CACHE_KEY_MAX];
    uint64_t timestamp_ms;
    bool     valid;
} CacheSlot;

struct AxlCache {
    CacheSlot *slots;
    uint8_t   *values;       /* flat array: slot[i] value at values[i * entry_size] */
    size_t     max_slots;
    size_t     entry_size;
    uint64_t   ttl_ms;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlCache *
axl_cache_new(size_t max_slots, size_t entry_size, uint64_t ttl_ms)
{
    AxlCache *c;

    if (max_slots == 0 || entry_size == 0) {
        return NULL;
    }

    c = (AxlCache *)axl_malloc(sizeof(AxlCache));
    if (c == NULL) {
        axl_error("cache alloc failed");
        return NULL;
    }

    c->max_slots = max_slots;
    c->entry_size = entry_size;
    c->ttl_ms = ttl_ms;

    c->slots = (CacheSlot *)axl_calloc(max_slots, sizeof(CacheSlot));
    c->values = (uint8_t *)axl_calloc(max_slots, entry_size);

    if (c->slots == NULL || c->values == NULL) {
        axl_error("cache slots/values alloc failed: %zu slots", max_slots);
        axl_free(c->slots);
        axl_free(c->values);
        axl_free(c);
        return NULL;
    }

    return c;
}

static CacheSlot *
find_slot(AxlCache *c, const char *key)
{
    for (size_t i = 0; i < c->max_slots; i++) {
        if (c->slots[i].valid && axl_streql(c->slots[i].key, key)) {
            return &c->slots[i];
        }
    }
    return NULL;
}

static bool
is_expired(AxlCache *c, CacheSlot *s)
{
    uint64_t now = axl_time_get_ms();

    return (now - s->timestamp_ms) > c->ttl_ms;
}

static size_t
slot_index(AxlCache *c, CacheSlot *s)
{
    return (size_t)(s - c->slots);
}

int
axl_cache_put(AxlCache *c, const char *key, const void *value)
{
    CacheSlot *slot;
    size_t     idx;

    if (c == NULL || key == NULL || value == NULL) {
        return -1;
    }

    /* Update existing? */
    slot = find_slot(c, key);
    if (slot != NULL) {
        idx = slot_index(c, slot);
        axl_memcpy(c->values + idx * c->entry_size, value, c->entry_size);
        slot->timestamp_ms = axl_time_get_ms();
        return 0;
    }

    /* Find empty slot */
    for (size_t i = 0; i < c->max_slots; i++) {
        if (!c->slots[i].valid) {
            slot = &c->slots[i];
            idx = i;
            goto fill;
        }
    }

    /* Evict oldest (LRU) */
    {
        uint64_t oldest_ts = ~(uint64_t)0;
        size_t   oldest_idx = 0;

        for (size_t i = 0; i < c->max_slots; i++) {
            if (c->slots[i].timestamp_ms < oldest_ts) {
                oldest_ts = c->slots[i].timestamp_ms;
                oldest_idx = i;
            }
        }
        slot = &c->slots[oldest_idx];
        idx = oldest_idx;
        axl_debug("cache evicting key '%s'", slot->key);
    }

fill:
    axl_strlcpy(slot->key, key, CACHE_KEY_MAX);
    slot->timestamp_ms = axl_time_get_ms();
    slot->valid = true;
    axl_memcpy(c->values + idx * c->entry_size, value, c->entry_size);
    return 0;
}

int
axl_cache_get(AxlCache *c, const char *key, void *value)
{
    CacheSlot *slot;
    size_t     idx;

    if (c == NULL || key == NULL || value == NULL) {
        return -1;
    }

    slot = find_slot(c, key);
    if (slot == NULL) {
        return -1;
    }

    if (is_expired(c, slot)) {
        slot->valid = false;
        return -1;
    }

    /* Refresh timestamp on hit (LRU) */
    slot->timestamp_ms = axl_time_get_ms();

    idx = slot_index(c, slot);
    axl_memcpy(value, c->values + idx * c->entry_size, c->entry_size);
    return 0;
}

void
axl_cache_invalidate(AxlCache *c, const char *key)
{
    CacheSlot *slot;

    if (c == NULL || key == NULL) {
        return;
    }

    slot = find_slot(c, key);
    if (slot != NULL) {
        slot->valid = false;
        axl_memset(c->values + slot_index(c, slot) * c->entry_size,
                   0, c->entry_size);
    }
}

void
axl_cache_free(AxlCache *c)
{
    if (c == NULL) {
        return;
    }

    axl_free(c->slots);
    axl_free(c->values);
    axl_free(c);
}
