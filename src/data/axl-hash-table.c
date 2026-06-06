/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hash-table.c
    GLib-style chained hash table with FNV-1a hashing.
    Generic keys via user-provided hash/equal callbacks.
    Resizes at 75% load factor.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-list.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("hash");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define INITIAL_BUCKETS  64
#define LOAD_FACTOR_NUM  3
#define LOAD_FACTOR_DEN  4

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct hash_node {
    void              *key;
    void              *value;
    struct hash_node  *next;
} hash_node;

struct AxlHashTable {
    hash_node        **buckets;
    size_t             bucket_count;
    size_t             entry_count;
    AxlHashFunc        hash_func;
    AxlEqualFunc       equal_func;
    AxlDestroyNotify   key_destroy;
    AxlDestroyNotify   value_destroy;
    bool               copy_keys;
};

// ---------------------------------------------------------------------------
// Built-in hash and equality functions
// ---------------------------------------------------------------------------

// axl_str_hash and axl_str_equal are defined in axl-str.c

// Avalanche a raw integer into a well-distributed hash. Shared by the
// pointer/int/int64/double hashers below.
static size_t
hash_finalize(size_t v)
{
#if defined(MDE_CPU_X64) || defined(MDE_CPU_AARCH64)
    // splitmix64 finalizer
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
#else
    // 32-bit finalizer
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
#endif
    return v;
}

size_t
axl_direct_hash(const void *key)
{
    return hash_finalize((size_t)key);
}

bool
axl_direct_equal(const void *a, const void *b)
{
    return a == b;
}

size_t
axl_int_hash(const void *key)
{
    return hash_finalize((size_t)(uint32_t)*(const int *)key);
}

bool
axl_int_equal(const void *a, const void *b)
{
    return *(const int *)a == *(const int *)b;
}

size_t
axl_int64_hash(const void *key)
{
    return hash_finalize((size_t)(uint64_t)*(const int64_t *)key);
}

bool
axl_int64_equal(const void *a, const void *b)
{
    return *(const int64_t *)a == *(const int64_t *)b;
}

size_t
axl_double_hash(const void *key)
{
    double d = *(const double *)key;
    uint64_t bits;
    // Normalize -0.0 to +0.0 so the two (which compare equal under
    // axl_double_equal) hash identically.
    if (d == 0.0) {
        bits = 0;
    } else {
        axl_memcpy(&bits, &d, sizeof(bits));
    }
    return hash_finalize((size_t)bits);
}

bool
axl_double_equal(const void *a, const void *b)
{
    return *(const double *)a == *(const double *)b;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Copy a string key (only used when copy_keys is true).
static void *
copy_str_key(const void *key)
{
    size_t len = axl_strlen((const char *)key);
    char *copy = axl_malloc(len + 1);
    if (copy != NULL) {
        axl_memcpy(copy, key, len + 1);
    }
    return copy;
}

/// Free a key according to table settings.
static void
free_key(struct AxlHashTable *table, void *key)
{
    if (table->copy_keys) {
        axl_free(key);
    } else if (table->key_destroy != NULL) {
        table->key_destroy(key);
    }
}

/// Free a value if the table has a value destructor.
static void
free_value(struct AxlHashTable *table, void *value)
{
    if (table->value_destroy != NULL) {
        table->value_destroy(value);
    }
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

static int
resize(struct AxlHashTable *table)
{
    size_t      new_count;
    hash_node **new_buckets;
    hash_node  *node;
    hash_node  *next;
    size_t      idx;

    new_count = table->bucket_count * 2;
    new_buckets = axl_calloc(1, new_count * sizeof(hash_node *));
    if (new_buckets == NULL) {
        axl_error("failed to resize bucket array to %llu",
                  (unsigned long long)new_count);
        return -1;
    }

    for (size_t i = 0; i < table->bucket_count; i++) {
        node = table->buckets[i];
        while (node != NULL) {
            next = node->next;
            idx = table->hash_func(node->key) % new_count;
            node->next = new_buckets[idx];
            new_buckets[idx] = node;
            node = next;
        }
    }

    axl_free(table->buckets);
    table->buckets = new_buckets;
    table->bucket_count = new_count;

    return 0;
}

// ---------------------------------------------------------------------------
// GLib-parity bulk operations
// ---------------------------------------------------------------------------

bool
axl_hash_table_add(AxlHashTable *h, void *key)
{
    return axl_hash_table_replace(h, key, key) == AXL_HASH_TABLE_NEW;
}

void
axl_hash_table_remove_all(AxlHashTable *h)
{
    if (h == NULL) {
        return;
    }

    for (size_t i = 0; i < h->bucket_count; i++) {
        hash_node *node = h->buckets[i];
        while (node != NULL) {
            hash_node *next = node->next;
            free_key(h, node->key);
            free_value(h, node->value);
            axl_free(node);
            node = next;
        }
        h->buckets[i] = NULL;
    }
    h->entry_count = 0;
}

void *
axl_hash_table_find(
    AxlHashTable         *h,
    AxlHashTableFindFunc  predicate,
    void                 *data
    )
{
    if (h == NULL || predicate == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < h->bucket_count; i++) {
        for (hash_node *node = h->buckets[i]; node != NULL; node = node->next) {
            if (predicate(node->key, node->value, data)) {
                return node->value;
            }
        }
    }
    return NULL;
}

AxlList *
axl_hash_table_get_keys(AxlHashTable *h)
{
    AxlList *list = NULL;

    if (h == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < h->bucket_count; i++) {
        for (hash_node *node = h->buckets[i]; node != NULL; node = node->next) {
            list = axl_list_prepend(list, node->key);
        }
    }
    return list;
}

AxlList *
axl_hash_table_get_values(AxlHashTable *h)
{
    AxlList *list = NULL;

    if (h == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < h->bucket_count; i++) {
        for (hash_node *node = h->buckets[i]; node != NULL; node = node->next) {
            list = axl_list_prepend(list, node->value);
        }
    }
    return list;
}

// ---------------------------------------------------------------------------
// Unlink a node from its bucket chain
// ---------------------------------------------------------------------------

static void
unlink_node(struct AxlHashTable *table, size_t bucket, hash_node *node)
{
    hash_node **prev = &table->buckets[bucket];

    while (*prev != NULL) {
        if (*prev == node) {
            *prev = node->next;
            table->entry_count--;
            return;
        }
        prev = &(*prev)->next;
    }
}

// ===========================================================================
// Constructors
// ===========================================================================

static struct AxlHashTable *
hash_table_alloc(
    AxlHashFunc      hash_func,
    AxlEqualFunc     equal_func,
    AxlDestroyNotify key_destroy,
    AxlDestroyNotify value_destroy,
    bool             copy_keys
    )
{
    struct AxlHashTable *table;

    table = axl_calloc(1, sizeof(struct AxlHashTable));
    if (table == NULL) {
        return NULL;
    }

    table->buckets = axl_calloc(1, INITIAL_BUCKETS * sizeof(hash_node *));
    if (table->buckets == NULL) {
        axl_free(table);
        return NULL;
    }

    table->bucket_count = INITIAL_BUCKETS;
    table->entry_count = 0;
    table->hash_func = hash_func != NULL ? hash_func : axl_str_hash;
    table->equal_func = equal_func != NULL ? equal_func : axl_str_equal;
    table->key_destroy = key_destroy;
    table->value_destroy = value_destroy;
    table->copy_keys = copy_keys;

    return table;
}

AxlHashTable *
axl_hash_table_new_str(void)
{
    return hash_table_alloc(axl_str_hash, axl_str_equal,
                            NULL, NULL, true);
}

AxlHashTable *
axl_hash_table_new(
    AxlHashFunc  hash_func,
    AxlEqualFunc equal_func
    )
{
    return hash_table_alloc(hash_func, equal_func,
                            NULL, NULL, false);
}

AxlHashTable *
axl_hash_table_new_full(
    AxlHashFunc      hash_func,
    AxlEqualFunc     equal_func,
    AxlDestroyNotify key_destroy,
    AxlDestroyNotify value_destroy
    )
{
    return hash_table_alloc(hash_func, equal_func,
                            key_destroy, value_destroy, false);
}

// ===========================================================================
// Core operations
// ===========================================================================

/// Internal insert/replace with a flag controlling key behavior.
/// keep_old_key=true is GLib "insert", keep_old_key=false is GLib "replace".
/// Returns one of the AxlHashTableInsertResult values.
static AxlHashTableInsertResult
hash_table_insert_or_replace(
    AxlHashTable *h,
    const void   *key,
    void         *value,
    bool          keep_old_key
    )
{
    size_t     idx;
    hash_node *node;

    if (h == NULL || key == NULL) {
        return AXL_HASH_TABLE_ERR;
    }

    idx = h->hash_func(key) % h->bucket_count;

    for (node = h->buckets[idx]; node != NULL; node = node->next) {
        if (h->equal_func(node->key, key)) {
            // Replace existing entry
            if (h->copy_keys) {
                // Keys are internal copies — nothing to do with caller's key
            } else if (keep_old_key) {
                // insert: keep old key, destroy new key
                if (h->key_destroy != NULL && node->key != (void *)key) {
                    h->key_destroy((void *)key);
                }
            } else {
                // replace: keep new key, destroy old key
                if (h->key_destroy != NULL && node->key != (void *)key) {
                    h->key_destroy(node->key);
                }
                node->key = (void *)key;
            }
            if (h->value_destroy != NULL && node->value != value) {
                h->value_destroy(node->value);
            }
            node->value = value;
            return AXL_HASH_TABLE_REPLACED;
        }
    }

    // New entry — check resize
    if (h->entry_count * LOAD_FACTOR_DEN >= h->bucket_count * LOAD_FACTOR_NUM) {
        resize(h);
        idx = h->hash_func(key) % h->bucket_count;
    }

    node = axl_malloc(sizeof(hash_node));
    if (node == NULL) {
        axl_error("failed to allocate hash node");
        return AXL_HASH_TABLE_ERR;
    }

    if (h->copy_keys) {
        node->key = copy_str_key(key);
        if (node->key == NULL) {
            axl_error("failed to allocate key copy");
            axl_free(node);
            return AXL_HASH_TABLE_ERR;
        }
    } else {
        node->key = (void *)key;
    }

    node->value = value;
    node->next = h->buckets[idx];
    h->buckets[idx] = node;
    h->entry_count++;

    return AXL_HASH_TABLE_NEW;
}

AxlHashTableInsertResult
axl_hash_table_insert(
    AxlHashTable *h,
    const void   *key,
    void         *value
    )
{
    return hash_table_insert_or_replace(h, key, value, true);
}

AxlHashTableInsertResult
axl_hash_table_replace(
    AxlHashTable *h,
    const void   *key,
    void         *value
    )
{
    return hash_table_insert_or_replace(h, key, value, false);
}

void *
axl_hash_table_lookup(
    AxlHashTable *h,
    const void   *key
    )
{
    size_t     idx;
    hash_node *node;

    if (h == NULL || key == NULL) {
        return NULL;
    }

    idx = h->hash_func(key) % h->bucket_count;

    for (node = h->buckets[idx]; node != NULL; node = node->next) {
        if (h->equal_func(node->key, key)) {
            return node->value;
        }
    }

    return NULL;
}

bool
axl_hash_table_contains(
    AxlHashTable *h,
    const void   *key
    )
{
    size_t     idx;
    hash_node *node;

    if (h == NULL || key == NULL) {
        return false;
    }

    idx = h->hash_func(key) % h->bucket_count;

    for (node = h->buckets[idx]; node != NULL; node = node->next) {
        if (h->equal_func(node->key, key)) {
            return true;
        }
    }

    return false;
}

bool
axl_hash_table_remove(
    AxlHashTable *h,
    const void   *key
    )
{
    size_t      idx;
    hash_node **prev;
    hash_node  *node;

    if (h == NULL || key == NULL) {
        return false;
    }

    idx = h->hash_func(key) % h->bucket_count;
    prev = &h->buckets[idx];

    for (node = *prev; node != NULL; prev = &node->next, node = node->next) {
        if (h->equal_func(node->key, key)) {
            *prev = node->next;
            free_key(h, node->key);
            free_value(h, node->value);
            axl_free(node);
            h->entry_count--;
            return true;
        }
    }

    return false;
}

bool
axl_hash_table_steal(
    AxlHashTable *h,
    const void   *key
    )
{
    size_t      idx;
    hash_node **prev;
    hash_node  *node;

    if (h == NULL || key == NULL) {
        return false;
    }

    idx = h->hash_func(key) % h->bucket_count;
    prev = &h->buckets[idx];

    for (node = *prev; node != NULL; prev = &node->next, node = node->next) {
        if (h->equal_func(node->key, key)) {
            *prev = node->next;
            // For copy_keys tables, the caller never had the internal
            // key copy, so we must free it to avoid a leak.
            if (h->copy_keys) {
                axl_free(node->key);
            }
            // value_destroy is NOT called — caller takes ownership
            axl_free(node);
            h->entry_count--;
            return true;
        }
    }

    return false;
}

// ===========================================================================
// Free and size
// ===========================================================================

void
axl_hash_table_free(AxlHashTable *h)
{
    hash_node *node;
    hash_node *next;

    if (h == NULL) {
        return;
    }

    for (size_t i = 0; i < h->bucket_count; i++) {
        node = h->buckets[i];
        while (node != NULL) {
            next = node->next;
            free_key(h, node->key);
            free_value(h, node->value);
            axl_free(node);
            node = next;
        }
    }

    axl_free(h->buckets);
    axl_free(h);
}

size_t
axl_hash_table_size(AxlHashTable *h)
{
    if (h == NULL) {
        return 0;
    }

    return h->entry_count;
}

bool
axl_hash_table_owns_entries(AxlHashTable *h)
{
    if (h == NULL) {
        return false;
    }
    return !h->copy_keys
        && h->key_destroy   != NULL
        && h->value_destroy != NULL;
}

// ===========================================================================
// Iteration
// ===========================================================================

void
axl_hash_table_foreach(
    AxlHashTable            *h,
    AxlHashTableForeachFunc  func,
    void                    *data
    )
{
    hash_node *node;

    if (h == NULL || func == NULL) {
        return;
    }

    for (size_t i = 0; i < h->bucket_count; i++) {
        for (node = h->buckets[i]; node != NULL; node = node->next) {
            func(node->key, node->value, data);
        }
    }
}

size_t
axl_hash_table_foreach_remove(
    AxlHashTable                  *h,
    AxlHashTableForeachRemoveFunc  func,
    void                          *data
    )
{
    hash_node **prev;
    hash_node  *node;
    hash_node  *next;
    size_t      removed = 0;

    if (h == NULL || func == NULL) {
        return 0;
    }

    for (size_t i = 0; i < h->bucket_count; i++) {
        prev = &h->buckets[i];
        node = *prev;

        while (node != NULL) {
            next = node->next;

            if (func(node->key, node->value, data)) {
                *prev = next;
                free_key(h, node->key);
                free_value(h, node->value);
                axl_free(node);
                h->entry_count--;
                removed++;
            } else {
                prev = &node->next;
            }

            node = next;
        }
    }

    return removed;
}

// ===========================================================================
// Iterator
// ===========================================================================

/// Find the next non-empty bucket starting from start_bucket.
static hash_node *
iter_find_next(struct AxlHashTable *h, size_t start_bucket, size_t *out_bucket)
{
    for (size_t i = start_bucket; i < h->bucket_count; i++) {
        if (h->buckets[i] != NULL) {
            *out_bucket = i;
            return h->buckets[i];
        }
    }
    return NULL;
}

void
axl_hash_table_iter_init(
    AxlHashTableIter *iter,
    AxlHashTable     *h
    )
{
    iter->table = h;
    iter->_current = NULL;
    iter->_cur_bucket = 0;

    if (h != NULL) {
        iter->_next = iter_find_next(h, 0, &iter->_bucket);
    } else {
        iter->_next = NULL;
        iter->_bucket = 0;
    }
}

bool
axl_hash_table_iter_next(
    AxlHashTableIter *iter,
    void            **key,
    void            **value
    )
{
    hash_node *node = (hash_node *)iter->_next;

    if (node == NULL) {
        iter->_current = NULL;
        return false;
    }

    // This node becomes the "current" (removable) entry
    iter->_current = node;
    iter->_cur_bucket = iter->_bucket;

    if (key != NULL) {
        *key = node->key;
    }
    if (value != NULL) {
        *value = node->value;
    }

    // Advance _next: try chain first, then scan buckets
    if (node->next != NULL) {
        iter->_next = node->next;
    } else {
        iter->_next = iter_find_next(iter->table, iter->_bucket + 1,
                                     &iter->_bucket);
    }

    return true;
}

void
axl_hash_table_iter_remove(
    AxlHashTableIter *iter
    )
{
    hash_node *node = (hash_node *)iter->_current;
    if (node == NULL) {
        return;
    }

    unlink_node(iter->table, iter->_cur_bucket, node);
    free_key(iter->table, node->key);
    free_value(iter->table, node->value);
    axl_free(node);
    iter->_current = NULL;
}

void
axl_hash_table_iter_steal(
    AxlHashTableIter *iter
    )
{
    hash_node *node = (hash_node *)iter->_current;
    if (node == NULL) {
        return;
    }

    unlink_node(iter->table, iter->_cur_bucket, node);
    if (iter->table->copy_keys) {
        axl_free(node->key);
    }
    axl_free(node);
    iter->_current = NULL;
}
