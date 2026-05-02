/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-ids.c
    PCI vendor / device / subsystem name database loader.

    Handle-based: AxlPciIds * holds three hash tables (vid → name,
    (vid,did) → name, (svid,sdid) → name) parsed from a JSON5 file
    or buffer. Multiple handles can coexist — a public+private overlay
    loads both an upstream database and an internal/OEM one and queries
    them in priority order. The process-global singleton
    (axl_pci_ids_load and friends) is a thin shim around a single
    internal handle.

    Intentionally separate from axl-pci.c so config-space access
    doesn't drag in the JSON parser.
**/

#include <axl/axl-pci.h>
#include <axl/axl-fs.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-path.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("pci-ids");

// ---------------------------------------------------------------------------
// Handle layout
// ---------------------------------------------------------------------------

struct AxlPciIds {
    AxlHashTable *vendors;     /* uintptr_t(vid)            → char* */
    AxlHashTable *devices;     /* uintptr_t(vid<<16 | did)  → char* */
    AxlHashTable *subsystems;  /* uintptr_t(svid<<16 | sdid) → char* */
};

/* Pack two 16-bit IDs into a single hash-table key. axl_direct_hash
   is bijective on the pointer bits, so distinct (a, b) pairs never
   collide. Keys are non-NULL because the loader rejects any entry
   whose primary ID is zero (axl_hash_table_lookup rejects NULL keys). */
static inline void *
pair_key(
    uint16_t  a,
    uint16_t  b
    )
{
    return (void *)((uintptr_t)((uint32_t)a << 16) | (uintptr_t)b);
}

// ---------------------------------------------------------------------------
// JSON5 parser → hash tables
//
// Schema dispatch:
//
//   schema: 1   Flat layout — three top-level arrays vendors[],
//               devices[], subsystems[], each entry self-contained
//               (devices carry their own vid; subsystems their own
//               svid). Cheap to parse, easy to generate from the
//               canonical pci.ids 'S' lines, awkward to hand-edit
//               at scale.
//
//   schema: 2   Hierarchical layout — top-level vendors[], with
//               nested devices[] under each vendor and nested
//               subsystems[] under each device. The natural
//               hand-edit shape for thousands of entries (locality
//               of related rows; no repeated vid: field per device).
//               Lookups are still flat globally — the loader walks
//               the nesting and inserts into the same three hash
//               tables a v1 file would populate.
//
// New schema numbers should bump the dispatch in ids_fill — old
// loaders return -2 on unrecognized schemas rather than silently
// misparsing.
// ---------------------------------------------------------------------------

/* Common helper: copy @p src into the table at @p key, freeing on
   insert failure. axl_strdup NULL is silently dropped (alloc
   failure is best-effort recoverable in this loader). */
static void
ids_insert_string(
    AxlHashTable  *t,
    void          *key,
    const char    *src
    )
{
    char *owned = axl_strdup(src);
    if (owned == NULL) {
        return;
    }
    if (axl_hash_table_insert(t, key, owned) < 0) {
        axl_free(owned);
    }
}

static bool
parse_vendors(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "vendors", &it)) {
        return true;  /* a database may legitimately ship without vendors */
    }
    AxlJsonReader entry;
    while (axl_json_array_next(&it, &entry)) {
        uint64_t id64 = 0;
        char     name[AXL_PCI_VENDOR_NAME_MAX] = "";
        if (!axl_json_get_uint(&entry, "id", &id64)
            || id64 == 0 || id64 > 0xFFFFu
            || !axl_json_get_string(&entry, "name", name, sizeof(name)))
        {
            continue;
        }
        char *name_owned = axl_strdup(name);
        if (name_owned == NULL) {
            continue;
        }
        if (axl_hash_table_insert(out,
                                  (void *)(uintptr_t)id64,
                                  name_owned) < 0) {
            axl_free(name_owned);
        }
    }
    return true;
}

static bool
parse_devices(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "devices", &it)) {
        return true;
    }
    AxlJsonReader entry;
    while (axl_json_array_next(&it, &entry)) {
        uint64_t vid64 = 0;
        uint64_t did64 = 0;
        char     name[AXL_PCI_DEVICE_NAME_MAX] = "";
        if (!axl_json_get_uint(&entry, "vid", &vid64)
            || vid64 == 0 || vid64 > 0xFFFFu
            || !axl_json_get_uint(&entry, "did", &did64)
            || did64 > 0xFFFFu
            || !axl_json_get_string(&entry, "name", name, sizeof(name)))
        {
            continue;
        }
        char *name_owned = axl_strdup(name);
        if (name_owned == NULL) {
            continue;
        }
        if (axl_hash_table_insert(out,
                                  pair_key((uint16_t)vid64, (uint16_t)did64),
                                  name_owned) < 0) {
            axl_free(name_owned);
        }
    }
    return true;
}

static bool
parse_subsystems(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "subsystems", &it)) {
        return true;  /* subsystems array is optional */
    }
    AxlJsonReader entry;
    while (axl_json_array_next(&it, &entry)) {
        uint64_t svid64 = 0;
        uint64_t sdid64 = 0;
        char     name[AXL_PCI_SUBSYS_NAME_MAX] = "";
        if (!axl_json_get_uint(&entry, "svid", &svid64)
            || svid64 == 0 || svid64 > 0xFFFFu
            || !axl_json_get_uint(&entry, "sdid", &sdid64)
            || sdid64 > 0xFFFFu
            || !axl_json_get_string(&entry, "name", name, sizeof(name)))
        {
            continue;
        }
        char *name_owned = axl_strdup(name);
        if (name_owned == NULL) {
            continue;
        }
        if (axl_hash_table_insert(out,
                                  pair_key((uint16_t)svid64, (uint16_t)sdid64),
                                  name_owned) < 0) {
            axl_free(name_owned);
        }
    }
    return true;
}

/* Allocate a handle with all three empty tables. Returns NULL on
   any allocation failure (caller does not have to free a partial
   handle). */
static AxlPciIds *
ids_alloc(
    void
    )
{
    AxlPciIds *ids = axl_malloc(sizeof(*ids));
    if (ids == NULL) {
        return NULL;
    }
    ids->vendors    = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    ids->devices    = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    ids->subsystems = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    if (ids->vendors == NULL
        || ids->devices == NULL
        || ids->subsystems == NULL)
    {
        axl_pci_ids_close(ids);
        return NULL;
    }
    return ids;
}

/* Hierarchical (schema 2) parser. Walks vendors[].devices[].subsystems[]
   and inserts into the same three flat hash tables a schema-1 file
   would populate. The nesting is purely organizational — lookups are
   global on the respective key. */
static void
parse_schema_v2(
    AxlJsonReader  *r,
    AxlPciIds      *ids
    )
{
    AxlJsonArrayIter v_it;
    if (!axl_json_array_begin(r, "vendors", &v_it)) {
        return;
    }
    AxlJsonReader v_entry;
    while (axl_json_array_next(&v_it, &v_entry)) {
        uint64_t vid64 = 0;
        char     vname[AXL_PCI_VENDOR_NAME_MAX] = "";
        if (!axl_json_get_uint(&v_entry, "id", &vid64)
            || vid64 == 0 || vid64 > 0xFFFFu)
        {
            continue;
        }
        /* Vendor 'name' is optional in v2 — a vendor entry can exist
           purely as a parent node for nested devices. */
        if (axl_json_get_string(&v_entry, "name", vname, sizeof(vname))) {
            ids_insert_string(ids->vendors,
                              (void *)(uintptr_t)vid64, vname);
        }
        AxlJsonArrayIter d_it;
        if (!axl_json_array_begin(&v_entry, "devices", &d_it)) {
            continue;
        }
        AxlJsonReader d_entry;
        while (axl_json_array_next(&d_it, &d_entry)) {
            uint64_t did64 = 0;
            char     dname[AXL_PCI_DEVICE_NAME_MAX] = "";
            if (!axl_json_get_uint(&d_entry, "did", &did64)
                || did64 > 0xFFFFu)
            {
                continue;
            }
            if (axl_json_get_string(&d_entry, "name",
                                    dname, sizeof(dname)))
            {
                ids_insert_string(ids->devices,
                                  pair_key((uint16_t)vid64,
                                           (uint16_t)did64),
                                  dname);
            }
            AxlJsonArrayIter s_it;
            if (!axl_json_array_begin(&d_entry, "subsystems", &s_it)) {
                continue;
            }
            AxlJsonReader s_entry;
            while (axl_json_array_next(&s_it, &s_entry)) {
                uint64_t svid64 = 0;
                uint64_t sdid64 = 0;
                char     sname[AXL_PCI_SUBSYS_NAME_MAX] = "";
                if (!axl_json_get_uint(&s_entry, "svid", &svid64)
                    || svid64 == 0 || svid64 > 0xFFFFu
                    || !axl_json_get_uint(&s_entry, "sdid", &sdid64)
                    || sdid64 > 0xFFFFu
                    || !axl_json_get_string(&s_entry, "name",
                                            sname, sizeof(sname)))
                {
                    continue;
                }
                ids_insert_string(ids->subsystems,
                                  pair_key((uint16_t)svid64,
                                           (uint16_t)sdid64),
                                  sname);
            }
        }
    }
}

/* Schema-aware fill. Returns 0 on success, -1 if the schema field
   is missing or unrecognized (caller maps to -2 = parse error). */
static int
ids_fill(
    AxlPciIds      *ids,
    AxlJsonReader  *r
    )
{
    /* `schema` field is REQUIRED. Defaulting to either version
       silently misparses files of the other version — a v2 file
       missing the declaration would parse as v1 with every nested
       device dropped (no error, no warning). Force explicit
       declaration; the error message tells the user exactly what
       to add. */
    uint64_t schema = 0;
    if (!axl_json_get_uint(r, "schema", &schema)) {
        axl_warning("pci-ids: 'schema' field missing — add "
                    "'schema: 2' (hierarchical, recommended) or "
                    "'schema: 1' (flat) to the file root");
        return -1;
    }

    if (schema == 1) {
        parse_vendors(r, ids->vendors);
        parse_devices(r, ids->devices);
        parse_subsystems(r, ids->subsystems);
        return 0;
    }
    if (schema == 2) {
        parse_schema_v2(r, ids);
        /* Schema 2 also accepts a top-level subsystems[] block
           alongside the nested form — useful for orphan entries the
           maintainer doesn't know which device to nest under. The
           nested + flat results merge into the same hash table. */
        parse_subsystems(r, ids->subsystems);
        return 0;
    }
    /* Unrecognized schema — old loader, new file. Better to fail
       loud than silently misparse a future schema 3 layout. */
    axl_warning("pci-ids: unrecognized schema %llu",
                (unsigned long long)schema);
    return -1;
}

// ---------------------------------------------------------------------------
// Handle API
// ---------------------------------------------------------------------------

int
axl_pci_ids_open(
    const char   *path,
    AxlPciIds   **out
    )
{
    if (out == NULL || path == NULL) {
        return -1;
    }
    *out = NULL;

    /* Distinguish "no file" (-1) from "parse error" (-2). The JSON
       loader returns false for both; check existence first so the
       error code reflects the actual failure mode. There's a TOCTOU
       window here (file exists at info-check, gone at load-time)
       but UEFI is single-threaded with no concurrent FS mutators
       so the window is theoretical. */
    AxlFileInfo finfo;
    if (axl_file_info(path, &finfo) != 0) {
        return -1;
    }

    AxlJsonReader r   = { 0 };
    void         *raw = NULL;
    if (!axl_json_load_file_flags(path, AXL_JSON_PARSER_JSON5,
                                  &r, &raw, NULL)) {
        axl_warning("pci-ids: failed to parse %s", path);
        return -2;
    }

    AxlPciIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return -2;
    }
    int fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    axl_free(raw);
    if (fill_rc != 0) {
        axl_pci_ids_close(ids);
        return -2;
    }

    axl_debug("pci-ids: %zu vendors / %zu devices / %zu subsystems from %s",
              axl_hash_table_size(ids->vendors),
              axl_hash_table_size(ids->devices),
              axl_hash_table_size(ids->subsystems),
              path);
    *out = ids;
    return 0;
}

int
axl_pci_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlPciIds   **out
    )
{
    if (out == NULL) {
        return -2;
    }
    *out = NULL;
    if (json5 == NULL || len == 0) {
        return -2;
    }

    AxlJsonReader r = { 0 };
    if (!axl_json_parse_flags(json5, len, AXL_JSON_PARSER_JSON5, &r)) {
        return -2;
    }
    AxlPciIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        return -2;
    }
    int fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    if (fill_rc != 0) {
        axl_pci_ids_close(ids);
        return -2;
    }
    *out = ids;
    return 0;
}

void
axl_pci_ids_close(
    AxlPciIds  *ids
    )
{
    if (ids == NULL) {
        return;
    }
    axl_hash_table_free(ids->vendors);
    axl_hash_table_free(ids->devices);
    axl_hash_table_free(ids->subsystems);
    axl_free(ids);
}

const char *
axl_pci_ids_vendor_name(
    const AxlPciIds  *ids,
    uint16_t          vid
    )
{
    if (ids == NULL || vid == 0) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(ids->vendors,
                                               (void *)(uintptr_t)vid);
}

const char *
axl_pci_ids_device_name(
    const AxlPciIds  *ids,
    uint16_t          vid,
    uint16_t          did
    )
{
    if (ids == NULL || vid == 0) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(ids->devices,
                                               pair_key(vid, did));
}

const char *
axl_pci_ids_subsys_name(
    const AxlPciIds  *ids,
    uint16_t          svid,
    uint16_t          sdid
    )
{
    if (ids == NULL || svid == 0) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(ids->subsystems,
                                               pair_key(svid, sdid));
}

// ---------------------------------------------------------------------------
// Iter API — debug dumps, validators, materialization
// ---------------------------------------------------------------------------

/* Trampoline pattern: axl_hash_table_foreach has no early-stop signal,
   so we wrap the user's callback and short-circuit the body once a
   non-zero return has been seen. The hash table walk continues to
   completion (every entry visited) but the user's fn is called only
   until the stop. For PCI databases (~hundreds of entries) the
   wasted hash iterations are negligible. */
typedef struct {
    AxlPciIdsVendorFn  fn;
    void              *ctx;
    int                stop_rc;
} VendorTrampoline;

static void
vendor_trampoline_cb(
    const void  *key,
    void        *value,
    void        *data
    )
{
    VendorTrampoline *t = data;
    if (t->stop_rc != 0) {
        return;
    }
    uint16_t vid = (uint16_t)(uintptr_t)key;
    int rc = t->fn(vid, (const char *)value, t->ctx);
    if (rc != 0) {
        t->stop_rc = rc;
    }
}

int
axl_pci_ids_foreach_vendor(
    const AxlPciIds   *ids,
    AxlPciIdsVendorFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return -1;
    }
    VendorTrampoline t = { .fn = fn, .ctx = ctx, .stop_rc = 0 };
    axl_hash_table_foreach(ids->vendors, vendor_trampoline_cb, &t);
    return t.stop_rc;
}

typedef struct {
    AxlPciIdsDeviceFn  fn;
    void              *ctx;
    int                stop_rc;
} DeviceTrampoline;

static void
device_trampoline_cb(
    const void  *key,
    void        *value,
    void        *data
    )
{
    DeviceTrampoline *t = data;
    if (t->stop_rc != 0) {
        return;
    }
    uintptr_t k = (uintptr_t)key;
    uint16_t  vid = (uint16_t)(k >> 16);
    uint16_t  did = (uint16_t)(k & 0xFFFFu);
    int rc = t->fn(vid, did, (const char *)value, t->ctx);
    if (rc != 0) {
        t->stop_rc = rc;
    }
}

int
axl_pci_ids_foreach_device(
    const AxlPciIds   *ids,
    AxlPciIdsDeviceFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return -1;
    }
    DeviceTrampoline t = { .fn = fn, .ctx = ctx, .stop_rc = 0 };
    axl_hash_table_foreach(ids->devices, device_trampoline_cb, &t);
    return t.stop_rc;
}

typedef struct {
    AxlPciIdsSubsysFn  fn;
    void              *ctx;
    int                stop_rc;
} SubsysTrampoline;

static void
subsys_trampoline_cb(
    const void  *key,
    void        *value,
    void        *data
    )
{
    SubsysTrampoline *t = data;
    if (t->stop_rc != 0) {
        return;
    }
    uintptr_t k = (uintptr_t)key;
    uint16_t  svid = (uint16_t)(k >> 16);
    uint16_t  sdid = (uint16_t)(k & 0xFFFFu);
    int rc = t->fn(svid, sdid, (const char *)value, t->ctx);
    if (rc != 0) {
        t->stop_rc = rc;
    }
}

int
axl_pci_ids_foreach_subsys(
    const AxlPciIds   *ids,
    AxlPciIdsSubsysFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return -1;
    }
    SubsysTrampoline t = { .fn = fn, .ctx = ctx, .stop_rc = 0 };
    axl_hash_table_foreach(ids->subsystems, subsys_trampoline_cb, &t);
    return t.stop_rc;
}

// ---------------------------------------------------------------------------
// Process-global singleton (thin shim over a single handle)
// ---------------------------------------------------------------------------

static AxlPciIds *g_singleton = NULL;

int
axl_pci_ids_load(
    const char  *override_path
    )
{
    if (g_singleton != NULL) {
        return 0;  /* idempotent — already loaded */
    }

    /* Explicit override path: use it authoritatively. No fallback —
       a non-NULL override means the user named the file they want,
       so missing/malformed errors should reflect that exact choice
       rather than silently load a different file. */
    if (override_path != NULL) {
        return axl_pci_ids_open(override_path, &g_singleton);
    }

    /* Autodiscover: walk companion → cwd. Same convention as memspd's
       jedec.json5 lookup. Distinguish "no candidate at any path" (-1)
       from "candidate found but parse failed" (-2). */
    char *path = axl_resolve_data_file(NULL, "pci-ids.json5");
    if (path == NULL) {
        return -1;
    }
    int rc = axl_pci_ids_open(path, &g_singleton);
    axl_free(path);
    return rc;
}

void
axl_pci_ids_free(
    void
    )
{
    axl_pci_ids_close(g_singleton);
    g_singleton = NULL;
}

const char *
axl_pci_vendor_name(
    uint16_t  vid
    )
{
    return axl_pci_ids_vendor_name(g_singleton, vid);
}

const char *
axl_pci_device_name(
    uint16_t  vid,
    uint16_t  did
    )
{
    return axl_pci_ids_device_name(g_singleton, vid, did);
}

const char *
axl_pci_subsys_name(
    uint16_t  svid,
    uint16_t  sdid
    )
{
    return axl_pci_ids_subsys_name(g_singleton, svid, sdid);
}

int
axl_pci_ids_format_name(
    const AxlPciIds  *ids,
    uint16_t          vid,
    uint16_t          did,
    char             *buf,
    size_t            buflen
    )
{
    if (buf == NULL || buflen == 0) {
        return -1;
    }

    /* Single-source-of-truth fallback chain so every consumer
       renders the same string for the same (vid, did) pair:
         vendor known + device known   → "<vendor> <device>"
         vendor known + device unknown → "<vendor> Device <DID hex>"
         vendor unknown                → "<VID>:<DID>"
       Vendor-unknown short-circuits regardless of device-name
       presence — without a verified vendor the device hit is
       ambiguous provenance, and consumers should see numeric so
       they know to be cautious. */
    const char *vname = axl_pci_ids_vendor_name(ids, vid);
    if (vname == NULL) {
        return axl_snprintf(buf, buflen, "%04x:%04x",
                            (unsigned)vid, (unsigned)did);
    }
    const char *dname = axl_pci_ids_device_name(ids, vid, did);
    if (dname == NULL) {
        return axl_snprintf(buf, buflen, "%s Device %04x",
                            vname, (unsigned)did);
    }
    return axl_snprintf(buf, buflen, "%s %s", vname, dname);
}

int
axl_pci_format_name(
    uint16_t  vid,
    uint16_t  did,
    char     *buf,
    size_t    buflen
    )
{
    return axl_pci_ids_format_name(g_singleton, vid, did, buf, buflen);
}
