/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvstore.c
    Non-volatile key-value storage — wraps firmware variable services.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-nvstore.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
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

static uint32_t
efi_to_axl_attrs(
    UINT32  attrs
    )
{
    uint32_t flags = 0;
    if (attrs & EFI_VARIABLE_NON_VOLATILE) {
        flags |= AXL_NV_PERSISTENT;
    }
    if (attrs & EFI_VARIABLE_BOOTSERVICE_ACCESS) {
        flags |= AXL_NV_BOOT;
    }
    if (attrs & EFI_VARIABLE_RUNTIME_ACCESS) {
        flags |= AXL_NV_RUNTIME;
    }
    return flags;
}

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
        return -1;
    }
    size_t len = 0;
    while (name[len] != '\0' && len < NS_NAME_MAX) {
        len++;
    }
    if (len == 0 || len >= NS_NAME_MAX) {
        axl_warning("namespace name '%s' too long (max %d)",
                    name, NS_NAME_MAX - 1);
        return -1;
    }

    /* Already registered? Allow idempotent re-register with same token,
       reject collision with different token. */
    for (size_t i = 0; i < num_namespaces; i++) {
        if (axl_strcmp(name, namespaces[i].name) == 0) {
            if (namespaces[i].guid == (const EFI_GUID *)backend_token) {
                return 0;
            }
            axl_warning("namespace '%s' already registered with different token",
                        name);
            return -1;
        }
    }

    if (num_namespaces >= MAX_NAMESPACES) {
        axl_warning("namespace table full (max %d)", MAX_NAMESPACES);
        return -1;
    }

    NamespaceEntry *e = &namespaces[num_namespaces];
    for (size_t i = 0; i < len; i++) {
        e->name[i] = name[i];
    }
    e->name[len] = '\0';
    e->guid = (const EFI_GUID *)backend_token;
    num_namespaces++;
    return 0;
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
        return -1;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("get: namespace '%s' not registered", ns ? ns : "(null)");
        return -1;
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

    /* EFI_BUFFER_TOO_SMALL is expected when buf is NULL (size query) */
    if (EFI_ERROR(status)) {
        axl_warning("nvstore get failed: key='%s' status=0x%llx",
                    key, (unsigned long long)status);
        return -1;
    }
    return 0;
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
        return -1;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("set: namespace '%s' not registered", ns ? ns : "(null)");
        return -1;
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
        return -1;
    }
    return 0;
}

int
axl_nvstore_delete(
    const char *ns,
    const char *key
    )
{
    if (key == NULL) {
        return -1;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        axl_warning("delete: namespace '%s' not registered",
                    ns ? ns : "(null)");
        return -1;
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

    if (EFI_ERROR(status)) {
        axl_warning("nvstore delete failed: key='%s' status=0x%llx",
                    key, (unsigned long long)status);
        return -1;
    }
    return 0;
}

int
axl_nvstore_get_attrs(
    const char *ns,
    const char *key,
    uint32_t   *attrs
    )
{
    if (key == NULL || attrs == NULL) {
        return -1;
    }
    const EFI_GUID *guid = ns_to_guid(ns);
    if (guid == NULL) {
        return -1;
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
        return -1;
    }
    *attrs = efi_to_axl_attrs(efi_attrs);
    return 0;
}

// ---------------------------------------------------------------------------
// Public API — iteration
// ---------------------------------------------------------------------------

int
axl_nvstore_iter(
    const char       *ns,
    AxlNvstoreIterFn  cb,
    void             *ctx
    )
{
    if (cb == NULL) {
        return -1;
    }
    const EFI_GUID *target = ns_to_guid(ns);
    if (target == NULL) {
        axl_warning("iter: namespace '%s' not registered",
                    ns ? ns : "(null)");
        return -1;
    }

    /* Working buffer for variable names. UEFI variable names are
       UCS-2; most are short, so 256 chars (512 bytes) is plenty for
       typical use. We grow if the firmware reports a longer name. */
    size_t          name_chars = 256;
    unsigned short *wname = axl_malloc(name_chars * sizeof(unsigned short));
    if (wname == NULL) {
        return -1;
    }
    wname[0] = 0;
    EFI_GUID iter_guid = { 0 };

    char utf8_key[256];
    int  cb_rc = 0;

    while (1) {
        UINTN      name_size = name_chars * sizeof(unsigned short);
        EFI_STATUS status = axl_rt()->GetNextVariableName(
            &name_size,
            wname,
            &iter_guid);

        if (status == EFI_NOT_FOUND) {
            break;
        }
        if (status == EFI_BUFFER_TOO_SMALL) {
            /* name_size was updated to the required size in bytes */
            size_t new_chars = (name_size / sizeof(unsigned short)) + 1;
            unsigned short *bigger = axl_malloc(new_chars * sizeof(unsigned short));
            if (bigger == NULL) {
                axl_free(wname);
                return -1;
            }
            axl_memcpy(bigger, wname, name_chars * sizeof(unsigned short));
            axl_free(wname);
            wname = bigger;
            name_chars = new_chars;
            continue;
        }
        if (EFI_ERROR(status)) {
            axl_free(wname);
            return -1;
        }

        /* Filter by GUID match */
        bool match = true;
        const uint8_t *pa = (const uint8_t *)&iter_guid;
        const uint8_t *pb = (const uint8_t *)target;
        for (size_t i = 0; i < sizeof(EFI_GUID); i++) {
            if (pa[i] != pb[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }

        axl_ucs2_to_utf8_buf(wname, utf8_key, sizeof(utf8_key));
        cb_rc = cb(utf8_key, ctx);
        if (cb_rc != 0) {
            break;
        }
    }

    axl_free(wname);
    return cb_rc;
}
