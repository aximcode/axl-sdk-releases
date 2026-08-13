/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvstore.c
    Non-volatile key-value storage — wraps firmware variable services.
**/

#include "../backend/axl-backend.h"
#include "axl-var-internal.h"
#include <axl/axl-nvstore.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("nvstore");

// ---------------------------------------------------------------------------
// Built-in namespace GUIDs
// ---------------------------------------------------------------------------

static const EFI_GUID GLOBAL_GUID = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }
};

static const EFI_GUID APP_GUID = {
    0xA1B2C3D4, 0xE5F6, 0x4789,
    { 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78 }
};

// ---------------------------------------------------------------------------
// Namespace registration table
// ---------------------------------------------------------------------------

#define NS_NAME_MAX     32
#define MAX_NAMESPACES  16

typedef struct {
    char            name[NS_NAME_MAX];
    const EFI_GUID *guid;
} NamespaceEntry;

static NamespaceEntry namespaces[MAX_NAMESPACES] = {
    { "global", &GLOBAL_GUID },
    { "app",    &APP_GUID    },
};
static size_t num_namespaces = 2;

static const EFI_GUID *
ns_to_guid(
    const char *ns
    )
{
    if (ns == NULL) {
        return &GLOBAL_GUID;
    }
    for (size_t i = 0; i < num_namespaces; i++) {
        if (axl_strcmp(ns, namespaces[i].name) == 0) {
            return namespaces[i].guid;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// EFI <-> AXL attribute mapping
// ---------------------------------------------------------------------------

static UINT32
axl_to_efi_attrs(
    uint32_t  flags
    )
{
    UINT32 attrs = 0;
    if (flags & AXL_NV_PERSISTENT) {
        attrs |= EFI_VARIABLE_NON_VOLATILE;
    }
    if (flags & AXL_NV_BOOT) {
        attrs |= EFI_VARIABLE_BOOTSERVICE_ACCESS;
    }
    if (flags & AXL_NV_RUNTIME) {
        attrs |= EFI_VARIABLE_RUNTIME_ACCESS;
    }
    /* Default: boot service access if no flags specified */
    if (attrs == 0) {
        attrs = EFI_VARIABLE_BOOTSERVICE_ACCESS;
    }
    return attrs;
}

/* The EFI -> AXL direction lives in axl-var.c as
   axl_var_attrs_from_efi(), shared with axl_var_enumerate() so the two
   surfaces cannot disagree about the same variable's attributes. Only
   the write direction below is nvstore-specific -- axl-var.h is
   read-only by design. */

// ---------------------------------------------------------------------------
// Public API — registration
// ---------------------------------------------------------------------------

int
axl_nvstore_register_namespace(
    const char *name,
    const void *backend_token
    )
{
    if (name == NULL || backend_token == NULL) {
        return AXL_ERR;
    }
    size_t len = axl_strnlen(name, NS_NAME_MAX);
    if (len == 0 || len >= NS_NAME_MAX) {
        axl_warning("namespace name '%s' too long (max %d)",
                    name, NS_NAME_MAX - 1);
        return AXL_ERR;
    }

    /* Already registered? Allow idempotent re-register with an
       equal token, reject collision with a different one.

       Compare the 16 GUID bytes, NOT the pointer: two translation
       units in one image each keeping a private `static const AxlGuid`
       for the same namespace is the obvious pattern (the API takes a
       `const void *`), and a pointer compare rejected the second one —
       leaving every get/set from that TU aimed at an unregistered
       namespace. The first registration's pointer stays in the table,
       so the lifetime contract is unchanged. */
    for (size_t i = 0; i < num_namespaces; i++) {
        if (axl_strcmp(name, namespaces[i].name) == 0) {
            if (axl_guid_equal((const AxlGuid *)namespaces[i].guid,
                               (const AxlGuid *)backend_token)) {
                return AXL_OK;
            }
            axl_warning("namespace '%s' already registered with different token",
                        name);
            return AXL_ERR;
        }
    }

    if (num_namespaces >= MAX_NAMESPACES) {
        axl_warning("namespace table full (max %d)", MAX_NAMESPACES);
        return AXL_ERR;
    }

    NamespaceEntry *e = &namespaces[num_namespaces];
    for (size_t i = 0; i < len; i++) {
        e->name[i] = name[i];
    }
    e->name[len] = '\0';
    e->guid = (const EFI_GUID *)backend_token;
    num_namespaces++;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API — get / set
// ---------------------------------------------------------------------------

int
axl_nvstore_get(
    const char *ns,
    const char *key,
    void       *buf,
    size_t     *size
    )
{
    if (key == NULL || size == NULL) {
        return AXL_ERR;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("get: namespace '%s' not registered", ns ? ns : "(null)");
        return AXL_ERR;
    }

    unsigned short wkey[128];
    axl_utf8_to_ucs2_buf(key, wkey, 128);

    size_t data_size = *size;
    EFI_STATUS status = axl_rt()->GetVariable(
        wkey,
        (EFI_GUID *)guid,
        NULL,
        &data_size,
        buf);

    *size = data_size;

    /* EFI_BUFFER_TOO_SMALL is the canonical probe-then-grow signal,
       and EFI_NOT_FOUND is the canonical "does this key exist?" probe.
       Both are normal control flow for callers — log at debug, not warn. */
    if (EFI_ERROR(status)) {
        if (status == EFI_BUFFER_TOO_SMALL || status == EFI_NOT_FOUND) {
            axl_debug("nvstore get: key='%s' status=0x%llx",
                      key, (unsigned long long)status);
        } else {
            axl_warning("nvstore get failed: key='%s' status=0x%llx",
                        key, (unsigned long long)status);
        }
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_nvstore_get_alloc(
    const char  *ns,
    const char  *key,
    void       **out_buf,
    size_t      *out_size
    )
{
    if (out_buf == NULL || out_size == NULL || key == NULL) {
        return AXL_ERR;
    }
    *out_buf  = NULL;
    *out_size = 0;

    /* Size probe: GetVariable(Data=NULL, DataSize=0) returns:
         - EFI_NOT_FOUND       → axl_nvstore_get rc=-1, size unchanged at 0
         - EFI_SUCCESS         → axl_nvstore_get rc=0, size=0 (genuinely 0-byte var)
         - EFI_BUFFER_TOO_SMALL → axl_nvstore_get rc=-1, size=actual payload
       The two rc=-1 cases differ only by whether *size was updated
       past zero. Use the probe's rc to distinguish "empty variable"
       (legitimate, success) from "missing / failed" (-1, no value
       to return). */
    size_t needed   = 0;
    int    probe_rc = axl_nvstore_get(ns, key, NULL, &needed);
    if (probe_rc != AXL_OK && needed == 0) {
        /* NOT_FOUND or some other non-truncation read failure. */
        return AXL_ERR;
    }

    /* Allocate needed+1 so callers reading a string variable can
       treat the result as NUL-terminated even when the wire payload
       omitted the NUL. The empty-variable case (needed==0) still
       gets a 1-byte allocation holding a single NUL — the caller
       sees out_size==0 and a non-NULL out_buf they can freely
       deref or pass to printf("%s", ...). */
    void *buf = axl_malloc(needed + 1);
    if (buf == NULL) {
        return AXL_ERR;
    }
    ((uint8_t *)buf)[needed] = 0;

    /* Skip the second GetVariable call when we already know the
       payload is empty — the probe answered the whole question. */
    size_t actual = needed;
    if (needed > 0) {
        if (axl_nvstore_get(ns, key, buf, &actual) != AXL_OK) {
            axl_free(buf);
            return AXL_ERR;
        }
        /* Defensive: a racing concurrent SetVariable could have grown
           the payload between the probe and the read. Don't lie about
           *out_size; report what was actually read and let the caller
           decide whether the truncation matters. */
        if (actual > needed) {
            actual = needed;
        }
        ((uint8_t *)buf)[actual] = 0;
    }
    *out_buf  = buf;
    *out_size = actual;
    return AXL_OK;
}

int
axl_nvstore_set(
    const char *ns,
    const char *key,
    const void *buf,
    size_t      size,
    uint32_t    flags
    )
{
    if (key == NULL) {
        return AXL_ERR;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("set: namespace '%s' not registered", ns ? ns : "(null)");
        return AXL_ERR;
    }

    unsigned short wkey[128];
    axl_utf8_to_ucs2_buf(key, wkey, 128);

    EFI_STATUS status = axl_rt()->SetVariable(
        wkey,
        (EFI_GUID *)guid,
        axl_to_efi_attrs(flags),
        size,
        (void *)buf);

    if (EFI_ERROR(status)) {
        axl_warning("nvstore set failed: key='%s' status=0x%llx",
                    key, (unsigned long long)status);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_nvstore_set_str(
    const char *ns,
    const char *key,
    const char *str,
    uint32_t    flags
    )
{
    if (str == NULL) {
        return AXL_ERR;
    }
    return axl_nvstore_set(ns, key, str, axl_strlen(str) + 1, flags);
}

int
axl_nvstore_get_str(
    const char  *ns,
    const char  *key,
    char       **out_str
    )
{
    if (out_str == NULL) {
        return AXL_ERR;
    }
    *out_str = NULL;

    void   *buf = NULL;
    size_t  sz  = 0;
    if (axl_nvstore_get_alloc(ns, key, &buf, &sz) != AXL_OK) {
        return AXL_ERR;
    }
    /* axl_nvstore_get_alloc guarantees one byte past the payload is
       NUL-extended, so the cast yields a NUL-terminated C string
       even when the wire payload omitted the trailing NUL. */
    *out_str = (char *)buf;
    return AXL_OK;
}

int
axl_nvstore_delete(
    const char *ns,
    const char *key
    )
{
    if (key == NULL) {
        return AXL_ERR;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("delete: namespace '%s' not registered",
                    ns ? ns : "(null)");
        return AXL_ERR;
    }

    unsigned short wkey[128];
    axl_utf8_to_ucs2_buf(key, wkey, 128);

    /* Per UEFI 2.11 §8.2: SetVariable with DataSize=0 deletes the variable.
       Pass attributes=0 too — implementations accept either zeroed attrs
       or the originally-stored attrs. */
    EFI_STATUS status = axl_rt()->SetVariable(
        wkey,
        (EFI_GUID *)guid,
        0,
        0,
        NULL);

    /* EFI_NOT_FOUND on delete is idempotent — caller asked us to
       remove a key that already wasn't there. Not an error. */
    if (EFI_ERROR(status)) {
        if (status == EFI_NOT_FOUND) {
            axl_debug("nvstore delete: key='%s' already absent", key);
        } else {
            axl_warning("nvstore delete failed: key='%s' status=0x%llx",
                        key, (unsigned long long)status);
        }
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_nvstore_get_attrs(
    const char *ns,
    const char *key,
    uint32_t   *attrs
    )
{
    if (key == NULL || attrs == NULL) {
        return AXL_ERR;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        return AXL_ERR;
    }

    unsigned short wkey[128];
    axl_utf8_to_ucs2_buf(key, wkey, 128);

    UINT32     efi_attrs = 0;
    UINTN      data_size = 0;
    EFI_STATUS status = axl_rt()->GetVariable(
        wkey,
        (EFI_GUID *)guid,
        &efi_attrs,
        &data_size,
        NULL);

    /* For an existing variable we expect EFI_BUFFER_TOO_SMALL (buffer
       was NULL / too small). EFI_SUCCESS only happens for zero-length
       variables, which is also fine. */
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        return AXL_ERR;
    }
    *attrs = axl_var_attrs_from_efi(efi_attrs);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API — iteration
// ---------------------------------------------------------------------------

/* The caller's return value travels in cb_rc, never in the walk's own
   return value. axl_var_walk answers only "did the walk work"; a
   caller stopping with -8 is theirs to interpret, not something to
   confuse with AXL_NO_RESOURCES. */
typedef struct {
    const AxlGuid    *target;   /* namespace GUID to match */
    AxlNvstoreIterFn  cb;       /* caller's callback */
    void             *ctx;      /* caller's context */
    int               cb_rc;    /* caller's return value, verbatim */
} NvstoreIterBridge;

static bool
nvstore_iter_bridge_cb(
    const unsigned short *wname,
    const char           *name,
    const AxlGuid        *vendor,
    void                 *ctx
    )
{
    (void)wname;
    NvstoreIterBridge *b = (NvstoreIterBridge *)ctx;

    if (!axl_guid_equal(vendor, b->target)) {
        return true;                   /* different namespace — keep going */
    }

    b->cb_rc = b->cb(name, b->ctx);
    return (b->cb_rc == 0);            /* non-zero from the caller stops it */
}

int
axl_nvstore_iter(
    const char       *ns,
    AxlNvstoreIterFn  cb,
    void             *ctx
    )
{
    if (cb == NULL) {
        return AXL_ERR;
    }
    const EFI_GUID *target = ns_to_guid(ns);
    if (target == NULL) {
        axl_warning("iter: namespace '%s' not registered",
                    ns ? ns : "(null)");
        return AXL_ERR;
    }

    /* The walk itself lives in axl-var.c and is shared with
       axl_var_enumerate() -- see axl-var-internal.h for why there is
       exactly one copy of it. This surface is that walk behind a
       vendor-GUID predicate. */
    NvstoreIterBridge bridge = { (const AxlGuid *)target, cb, ctx, 0 };
    int rc = axl_var_walk(nvstore_iter_bridge_cb, &bridge);

    if (rc != AXL_OK) {
        return AXL_ERR;                /* firmware walk or allocation failed */
    }
    /* Completed (cb_rc 0), or the caller stopped it (cb_rc theirs). */
    return bridge.cb_rc;
}
