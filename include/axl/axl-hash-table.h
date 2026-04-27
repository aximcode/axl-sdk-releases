/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-hash-table.h:
 *
 * GLib-style hash table with generic keys. FNV-1a hashing, chained
 * collision resolution, automatic resize at 75% load factor.
 *
 * API mirrors GLib's GHashTable (g_hash_table_*). See GLib
 * documentation for detailed usage patterns. Key differences:
 * - axl_hash_table_new_str() is an AXL convenience (copies string keys)
 * - Insert/replace return a tri-state int (1=new, 0=replaced, -1=OOM)
 *   instead of GLib's bool, since GLib aborts on OOM and AXL recovers.
 * - No reference counting (use axl_hash_table_free, not unref)
 *
 * # Ownership of keys and values
 *
 * All three constructors allocate the AxlHashTable struct itself.
 * They differ in how the table treats the key and value pointers
 * passed to insert/replace:
 *
 *   axl_hash_table_new_str()
 *       Keys are COPIED internally via strdup; the table owns the
 *       copy and frees it on remove/free. Values are borrowed —
 *       caller manages their lifetime.
 *
 *   axl_hash_table_new(hash, equal)
 *       Keys and values are BORROWED. Caller manages all lifetimes.
 *       The table never copies or frees either.
 *
 *   axl_hash_table_new_full(hash, equal, key_destroy, value_destroy)
 *       Keys are NOT copied. The table TAKES OWNERSHIP if the
 *       corresponding destroy callback is non-NULL — on remove/free
 *       it calls the destroy callback on the pointer it stored.
 *       If a destroy callback is NULL, that side is treated as
 *       borrowed.
 *
 * Insert vs. replace differ on key collision when ownership is in
 * play (see axl_hash_table_insert / axl_hash_table_replace docs):
 *   - insert: keep the OLD key, destroy the NEW key
 *   - replace: destroy the OLD key, keep the NEW key
 * Both destroy the old value either way.
 */

#ifndef AXL_HASH_TABLE_H
#define AXL_HASH_TABLE_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlHashTable AxlHashTable;

// ---------------------------------------------------------------------------
// Callback types (match GLib: GHashFunc, GEqualFunc)
// ---------------------------------------------------------------------------

/// Hash function: compute a hash value from a key.
typedef size_t (*AxlHashFunc)(const void *key);

/// Equality function: return true if two keys are equal.
typedef bool (*AxlEqualFunc)(const void *a, const void *b);

/// Callback for axl_hash_table_foreach (GLib: GHFunc).
typedef void (*AxlHashTableForeachFunc)(
    const void *key,   ///< entry key
    void       *value, ///< entry value
    void       *data   ///< opaque callback data
);

/// Predicate for axl_hash_table_foreach_remove (GLib: GHRFunc).
/// Return true to remove the entry.
typedef bool (*AxlHashTableForeachRemoveFunc)(
    const void *key,   ///< entry key
    void       *value, ///< entry value
    void       *data   ///< opaque callback data
);

// ---------------------------------------------------------------------------
// Built-in hash and equality functions
// ---------------------------------------------------------------------------

/* String-keyed hash/equal helpers live in axl-str.h (they're string
 * utilities, used here as hash-table callbacks via the void*-typed
 * AxlHashFunc / AxlEqualFunc signatures):
 *
 *   size_t axl_str_hash(const void *key);
 *   bool   axl_str_equal(const void *a, const void *b);
 *
 * Include <axl/axl-str.h> — or the <axl.h> umbrella — when
 * constructing a string-keyed AxlHashTable. */

/// Hash a pointer value directly. (GLib: g_direct_hash)
size_t axl_direct_hash(const void *key);

/// Pointer equality. (GLib: g_direct_equal)
bool axl_direct_equal(const void *a, const void *b);

// ---------------------------------------------------------------------------
// Construction and destruction
// ---------------------------------------------------------------------------

/**
 * @brief Create a string-keyed hash table that COPIES its keys.
 *
 * Allocates the AxlHashTable struct. Each call to insert/replace
 * also strdup's the key into a heap-allocated copy that the table
 * owns; that copy is freed on remove and on axl_hash_table_free.
 * Values are borrowed — caller retains ownership and lifetime
 * responsibility.
 *
 * AXL extension; no GLib equivalent. Use this when keys are
 * borrowed or literal strings and you want zero ceremony around
 * key lifetime.
 *
 * @return new AxlHashTable, or NULL on allocation failure.
 */
AxlHashTable *
axl_hash_table_new_str(void);

/**
 * @brief Create a hash table that BORROWS keys and values.
 *
 * Allocates the AxlHashTable struct. Keys and values are stored
 * as raw pointers — the table never copies and never frees either
 * side. Caller manages all lifetimes.
 *
 * Matches g_hash_table_new().
 *
 * @return new AxlHashTable, or NULL on allocation failure.
 */
AxlHashTable *
axl_hash_table_new(
    AxlHashFunc  hash_func,   ///< hash function
    AxlEqualFunc equal_func   ///< equality function
);

/**
 * @brief Create a hash table that TAKES OWNERSHIP of keys/values.
 *
 * Allocates the AxlHashTable struct. Keys and values are stored
 * by pointer (NOT copied); the table calls @p key_destroy /
 * @p value_destroy on remove/free for any side whose destroy
 * callback is non-NULL. Pass NULL for a side to leave it
 * borrowed (caller retains ownership of that side).
 *
 * Pass NULL for @p hash_func and @p equal_func to default to
 * axl_str_hash / axl_str_equal.
 *
 * Matches g_hash_table_new_full().
 *
 * @return new AxlHashTable, or NULL on allocation failure.
 */
AxlHashTable *
axl_hash_table_new_full(
    AxlHashFunc      hash_func,    ///< hash function, or NULL for axl_str_hash
    AxlEqualFunc     equal_func,   ///< equality function, or NULL for axl_str_equal
    AxlDestroyNotify key_destroy,  ///< key destructor, or NULL to borrow keys
    AxlDestroyNotify value_destroy ///< value destructor, or NULL to borrow values
);

/**
 * @brief Free a hash table and all entries.
 *
 * Calls key_destroy and value_destroy on each entry (if set).
 * Equivalent to g_hash_table_destroy().
 */
void
axl_hash_table_free(
    AxlHashTable *h  ///< hash table (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlHashTable, axl_hash_table_free)
#endif

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

/**
 * @brief Insert a key-value pair, keeping the OLD key on collision.
 *
 * If the key already exists: the new key is freed via key_destroy
 * (if set), the old key is kept, and the old value is freed via
 * value_destroy (if set). Matches g_hash_table_insert().
 *
 * For tables created with axl_hash_table_new_str() (copy_keys mode),
 * insert and replace behave identically.
 *
 * @return 1 if a new entry was added, 0 if an existing entry was
 *         replaced, -1 on allocation failure (also logged).
 */
int
axl_hash_table_insert(
    AxlHashTable *h,     ///< hash table
    const void   *key,   ///< key (copied for new() tables, owned for new_full())
    void         *value  ///< value pointer
);

/**
 * @brief Insert a key-value pair, keeping the NEW key on collision.
 *
 * If the key already exists: the old key is freed via key_destroy
 * (if set), the new key is kept, and the old value is freed via
 * value_destroy (if set). Matches g_hash_table_replace().
 *
 * For tables created with axl_hash_table_new_str() (copy_keys mode),
 * insert and replace behave identically.
 *
 * @return 1 if a new entry was added, 0 if an existing entry was
 *         replaced, -1 on allocation failure (also logged).
 */
int
axl_hash_table_replace(
    AxlHashTable *h,     ///< hash table
    const void   *key,   ///< key (copied for new() tables, owned for new_full())
    void         *value  ///< value pointer
);

/**
 * @brief Look up a key.
 *
 * Matches g_hash_table_lookup().
 *
 * @return value pointer, or NULL if not found.
 */
void *
axl_hash_table_lookup(
    AxlHashTable *h,    ///< hash table
    const void   *key   ///< key to look up
);

/**
 * @brief Check if a key exists in the table.
 *
 * Unlike lookup, this distinguishes between a key with a NULL value
 * and a missing key. Matches g_hash_table_contains().
 *
 * @return true if the key exists, false otherwise.
 */
bool
axl_hash_table_contains(
    AxlHashTable *h,    ///< hash table
    const void   *key   ///< key to check
);

/**
 * @brief Remove an entry, calling key_destroy and value_destroy.
 *
 * Matches g_hash_table_remove().
 *
 * @return true if removed, false if not found.
 */
bool
axl_hash_table_remove(
    AxlHashTable *h,    ///< hash table
    const void   *key   ///< key to remove
);

/**
 * @brief Remove an entry WITHOUT calling destructors.
 *
 * The caller takes ownership of the value. For copy_keys tables,
 * the internal key copy is freed (since the caller never had it).
 * Matches g_hash_table_steal().
 *
 * @return true if removed, false if not found.
 */
bool
axl_hash_table_steal(
    AxlHashTable *h,    ///< hash table
    const void   *key   ///< key to steal
);

/**
 * @brief Get the number of entries.
 *
 * Matches g_hash_table_size().
 *
 * @return entry count.
 */
size_t
axl_hash_table_size(
    AxlHashTable *h  ///< hash table
);

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

/**
 * @brief Iterate all entries. Order is undefined.
 *
 * Matches g_hash_table_foreach().
 */
void
axl_hash_table_foreach(
    AxlHashTable             *h,    ///< hash table
    AxlHashTableForeachFunc   func, ///< callback
    void                     *data  ///< opaque data passed to callback
);

/**
 * @brief Remove entries matching a predicate.
 *
 * Calls @p func for each entry. If it returns true, the entry is
 * removed (key_destroy and value_destroy are called).
 * Matches g_hash_table_foreach_remove().
 *
 * @return number of entries removed.
 */
size_t
axl_hash_table_foreach_remove(
    AxlHashTable                   *h,    ///< hash table
    AxlHashTableForeachRemoveFunc   func, ///< predicate
    void                           *data  ///< opaque data
);

// ---------------------------------------------------------------------------
// Iterator (safe removal during iteration)
// ---------------------------------------------------------------------------

/**
 * AxlHashTableIter:
 *
 * Stack-allocated iterator. Matches GHashTableIter.
 * Fields prefixed with _ are private.
 */
typedef struct {
    AxlHashTable *table;
    size_t        _bucket;
    size_t        _cur_bucket;
    void         *_current;
    void         *_next;
} AxlHashTableIter;

/**
 * @brief Initialize an iterator over a hash table.
 *
 * Matches g_hash_table_iter_init().
 */
void
axl_hash_table_iter_init(
    AxlHashTableIter *iter,  ///< iterator to initialize
    AxlHashTable     *h      ///< hash table to iterate
);

/**
 * @brief Advance to the next entry.
 *
 * @p key and @p value are optional (pass NULL to ignore).
 * Matches g_hash_table_iter_next().
 *
 * @return true if an entry was returned, false if exhausted.
 */
bool
axl_hash_table_iter_next(
    AxlHashTableIter *iter,   ///< iterator
    void            **key,    ///< receives key pointer, or NULL
    void            **value   ///< receives value pointer, or NULL
);

/**
 * @brief Remove the current entry, calling destructors.
 *
 * Must be called after a successful axl_hash_table_iter_next().
 * Matches g_hash_table_iter_remove().
 */
void
axl_hash_table_iter_remove(
    AxlHashTableIter *iter  ///< iterator
);

/**
 * @brief Remove the current entry WITHOUT calling destructors.
 *
 * Must be called after a successful axl_hash_table_iter_next().
 * Matches g_hash_table_iter_steal().
 */
void
axl_hash_table_iter_steal(
    AxlHashTableIter *iter  ///< iterator
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HASH_TABLE_H */
