/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvstore.c
    Non-volatile key-value storage — wraps firmware variable services.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-nvstore.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("nvstore");

// ---------------------------------------------------------------------------
// Namespace → GUID mapping
// ---------------------------------------------------------------------------

static const EFI_GUID GLOBAL_GUID = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }
};

static const EFI_GUID APP_GUID = {
    0xA1B2C3D4, 0xE5F6, 0x4789,
    { 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78 }
};

static const EFI_GUID *
ns_to_guid(
    const char *ns
    )
{
    if (ns == NULL || axl_strcmp(ns, "global") == 0) {
        return &GLOBAL_GUID;
    }
    if (axl_strcmp(ns, "app") == 0) {
        return &APP_GUID;
    }
    return &GLOBAL_GUID;
}

// ---------------------------------------------------------------------------
// UTF-8 key → UCS-2 conversion (local, avoids AxlData dependency)
// ---------------------------------------------------------------------------

static void
key_to_ucs2(
    const char     *key,
    unsigned short *out,
    size_t          out_chars
    )
{
    size_t i;
    for (i = 0; i < out_chars - 1 && key[i] != '\0'; i++) {
        out[i] = (unsigned short)(unsigned char)key[i];
    }
    out[i] = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_nvstore_get(
    const char *ns,
    const char *key,
    void       *buf,
    size_t     *size
    )
{
    const EFI_GUID *guid;
    unsigned short  wkey[128];
    size_t           data_size;
    EFI_STATUS      status;

    if (key == NULL || size == NULL) {
        return -1;
    }

    guid = ns_to_guid(ns);
    key_to_ucs2(key, wkey, 128);
    data_size = (size_t)*size;

    status = axl_rt()->GetVariable(
        (unsigned short *)wkey,
        (EFI_GUID *)guid,
        NULL,
        &data_size,
        buf);

    *size = (size_t)data_size;

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
    const EFI_GUID *guid;
    unsigned short  wkey[128];
    UINT32          attrs;
    EFI_STATUS      status;

    if (key == NULL) {
        return -1;
    }

    guid = ns_to_guid(ns);
    key_to_ucs2(key, wkey, 128);

    /* Map AXL flags to EFI variable attributes */
    attrs = 0;
    if (flags & AXL_NV_PERSISTENT) {
        attrs |= 0x00000001;  /* EFI_VARIABLE_NON_VOLATILE */
    }
    if (flags & AXL_NV_BOOT) {
        attrs |= 0x00000002;  /* EFI_VARIABLE_BOOTSERVICE_ACCESS */
    }
    if (flags & AXL_NV_RUNTIME) {
        attrs |= 0x00000004;  /* EFI_VARIABLE_RUNTIME_ACCESS */
    }
    /* Default: boot service access if no flags specified */
    if (attrs == 0) {
        attrs = 0x00000002;  /* EFI_VARIABLE_BOOTSERVICE_ACCESS */
    }

    status = axl_rt()->SetVariable(
        (unsigned short *)wkey,
        (EFI_GUID *)guid,
        attrs,
        (size_t)size,
        (void *)buf);

    if (EFI_ERROR(status)) {
        axl_warning("nvstore set failed: key='%s' status=0x%llx",
                    key, (unsigned long long)status);
        return -1;
    }

    return 0;
}
