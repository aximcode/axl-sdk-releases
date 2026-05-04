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
    doesn't drag in the JSON parser. Builds on top of `axl-sidecar`
    (file open / schema check / singleton lifecycle / foreach
    trampoline) — the only concern this file owns is the
    schema-specific typed walk into the three flat hash tables.
**/

#include "../data/axl-sidecar-internal.h"

#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-pci.h>
#include <axl/axl-sidecar.h>
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
// Schema-specific typed walks (concern: turn JSON5 entries into hash-
// table inserts; everything else lives in axl-sidecar.c)
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
// loaders return AXL_SIDECAR_PARSE_ERROR on unrecognized schemas
// rather than silently misparsing.
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
    if (axl_hash_table_insert(t, key, owned) == AXL_HASH_TABLE_ERR) {
        axl_free(owned);
    }
}

static void
parse_vendors(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "vendors", &it)) {
        return;  /* a database may legitimately ship without vendors */
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
        ids_insert_string(out, (void *)(uintptr_t)id64, name);
    }
}

static void
parse_devices(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "devices", &it)) {
        return;
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
        ids_insert_string(out,
                          pair_key((uint16_t)vid64, (uint16_t)did64),
                          name);
    }
}

static void
parse_subsystems(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "subsystems", &it)) {
        return;  /* subsystems array is optional */
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
        ids_insert_string(out,
                          pair_key((uint16_t)svid64, (uint16_t)sdid64),
                          name);
    }
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

/* Schema-aware fill. Returns AXL_SIDECAR_OK on a recognized schema,
   AXL_SIDECAR_PARSE_ERROR otherwise. The schema-field validation
   itself is delegated to axl_sidecar_check_schema. */
static AxlSidecarStatus
ids_fill(
    AxlPciIds      *ids,
    AxlJsonReader  *r
    )
{
    static const uint64_t accepted[] = { 1u, 2u };
    uint64_t schema = 0;
    AxlSidecarStatus rc = axl_sidecar_check_schema(
        r, "pci-ids", accepted, sizeof(accepted) / sizeof(accepted[0]),
        &schema);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }

    if (schema == 1) {
        parse_vendors(r, ids->vendors);
        parse_devices(r, ids->devices);
        parse_subsystems(r, ids->subsystems);
    } else /* schema == 2 */ {
        parse_schema_v2(r, ids);
        /* Schema 2 also accepts a top-level subsystems[] block
           alongside the nested form — useful for orphan entries the
           maintainer doesn't know which device to nest under. The
           nested + flat results merge into the same hash table. */
        parse_subsystems(r, ids->subsystems);
    }
    return AXL_SIDECAR_OK;
}

// ---------------------------------------------------------------------------
// Handle alloc / free
// ---------------------------------------------------------------------------

/* Allocate a handle with all three empty tables. Returns NULL on any
   allocation failure (caller does not have to free a partial
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

// ---------------------------------------------------------------------------
// Handle API
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_pci_ids_open(
    const char   *path,
    AxlPciIds   **out
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

    AxlPciIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    axl_free(raw);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_pci_ids_close(ids);
        return fill_rc;
    }

    axl_debug("pci-ids: %zu vendors / %zu devices / %zu subsystems from %s",
              axl_hash_table_size(ids->vendors),
              axl_hash_table_size(ids->devices),
              axl_hash_table_size(ids->subsystems),
              path);
    *out = ids;
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_pci_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlPciIds   **out
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
    AxlPciIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_pci_ids_close(ids);
        return fill_rc;
    }
    *out = ids;
    return AXL_SIDECAR_OK;
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

typedef struct {
    AxlPciIdsVendorFn  fn;
    void              *ctx;
} VendorAdapterCtx;

static int
vendor_adapter(const void *key, void *value, void *data)
{
    VendorAdapterCtx *a = data;
    uint16_t vid = (uint16_t)(uintptr_t)key;
    return a->fn(vid, (const char *)value, a->ctx);
}

int
axl_pci_ids_foreach_vendor(
    const AxlPciIds   *ids,
    AxlPciIdsVendorFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return AXL_ERR;
    }
    VendorAdapterCtx adapter = { .fn = fn, .ctx = ctx };
    return _axl_sidecar_foreach(ids->vendors, vendor_adapter, &adapter);
}

typedef struct {
    AxlPciIdsDeviceFn  fn;
    void              *ctx;
} DeviceAdapterCtx;

static int
device_adapter(const void *key, void *value, void *data)
{
    DeviceAdapterCtx *a = data;
    uintptr_t k = (uintptr_t)key;
    uint16_t  vid = (uint16_t)(k >> 16);
    uint16_t  did = (uint16_t)(k & 0xFFFFu);
    return a->fn(vid, did, (const char *)value, a->ctx);
}

int
axl_pci_ids_foreach_device(
    const AxlPciIds   *ids,
    AxlPciIdsDeviceFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return AXL_ERR;
    }
    DeviceAdapterCtx adapter = { .fn = fn, .ctx = ctx };
    return _axl_sidecar_foreach(ids->devices, device_adapter, &adapter);
}

typedef struct {
    AxlPciIdsSubsysFn  fn;
    void              *ctx;
} SubsysAdapterCtx;

static int
subsys_adapter(const void *key, void *value, void *data)
{
    SubsysAdapterCtx *a = data;
    uintptr_t k = (uintptr_t)key;
    uint16_t  svid = (uint16_t)(k >> 16);
    uint16_t  sdid = (uint16_t)(k & 0xFFFFu);
    return a->fn(svid, sdid, (const char *)value, a->ctx);
}

int
axl_pci_ids_foreach_subsys(
    const AxlPciIds   *ids,
    AxlPciIdsSubsysFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return AXL_ERR;
    }
    SubsysAdapterCtx adapter = { .fn = fn, .ctx = ctx };
    return _axl_sidecar_foreach(ids->subsystems, subsys_adapter, &adapter);
}

// ---------------------------------------------------------------------------
// Process-global singleton (thin shim over a single handle, driven by
// the shared sidecar machinery)
// ---------------------------------------------------------------------------

static void     *g_singleton;        /* AxlPciIds * cast through void * */
static uint32_t  g_atexit_handle;
static void     *g_atexit_ctx;       /* opaque heap pointer owned by sidecar helper */

static AxlSidecarStatus
singleton_open_thunk(const char *path, void **out)
{
    return axl_pci_ids_open(path, (AxlPciIds **)out);
}

static void
singleton_close_thunk(void *handle)
{
    axl_pci_ids_close((AxlPciIds *)handle);
}

AxlSidecarStatus
axl_pci_ids_load(
    const char  *override_path
    )
{
    return _axl_sidecar_singleton_load(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        override_path, "pci-ids.json5",
        singleton_open_thunk, singleton_close_thunk);
}

void
axl_pci_ids_free(
    void
    )
{
    _axl_sidecar_singleton_free(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        singleton_close_thunk);
}

const char *
axl_pci_vendor_name(
    uint16_t  vid
    )
{
    return axl_pci_ids_vendor_name((AxlPciIds *)g_singleton, vid);
}

const char *
axl_pci_device_name(
    uint16_t  vid,
    uint16_t  did
    )
{
    return axl_pci_ids_device_name((AxlPciIds *)g_singleton, vid, did);
}

const char *
axl_pci_subsys_name(
    uint16_t  svid,
    uint16_t  sdid
    )
{
    return axl_pci_ids_subsys_name((AxlPciIds *)g_singleton, svid, sdid);
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
        return AXL_ERR;
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
    return axl_pci_ids_format_name(
        (AxlPciIds *)g_singleton, vid, did, buf, buflen);
}
