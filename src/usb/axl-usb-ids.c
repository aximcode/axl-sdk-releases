/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-usb-ids.c
    USB vendor / device name database loader.

    Handle-based: AxlUsbIds * holds two hash tables (vid → name and
    (vid, pid) → name) parsed from a JSON5 file or buffer. Multiple
    handles can coexist for public+private overlay use; the
    process-global singleton (axl_usb_ids_load and friends) is a
    thin shim around a single internal handle.

    Schema 1 only — hierarchical from the start. USB has no
    subsystem dimension that motivated AxlPciIds's v1 (flat) → v2
    (hierarchical) split, so the only layout is

        { schema: 1,
          vendors: [
            { id: <vid>, name: '<vendor>',
              devices: [
                { pid: <pid>, name: '<device>' },
                ...
              ],
            },
            ...
          ],
        }

    Builds on `axl-sidecar` for file open / schema check / singleton
    lifecycle / foreach trampoline. The only concern this file owns
    is the schema-1 typed walk into the two flat hash tables.
**/

#include "../data/axl-sidecar-internal.h"

#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-sidecar.h>
#include <axl/axl-str.h>
#include <axl/axl-usb.h>

AXL_LOG_DOMAIN("usb-ids");

// ---------------------------------------------------------------------------
// Handle layout
// ---------------------------------------------------------------------------

struct AxlUsbIds {
    AxlHashTable *vendors;     /* uintptr_t(vid)            → char* */
    AxlHashTable *devices;     /* uintptr_t(vid<<16 | pid)  → char* */
};

/* Pack two 16-bit IDs into a single hash-table key. axl_direct_hash
   is bijective on the pointer bits, so distinct (vid, pid) pairs
   never collide. Keys are non-NULL because the loader rejects any
   entry whose vid is zero. */
static inline void *
pair_key(
    uint16_t  vid,
    uint16_t  pid
    )
{
    return (void *)((uintptr_t)((uint32_t)vid << 16) | (uintptr_t)pid);
}

// ---------------------------------------------------------------------------
// Schema-specific typed walk
// ---------------------------------------------------------------------------

/* Common helper: copy @p src into the table at @p key, freeing on
   insert failure. axl_strdup NULL is silently dropped. */
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

/* Hierarchical schema 1: walk vendors[].devices[] and insert into
   the two flat hash tables. Vendor 'name' is optional — an entry
   may exist purely as a parent for nested devices. Device entries
   require both 'pid' and 'name'. */
static void
parse_schema_v1(
    AxlJsonReader  *r,
    AxlUsbIds      *ids
    )
{
    AxlJsonArrayIter v_it;
    if (!axl_json_array_begin(r, "vendors", &v_it)) {
        return;
    }
    AxlJsonReader v_entry;
    while (axl_json_array_next(&v_it, &v_entry)) {
        uint64_t vid64 = 0;
        char     vname[AXL_USB_VENDOR_NAME_MAX] = "";
        if (!axl_json_get_uint(&v_entry, "id", &vid64)
            || vid64 == 0 || vid64 > 0xFFFFu)
        {
            continue;
        }
        if (axl_json_get_string(&v_entry, "name",
                                vname, sizeof(vname)))
        {
            ids_insert_string(ids->vendors,
                              (void *)(uintptr_t)vid64, vname);
        }
        AxlJsonArrayIter d_it;
        if (!axl_json_array_begin(&v_entry, "devices", &d_it)) {
            continue;
        }
        AxlJsonReader d_entry;
        while (axl_json_array_next(&d_it, &d_entry)) {
            uint64_t pid64 = 0;
            char     dname[AXL_USB_DEVICE_NAME_MAX] = "";
            if (!axl_json_get_uint(&d_entry, "pid", &pid64)
                || pid64 > 0xFFFFu
                || !axl_json_get_string(&d_entry, "name",
                                        dname, sizeof(dname)))
            {
                continue;
            }
            ids_insert_string(ids->devices,
                              pair_key((uint16_t)vid64,
                                       (uint16_t)pid64),
                              dname);
        }
    }
}

static AxlSidecarStatus
ids_fill(
    AxlUsbIds      *ids,
    AxlJsonReader  *r
    )
{
    static const uint64_t accepted[] = { 1u };
    uint64_t schema = 0;
    AxlSidecarStatus rc = axl_sidecar_check_schema(
        r, "usb-ids", accepted, sizeof(accepted) / sizeof(accepted[0]),
        &schema);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }
    parse_schema_v1(r, ids);
    return AXL_SIDECAR_OK;
}

// ---------------------------------------------------------------------------
// Handle alloc / free
// ---------------------------------------------------------------------------

static AxlUsbIds *
ids_alloc(
    void
    )
{
    AxlUsbIds *ids = axl_malloc(sizeof(*ids));
    if (ids == NULL) {
        return NULL;
    }
    ids->vendors = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    ids->devices = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    if (ids->vendors == NULL || ids->devices == NULL) {
        axl_usb_ids_close(ids);
        return NULL;
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Handle API
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_usb_ids_open(
    const char   *path,
    AxlUsbIds   **out
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
    AxlUsbIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    axl_free(raw);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_usb_ids_close(ids);
        return fill_rc;
    }

    axl_debug("usb-ids: %zu vendors / %zu devices from %s",
              axl_hash_table_size(ids->vendors),
              axl_hash_table_size(ids->devices),
              path);
    *out = ids;
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_usb_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlUsbIds   **out
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
    AxlUsbIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_usb_ids_close(ids);
        return fill_rc;
    }
    *out = ids;
    return AXL_SIDECAR_OK;
}

void
axl_usb_ids_close(
    AxlUsbIds  *ids
    )
{
    if (ids == NULL) {
        return;
    }
    axl_hash_table_free(ids->vendors);
    axl_hash_table_free(ids->devices);
    axl_free(ids);
}

const char *
axl_usb_ids_vendor_name(
    const AxlUsbIds  *ids,
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
axl_usb_ids_device_name(
    const AxlUsbIds  *ids,
    uint16_t          vid,
    uint16_t          pid
    )
{
    if (ids == NULL || vid == 0) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(ids->devices,
                                               pair_key(vid, pid));
}

// ---------------------------------------------------------------------------
// Iter API
// ---------------------------------------------------------------------------

typedef struct {
    AxlUsbIdsVendorFn  fn;
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
axl_usb_ids_foreach_vendor(
    const AxlUsbIds   *ids,
    AxlUsbIdsVendorFn  fn,
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
    AxlUsbIdsDeviceFn  fn;
    void              *ctx;
} DeviceAdapterCtx;

static int
device_adapter(const void *key, void *value, void *data)
{
    DeviceAdapterCtx *a = data;
    uintptr_t k = (uintptr_t)key;
    uint16_t  vid = (uint16_t)(k >> 16);
    uint16_t  pid = (uint16_t)(k & 0xFFFFu);
    return a->fn(vid, pid, (const char *)value, a->ctx);
}

int
axl_usb_ids_foreach_device(
    const AxlUsbIds   *ids,
    AxlUsbIdsDeviceFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return AXL_ERR;
    }
    DeviceAdapterCtx adapter = { .fn = fn, .ctx = ctx };
    return _axl_sidecar_foreach(ids->devices, device_adapter, &adapter);
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

int
axl_usb_ids_format_name(
    const AxlUsbIds  *ids,
    uint16_t          vid,
    uint16_t          pid,
    char             *buf,
    size_t            buflen
    )
{
    if (buf == NULL || buflen == 0) {
        return AXL_ERR;
    }
    /* Single-source-of-truth fallback chain so every consumer
       renders the same string for the same (vid, pid) pair:
         vendor known + device known   → "<vendor> <device>"
         vendor known + device unknown → "<vendor> Device <pid hex>"
         vendor unknown                → "<vid>:<pid>"
       Vendor-unknown short-circuits regardless of device-name
       presence — without a verified vendor a device hit is
       ambiguous provenance, so the fallback is always all-numeric. */
    const char *vname = axl_usb_ids_vendor_name(ids, vid);
    if (vname == NULL) {
        return axl_snprintf(buf, buflen, "%04x:%04x",
                            (unsigned)vid, (unsigned)pid);
    }
    const char *dname = axl_usb_ids_device_name(ids, vid, pid);
    if (dname == NULL) {
        return axl_snprintf(buf, buflen, "%s Device %04x",
                            vname, (unsigned)pid);
    }
    return axl_snprintf(buf, buflen, "%s %s", vname, dname);
}

// ---------------------------------------------------------------------------
// Process-global singleton
// ---------------------------------------------------------------------------

static void     *g_singleton;        /* AxlUsbIds * cast */
static uint32_t  g_atexit_handle;
static void     *g_atexit_ctx;       /* opaque, owned by sidecar helper */

static AxlSidecarStatus
singleton_open_thunk(const char *path, void **out)
{
    return axl_usb_ids_open(path, (AxlUsbIds **)out);
}

static void
singleton_close_thunk(void *handle)
{
    axl_usb_ids_close((AxlUsbIds *)handle);
}

AxlSidecarStatus
axl_usb_ids_load(
    const char  *override_path
    )
{
    return _axl_sidecar_singleton_load(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        override_path, "usb-ids.json5",
        singleton_open_thunk, singleton_close_thunk);
}

void
axl_usb_ids_free(
    void
    )
{
    _axl_sidecar_singleton_free(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        singleton_close_thunk);
}

const char *
axl_usb_vendor_name(
    uint16_t  vid
    )
{
    return axl_usb_ids_vendor_name((AxlUsbIds *)g_singleton, vid);
}

const char *
axl_usb_device_name(
    uint16_t  vid,
    uint16_t  pid
    )
{
    return axl_usb_ids_device_name((AxlUsbIds *)g_singleton, vid, pid);
}

int
axl_usb_format_name(
    uint16_t  vid,
    uint16_t  pid,
    char     *buf,
    size_t    buflen
    )
{
    return axl_usb_ids_format_name((AxlUsbIds *)g_singleton,
                                   vid, pid, buf, buflen);
}
