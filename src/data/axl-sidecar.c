/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sidecar.c
    Common JSON5 sidecar scaffolding.

    Owns the open-with-error-split, schema-field validation, singleton
    lifecycle, and hash-table foreach trampoline patterns previously
    duplicated across `src/pci/axl-pci-ids.c`, `src/pci/axl-pci-class.c`,
    and `tools/memspd.c`. See `include/axl/axl-sidecar.h` for the public
    contract and `src/data/axl-sidecar-internal.h` for the internal
    helpers that axl-sdk's own sidecar consumers reach for.

    Dogfood note: every primitive in this file consumes axl-sdk's
    public API (axl_json_*, axl_file_info, axl_resolve_data_file,
    axl_atexit, axl_hash_table_foreach, axl_warning, ...) per
    feedback_dogfood. No local statics that duplicate logic available
    elsewhere.
**/

#include "axl-sidecar-internal.h"

#include <axl/axl-atexit.h>
#include <axl/axl-fs.h>
#include <axl/axl-json.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-path.h>
#include <axl/axl-sidecar.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("sidecar");

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/* Atexit dispatch: the registered fn receives the data pointer, so
   we wrap the close_fn pair in a tiny stub that knows how to undo a
   _load. The stub allocation lives until atexit runs (or
   _singleton_free pops it explicitly via axl_atexit_remove). */
typedef struct {
    void                **handle_slot;
    /* Back-pointers to the caller's bookkeeping slots so the thunk can
       null them when it frees ctx — prevents `_singleton_free` running
       after the thunk from double-freeing or dereferencing a dangling
       *atexit_ctx_slot. */
    uint32_t             *atexit_slot;
    void                **atexit_ctx_slot;
    AxlSidecarCloseFn     close_fn;
} SingletonAtexitCtx;

/* Trampoline carrying a stop_rc out of axl_hash_table_foreach so
   the early-stop semantics survive the void-callback contract. */
typedef struct {
    AxlSidecarEntryFn  fn;
    void              *ctx;
    int                stop_rc;
} ForeachTrampoline;

// ---------------------------------------------------------------------------
// Public — file / buffer load
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_sidecar_open_file(
    const char     *path,
    AxlJsonReader  *r,
    void          **out_raw
    )
{
    if (path == NULL || r == NULL || out_raw == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }

    /* Existence check first so callers can distinguish "deployment
       problem" from "authoring problem." TOCTOU window between the
       info call and the load is theoretical on a single-threaded
       UEFI system without concurrent FS mutators. */
    AxlFsEntry finfo;
    if (axl_file_info(path, &finfo) != AXL_OK) {
        return AXL_SIDECAR_FILE_MISSING;
    }
    if (!axl_json_load_file_flags(path, AXL_JSON_PARSER_JSON5,
                                  r, out_raw, NULL))
    {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_sidecar_open_buffer(
    const char     *json5,
    size_t          len,
    AxlJsonReader  *r
    )
{
    if (r == NULL || json5 == NULL || len == 0) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    if (!axl_json_parse_flags(json5, len, AXL_JSON_PARSER_JSON5, r)) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    return AXL_SIDECAR_OK;
}

// ---------------------------------------------------------------------------
// Public — schema validation
// ---------------------------------------------------------------------------

AxlSidecarStatus
axl_sidecar_check_schema(
    AxlJsonReader   *r,
    const char      *module_name,
    const uint64_t  *accepted,
    size_t           n_accepted,
    uint64_t        *out_schema
    )
{
    if (r == NULL || module_name == NULL
        || accepted == NULL || n_accepted == 0
        || out_schema == NULL)
    {
        return AXL_SIDECAR_PARSE_ERROR;
    }

    /* `schema` field is REQUIRED. Defaulting to any version silently
       misparses files of other versions (the v2-vs-v1 bug pci-ids
       called out in commit 0d97935). Force explicit declaration so
       the operator gets a precise diagnostic instead of a half-empty
       table. */
    uint64_t schema = 0;
    if (!axl_json_get_uint(r, "schema", &schema)) {
        axl_warning("%s: 'schema' field missing - add 'schema: %llu' "
                    "to the file root (accepted: see module docstring)",
                    module_name,
                    (unsigned long long)accepted[0]);
        return AXL_SIDECAR_PARSE_ERROR;
    }
    for (size_t i = 0; i < n_accepted; i++) {
        if (schema == accepted[i]) {
            *out_schema = schema;
            return AXL_SIDECAR_OK;
        }
    }
    /* Build a comma-separated list of accepted versions for the
       diagnostic. axl_snprintf returns the would-have-written count;
       cap at the local buffer to avoid stack growth on long lists. */
    char accepted_list[64] = "";
    size_t off = 0;
    for (size_t i = 0; i < n_accepted; i++) {
        int n = axl_snprintf(accepted_list + off,
                             sizeof(accepted_list) - off,
                             "%s%llu",
                             (i == 0) ? "" : ", ",
                             (unsigned long long)accepted[i]);
        if (n <= 0 || (size_t)n >= sizeof(accepted_list) - off) {
            break;  /* truncated — diagnostic still useful */
        }
        off += (size_t)n;
    }
    axl_warning("%s: unrecognized schema %llu (accepted: %s)",
                module_name, (unsigned long long)schema, accepted_list);
    return AXL_SIDECAR_PARSE_ERROR;
}

// ---------------------------------------------------------------------------
// Internal — singleton lifecycle
// ---------------------------------------------------------------------------

static void
singleton_atexit_thunk(
    void  *data
    )
{
    SingletonAtexitCtx *ctx = (SingletonAtexitCtx *)data;
    if (ctx == NULL) {
        return;
    }
    if (ctx->handle_slot != NULL && *ctx->handle_slot != NULL
        && ctx->close_fn != NULL)
    {
        ctx->close_fn(*ctx->handle_slot);
        *ctx->handle_slot = NULL;
    }
    /* Null the caller's bookkeeping BEFORE we free ctx. If
       `_axl_sidecar_singleton_free` runs after this thunk (legal in
       cleanup ordering — another module's atexit handler could call
       it on its own way out), it will see a zeroed atexit_slot +
       null atexit_ctx_slot and short-circuit safely. Without these
       two writes, the late _free would `axl_free` a dangling
       pointer. */
    if (ctx->atexit_slot != NULL) {
        *ctx->atexit_slot = 0;
    }
    if (ctx->atexit_ctx_slot != NULL) {
        *ctx->atexit_ctx_slot = NULL;
    }
    /* Free ctx — atexit-runall happens before axl_mem_dump_leaks() in
       axl_runtime_cleanup, so leaving ctx live here would surface as
       a leak in the AxlMem report. */
    axl_free(ctx);
}

AxlSidecarStatus
_axl_sidecar_singleton_load(
    void              **handle_slot,
    uint32_t           *atexit_slot,
    void              **atexit_ctx_slot,
    const char         *override_path,
    const char         *autodiscover_name,
    AxlSidecarOpenFn    open_fn,
    AxlSidecarCloseFn   close_fn
    )
{
    if (handle_slot == NULL || atexit_slot == NULL
        || atexit_ctx_slot == NULL
        || open_fn == NULL || close_fn == NULL)
    {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    /* Idempotent — already loaded. */
    if (*handle_slot != NULL) {
        return AXL_SIDECAR_OK;
    }

    AxlSidecarStatus rc;
    if (override_path != NULL) {
        /* Explicit override is authoritative — no fallback so the
           error code reflects the user's exact request. */
        rc = open_fn(override_path, handle_slot);
    } else {
        if (autodiscover_name == NULL) {
            return AXL_SIDECAR_FILE_MISSING;
        }
        char *path = axl_resolve_data_file(NULL, autodiscover_name);
        if (path == NULL) {
            return AXL_SIDECAR_FILE_MISSING;
        }
        rc = open_fn(path, handle_slot);
        axl_free(path);
    }
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }

    /* Register the close-on-exit hook. Allocation failure here means
       the handle stays loaded (no leak — caller can still invoke the
       _free path manually), we just don't get the auto-cleanup.
       The ctx pointer is stashed in *atexit_ctx_slot so _free can
       reclaim the heap when it deregisters the thunk early. */
    SingletonAtexitCtx *ctx = axl_malloc(sizeof(*ctx));
    if (ctx != NULL) {
        ctx->handle_slot     = handle_slot;
        ctx->atexit_slot     = atexit_slot;
        ctx->atexit_ctx_slot = atexit_ctx_slot;
        ctx->close_fn        = close_fn;
        *atexit_slot     = axl_atexit(singleton_atexit_thunk, ctx);
        if (*atexit_slot == 0) {
            axl_free(ctx);
        } else {
            *atexit_ctx_slot = ctx;
        }
    }
    return AXL_SIDECAR_OK;
}

void
_axl_sidecar_singleton_free(
    void              **handle_slot,
    uint32_t           *atexit_slot,
    void              **atexit_ctx_slot,
    AxlSidecarCloseFn   close_fn
    )
{
    if (handle_slot == NULL) {
        return;
    }
    /* Pop the atexit before invoking close_fn: otherwise the atexit
       thunk will fire on shutdown against an already-freed handle.
       After deregistration the thunk won't free the ctx, so we
       reclaim it here. axl_atexit_remove and axl_free are NULL-safe,
       so the no-atexit path (allocation failed at load time) costs
       nothing here. */
    if (atexit_slot != NULL && *atexit_slot != 0) {
        axl_atexit_remove(*atexit_slot);
        *atexit_slot = 0;
    }
    if (atexit_ctx_slot != NULL && *atexit_ctx_slot != NULL) {
        axl_free(*atexit_ctx_slot);
        *atexit_ctx_slot = NULL;
    }
    if (*handle_slot != NULL && close_fn != NULL) {
        close_fn(*handle_slot);
        *handle_slot = NULL;
    }
}

// ---------------------------------------------------------------------------
// Internal — hash-table foreach with early-stop
// ---------------------------------------------------------------------------

static void
foreach_thunk(
    const void  *key,
    void        *value,
    void        *data
    )
{
    ForeachTrampoline *t = (ForeachTrampoline *)data;
    if (t->stop_rc != 0) {
        return;  /* short-circuit — walker can't actually stop early */
    }
    int rc = t->fn(key, value, t->ctx);
    if (rc != 0) {
        t->stop_rc = rc;
    }
}

int
_axl_sidecar_foreach(
    AxlHashTable       *t,
    AxlSidecarEntryFn   fn,
    void               *ctx
    )
{
    if (t == NULL || fn == NULL) {
        return -1;
    }
    ForeachTrampoline trampoline = { .fn = fn, .ctx = ctx, .stop_rc = 0 };
    axl_hash_table_foreach(t, foreach_thunk, &trampoline);
    return trampoline.stop_rc;
}
