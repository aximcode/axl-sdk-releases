/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-class.c
    Optional class-name overlay loader.

    The PCI class triplet (base, sub, prog) decoders in axl-pci.c
    use compiled-in tables for the bootstrap default. This file adds
    a JSON5 sidecar that overlays per-tier names — when a new class
    triplet ships (e.g. CXL Memory Expanders), field engineers can
    fix names with a `git pull` of the JSON5 sheet rather than
    rebuilding every consumer.

    Layered like axl-pci-ids.c: opaque AxlPciClassDb handle, parallel
    open / open_from_buffer / close, plus a process-global singleton
    consulted automatically by axl_pci_class_string_fmt.

    Lookup order in axl_pci_class_string_fmt's lookup_* helpers:
      1. Overlay singleton (this file)
      2. Compiled-in table (axl-pci.c)

    Builds on `axl-sidecar` for the file-open / schema-check /
    singleton-lifecycle plumbing — this file owns only the
    schema-specific typed walk into the three flat hash tables.
**/

#include "../data/axl-sidecar-internal.h"
#include "axl-pci-class-internal.h"

#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-pci.h>
#include <axl/axl-sidecar.h>

AXL_LOG_DOMAIN("pci-class");

// ---------------------------------------------------------------------------
// Handle layout
// ---------------------------------------------------------------------------

struct AxlPciClassDb {
    AxlHashTable *bases;     /* uintptr_t(base)                     → char* */
    AxlHashTable *subs;      /* uintptr_t(base<<8 | sub)            → char* */
    AxlHashTable *progs;     /* uintptr_t(base<<16 | sub<<8 | prog) → char* */
};

static inline void *
base_key(
    uint8_t  base
    )
{
    /* Add a high bit so 0x00 doesn't collide with NULL — base 0x00
       is the legitimate "Unclassified" tier. */
    return (void *)((uintptr_t)0x100u | base);
}

static inline void *
sub_key(
    uint8_t  base,
    uint8_t  sub
    )
{
    return (void *)((uintptr_t)0x10000u
                  | ((uintptr_t)base << 8)
                  | (uintptr_t)sub);
}

static inline void *
prog_key(
    uint8_t  base,
    uint8_t  sub,
    uint8_t  prog
    )
{
    return (void *)((uintptr_t)0x1000000u
                  | ((uintptr_t)base << 16)
                  | ((uintptr_t)sub  << 8)
                  | (uintptr_t)prog);
}

// ---------------------------------------------------------------------------
// Schema-specific typed walk
// ---------------------------------------------------------------------------

/* Insert one (base,sub,prog,name) tuple into the right table per
   the routing below. Shared by both schema parsers — schema 1 calls
   it once per flat row, schema 2 calls it once per (base,sub,prog)
   slot reached during the hierarchical walk.

   Entry routing:
     base only         → bases table
     base + sub        → subs table
     base + sub + prog → progs table
   prog without sub is meaningless — the caller must ensure sub is
   present whenever prog is. */
static void
class_insert(
    AxlPciClassDb  *db,
    uint8_t         base,
    bool            has_sub,
    uint8_t         sub,
    bool            has_prog,
    uint8_t         prog,
    const char     *name
    )
{
    char *name_owned = axl_strdup(name);
    if (name_owned == NULL) {
        return;
    }
    AxlHashTable *target;
    void         *key;
    if (has_prog) {
        target = db->progs;
        key    = prog_key(base, sub, prog);
    } else if (has_sub) {
        target = db->subs;
        key    = sub_key(base, sub);
    } else {
        target = db->bases;
        key    = base_key(base);
    }
    if (axl_hash_table_insert(target, key, name_owned) == AXL_HASH_TABLE_ERR) {
        axl_free(name_owned);
    }
}

/* Schema 1 — flat: each entry pins any subset of (base, sub, prog).
   Missing 'base' or 'name' rejects the entry. */
static void
parse_classes_v1(
    AxlJsonReader  *r,
    AxlPciClassDb  *db
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "classes", &it)) {
        return;  /* empty stub is valid */
    }
    AxlJsonReader entry;
    while (axl_json_array_next(&it, &entry)) {
        uint64_t base64 = 0;
        char     name[AXL_PCI_CLASS_NAME_MAX] = "";
        if (!axl_json_get_uint(&entry, "base", &base64)
            || base64 > 0xFFu
            || !axl_json_get_string(&entry, "name", name, sizeof(name)))
        {
            continue;
        }
        uint64_t sub64 = 0;
        uint64_t prog64 = 0;
        bool has_sub  = axl_json_get_uint(&entry, "sub",  &sub64)
                        && sub64  <= 0xFFu;
        bool has_prog = axl_json_get_uint(&entry, "prog", &prog64)
                        && prog64 <= 0xFFu;
        if (has_prog && !has_sub) {
            continue;  /* prog without sub is meaningless */
        }
        class_insert(db, (uint8_t)base64,
                     has_sub,  (uint8_t)sub64,
                     has_prog, (uint8_t)prog64, name);
    }
}

/* Schema 2 — hierarchical: subclasses nest under bases, progs nest
   under subclasses. Locality matches pci-ids schema 2; lookups are
   global on the same composite keys regardless of file shape. */
static void
parse_classes_v2(
    AxlJsonReader  *r,
    AxlPciClassDb  *db
    )
{
    AxlJsonArrayIter b_it;
    if (!axl_json_array_begin(r, "classes", &b_it)) {
        return;  /* empty stub is valid */
    }
    AxlJsonReader b_entry;
    while (axl_json_array_next(&b_it, &b_entry)) {
        uint64_t base64 = 0;
        char     bname[AXL_PCI_CLASS_NAME_MAX] = "";
        if (!axl_json_get_uint(&b_entry, "base", &base64)
            || base64 > 0xFFu)
        {
            continue;
        }
        uint8_t base = (uint8_t)base64;
        /* Base-tier 'name' is optional in v2 — a base entry can exist
           purely as a parent node for nested subclasses. */
        if (axl_json_get_string(&b_entry, "name", bname, sizeof(bname))) {
            class_insert(db, base, false, 0, false, 0, bname);
        }
        AxlJsonArrayIter s_it;
        if (!axl_json_array_begin(&b_entry, "subclasses", &s_it)) {
            continue;
        }
        AxlJsonReader s_entry;
        while (axl_json_array_next(&s_it, &s_entry)) {
            uint64_t sub64 = 0;
            char     sname[AXL_PCI_CLASS_NAME_MAX] = "";
            if (!axl_json_get_uint(&s_entry, "sub", &sub64)
                || sub64 > 0xFFu)
            {
                continue;
            }
            uint8_t sub = (uint8_t)sub64;
            if (axl_json_get_string(&s_entry, "name",
                                    sname, sizeof(sname)))
            {
                class_insert(db, base, true, sub, false, 0, sname);
            }
            AxlJsonArrayIter p_it;
            if (!axl_json_array_begin(&s_entry, "progs", &p_it)) {
                continue;
            }
            AxlJsonReader p_entry;
            while (axl_json_array_next(&p_it, &p_entry)) {
                uint64_t prog64 = 0;
                char     pname[AXL_PCI_CLASS_NAME_MAX] = "";
                if (!axl_json_get_uint(&p_entry, "prog", &prog64)
                    || prog64 > 0xFFu
                    || !axl_json_get_string(&p_entry, "name",
                                            pname, sizeof(pname)))
                {
                    continue;
                }
                class_insert(db, base, true, sub, true,
                             (uint8_t)prog64, pname);
            }
        }
    }
}

static AxlSidecarStatus
db_fill(
    AxlPciClassDb  *db,
    AxlJsonReader  *r
    )
{
    static const uint64_t accepted[] = { 1u, 2u };
    uint64_t schema = 0;
    AxlSidecarStatus rc = axl_sidecar_check_schema(
        r, "pci-class", accepted, sizeof(accepted) / sizeof(accepted[0]),
        &schema);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }
    if (schema == 1) {
        parse_classes_v1(r, db);
    } else /* schema == 2 */ {
        parse_classes_v2(r, db);
    }
    return AXL_SIDECAR_OK;
}

// ---------------------------------------------------------------------------
// Handle alloc / free
// ---------------------------------------------------------------------------

static AxlPciClassDb *
db_alloc(
    void
    )
{
    AxlPciClassDb *db = axl_malloc(sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
    db->bases = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    db->subs  = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    db->progs = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    if (db->bases == NULL || db->subs == NULL || db->progs == NULL) {
        axl_pci_class_close(db);
        return NULL;
    }
    return db;
}

// ---------------------------------------------------------------------------
// Handle API
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_pci_class_open(
    const char       *path,
    AxlPciClassDb   **out
    )
{
    if (out == NULL || path == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    *out = NULL;

    AxlJsonReader r   = { 0 };
    void         *raw = NULL;
    AxlSidecarStatus rc = axl_sidecar_open_file(path, &r, &raw);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }
    AxlPciClassDb *db = db_alloc();
    if (db == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = db_fill(db, &r);
    axl_json_free(&r);
    axl_free(raw);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_pci_class_close(db);
        return fill_rc;
    }

    axl_debug("pci-class: %zu base / %zu sub / %zu prog overrides from %s",
              axl_hash_table_size(db->bases),
              axl_hash_table_size(db->subs),
              axl_hash_table_size(db->progs),
              path);
    *out = db;
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_pci_class_open_from_buffer(
    const char       *json5,
    size_t            len,
    AxlPciClassDb   **out
    )
{
    if (out == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    *out = NULL;

    AxlJsonReader r = { 0 };
    AxlSidecarStatus rc = axl_sidecar_open_buffer(json5, len, &r);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }
    AxlPciClassDb *db = db_alloc();
    if (db == NULL) {
        axl_json_free(&r);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = db_fill(db, &r);
    axl_json_free(&r);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_pci_class_close(db);
        return fill_rc;
    }
    *out = db;
    return AXL_SIDECAR_OK;
}

void
axl_pci_class_close(
    AxlPciClassDb  *db
    )
{
    if (db == NULL) {
        return;
    }
    axl_hash_table_free(db->bases);
    axl_hash_table_free(db->subs);
    axl_hash_table_free(db->progs);
    axl_free(db);
}

const char *
axl_pci_class_db_base_name(
    const AxlPciClassDb  *db,
    uint8_t               base
    )
{
    if (db == NULL) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(db->bases, base_key(base));
}

const char *
axl_pci_class_db_sub_name(
    const AxlPciClassDb  *db,
    uint8_t               base,
    uint8_t               sub
    )
{
    if (db == NULL) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(db->subs, sub_key(base, sub));
}

const char *
axl_pci_class_db_prog_name(
    const AxlPciClassDb  *db,
    uint8_t               base,
    uint8_t               sub,
    uint8_t               prog
    )
{
    if (db == NULL) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(db->progs,
                                               prog_key(base, sub, prog));
}

// ---------------------------------------------------------------------------
// Process-global singleton (driven by the shared sidecar machinery)
// ---------------------------------------------------------------------------

static void     *g_class_singleton;        /* AxlPciClassDb * cast */
static uint32_t  g_class_atexit_handle;
static void     *g_class_atexit_ctx;       /* opaque, owned by sidecar helper */

static AxlSidecarStatus
class_open_thunk(const char *path, void **out)
{
    return axl_pci_class_open(path, (AxlPciClassDb **)out);
}

static void
class_close_thunk(void *handle)
{
    axl_pci_class_close((AxlPciClassDb *)handle);
}

AxlSidecarStatus
axl_pci_class_load(
    const char  *override_path
    )
{
    /* Autodiscovery looks for share/pci-ids.json5 — the same file
       axl_pci_ids_load consumes. The schema-2 layout carries both
       vendors[] (read by ids_load) and classes[] (read here); this
       loader simply ignores the vendors[] block. Override path is
       honored verbatim. */
    return _axl_sidecar_singleton_load(
        &g_class_singleton, &g_class_atexit_handle, &g_class_atexit_ctx,
        override_path, "pci-ids.json5",
        class_open_thunk, class_close_thunk);
}

void
axl_pci_class_free(
    void
    )
{
    _axl_sidecar_singleton_free(
        &g_class_singleton, &g_class_atexit_handle, &g_class_atexit_ctx,
        class_close_thunk);
}

// ---------------------------------------------------------------------------
// Internal accessors used by axl-pci.c lookup_* helpers.
// Walk the singleton overlay; return NULL if no overlay loaded or
// the overlay has no entry for this code. Caller falls back to
// compiled-in tables.
// ---------------------------------------------------------------------------

const char *
_axl_pci_class_overlay_base(
    uint8_t  base
    )
{
    return axl_pci_class_db_base_name(
        (AxlPciClassDb *)g_class_singleton, base);
}

const char *
_axl_pci_class_overlay_sub(
    uint8_t  base,
    uint8_t  sub
    )
{
    return axl_pci_class_db_sub_name(
        (AxlPciClassDb *)g_class_singleton, base, sub);
}

const char *
_axl_pci_class_overlay_prog(
    uint8_t  base,
    uint8_t  sub,
    uint8_t  prog
    )
{
    return axl_pci_class_db_prog_name(
        (AxlPciClassDb *)g_class_singleton, base, sub, prog);
}
