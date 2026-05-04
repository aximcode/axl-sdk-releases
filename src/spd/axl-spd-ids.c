/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd-ids.c
    JEDEC manufacturer-name database loader.

    Handle-based: AxlSpdIds * holds one hash table (code → name)
    parsed from a JSON5 file or buffer. Schema 1 only (a flat
    `vendors[]` array with `{ code, name }` entries) — JEDEC
    JEP-106 has no nested vendor/device/subsystem hierarchy that
    would motivate the v2 layout AxlPciIds carries.

    Builds on `axl-sidecar` for file open / schema check / singleton
    lifecycle / foreach trampoline. The only concern this file owns
    is the schema-1 typed walk into the single hash table.
**/

#include "../data/axl-sidecar-internal.h"

#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-sidecar.h>
#include <axl/axl-spd.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("spd-ids");

// ---------------------------------------------------------------------------
// Handle layout
// ---------------------------------------------------------------------------

struct AxlSpdIds {
    AxlHashTable *vendors;     /* uintptr_t(code) → char* */
};

// ---------------------------------------------------------------------------
// Schema-specific typed walk
// ---------------------------------------------------------------------------

static void
parse_vendors(
    AxlJsonReader  *r,
    AxlHashTable   *out
    )
{
    AxlJsonArrayIter it;
    if (!axl_json_array_begin(r, "vendors", &it)) {
        return;
    }
    AxlJsonReader entry;
    while (axl_json_array_next(&it, &entry)) {
        uint64_t code64 = 0;
        char     name[AXL_SPD_VENDOR_NAME_MAX] = "";
        if (!axl_json_get_uint(&entry, "code", &code64)
            || code64 == 0 || code64 > 0xFFFFu
            || !axl_json_get_string(&entry, "name", name, sizeof(name)))
        {
            continue;
        }
        char *name_owned = axl_strdup(name);
        if (name_owned == NULL) {
            continue;
        }
        if (axl_hash_table_insert(out,
                                  (void *)(uintptr_t)code64,
                                  name_owned) < 0) {
            axl_free(name_owned);
        }
    }
}

static AxlSidecarStatus
ids_fill(
    AxlSpdIds      *ids,
    AxlJsonReader  *r
    )
{
    static const uint64_t accepted[] = { 1u };
    uint64_t schema = 0;
    AxlSidecarStatus rc = axl_sidecar_check_schema(
        r, "jedec", accepted, sizeof(accepted) / sizeof(accepted[0]),
        &schema);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }
    parse_vendors(r, ids->vendors);
    return AXL_SIDECAR_OK;
}

// ---------------------------------------------------------------------------
// Handle alloc / free
// ---------------------------------------------------------------------------

static AxlSpdIds *
ids_alloc(
    void
    )
{
    AxlSpdIds *ids = axl_malloc(sizeof(*ids));
    if (ids == NULL) {
        return NULL;
    }
    ids->vendors = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    if (ids->vendors == NULL) {
        axl_spd_ids_close(ids);
        return NULL;
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Handle API
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_spd_ids_open(
    const char   *path,
    AxlSpdIds   **out
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

    AxlSpdIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    axl_free(raw);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_spd_ids_close(ids);
        return fill_rc;
    }

    axl_debug("jedec: %zu vendor entries from %s",
              axl_hash_table_size(ids->vendors), path);
    *out = ids;
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_spd_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlSpdIds   **out
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
    AxlSpdIds *ids = ids_alloc();
    if (ids == NULL) {
        axl_json_free(&r);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    AxlSidecarStatus fill_rc = ids_fill(ids, &r);
    axl_json_free(&r);
    if (fill_rc != AXL_SIDECAR_OK) {
        axl_spd_ids_close(ids);
        return fill_rc;
    }
    *out = ids;
    return AXL_SIDECAR_OK;
}

void
axl_spd_ids_close(
    AxlSpdIds  *ids
    )
{
    if (ids == NULL) {
        return;
    }
    axl_hash_table_free(ids->vendors);
    axl_free(ids);
}

const char *
axl_spd_ids_vendor_name(
    const AxlSpdIds  *ids,
    uint16_t          code
    )
{
    if (ids == NULL || code == 0) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(ids->vendors,
                                               (void *)(uintptr_t)code);
}

// ---------------------------------------------------------------------------
// Iter API
// ---------------------------------------------------------------------------

typedef struct {
    AxlSpdIdsVendorFn  fn;
    void              *ctx;
} VendorAdapterCtx;

static int
vendor_adapter(const void *key, void *value, void *data)
{
    VendorAdapterCtx *a = data;
    uint16_t code = (uint16_t)(uintptr_t)key;
    return a->fn(code, (const char *)value, a->ctx);
}

int
axl_spd_ids_foreach_vendor(
    const AxlSpdIds   *ids,
    AxlSpdIdsVendorFn  fn,
    void              *ctx
    )
{
    if (ids == NULL || fn == NULL) {
        return AXL_ERR;
    }
    VendorAdapterCtx adapter = { .fn = fn, .ctx = ctx };
    return _axl_sidecar_foreach(ids->vendors, vendor_adapter, &adapter);
}

// ---------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------

int
axl_spd_ids_format_name(
    const AxlSpdIds  *ids,
    uint16_t          code,
    char             *buf,
    size_t            buflen
    )
{
    if (buf == NULL || buflen == 0) {
        return AXL_ERR;
    }
    const char *name = axl_spd_ids_vendor_name(ids, code);
    if (name != NULL) {
        return axl_snprintf(buf, buflen, "%s", name);
    }
    /* Numeric fallback — uppercase 4-digit hex matches the existing
       memspd tool convention so consumers transitioning to the new
       API see the same output for unknown codes. */
    return axl_snprintf(buf, buflen, "0x%04X", (unsigned)code);
}

// ---------------------------------------------------------------------------
// Process-global singleton
// ---------------------------------------------------------------------------

static void     *g_singleton;        /* AxlSpdIds * cast */
static uint32_t  g_atexit_handle;
static void     *g_atexit_ctx;       /* opaque, owned by sidecar helper */

static AxlSidecarStatus
singleton_open_thunk(const char *path, void **out)
{
    return axl_spd_ids_open(path, (AxlSpdIds **)out);
}

static void
singleton_close_thunk(void *handle)
{
    axl_spd_ids_close((AxlSpdIds *)handle);
}

AxlSidecarStatus
axl_spd_ids_load(
    const char  *override_path
    )
{
    return _axl_sidecar_singleton_load(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        override_path, "jedec.json5",
        singleton_open_thunk, singleton_close_thunk);
}

void
axl_spd_ids_free(
    void
    )
{
    _axl_sidecar_singleton_free(
        &g_singleton, &g_atexit_handle, &g_atexit_ctx,
        singleton_close_thunk);
}

const char *
axl_spd_vendor_name(
    uint16_t  code
    )
{
    return axl_spd_ids_vendor_name((AxlSpdIds *)g_singleton, code);
}

int
axl_spd_format_name(
    uint16_t  code,
    char     *buf,
    size_t    buflen
    )
{
    return axl_spd_ids_format_name((AxlSpdIds *)g_singleton,
                                   code, buf, buflen);
}
