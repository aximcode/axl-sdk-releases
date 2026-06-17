/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-protocol.c
    Protocol registry — name-keyed UEFI protocol discovery and registration.
**/

#include <stdarg.h>
#include "../backend/axl-backend.h"
#include "axl-protocol-internal.h"
#include <axl/axl-sys.h>
#include <axl/axl-driver.h>   /* axl_protocol_install / _uninstall (the seam) */
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("protocol");

/* The protocol registry treats AxlGuid (public C type) and EFI_GUID
   (internal UEFI type) interchangeably via memcpy/memcmp. The
   layout match is documented in <axl/axl-sys.h>; assert it here so
   any future drift surfaces at compile time, not as a 16-byte
   GUID-corruption bug at runtime. */
_Static_assert(sizeof(AxlGuid) == sizeof(EFI_GUID),
               "AxlGuid must be ABI-compatible with EFI_GUID");

// ---------------------------------------------------------------------------
// Name → GUID mapping
// ---------------------------------------------------------------------------

/* axl_protocol_lookup_guid prototype lives in axl-protocol-internal.h
   so axl-sys.c can call it without the prior extern-at-point-of-use
   workaround. */

typedef struct {
    const char     *name;
    const EFI_GUID *guid;
} ProtocolEntry;

/* TCG2 (TPM 2.0) protocol GUID — not in generated headers */
static const EFI_GUID TCG2_PROTOCOL_GUID = {
    0x607f766c, 0x7455, 0x42be,
    { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f }
};

/* Runtime-pinned (name → GUID) registry populated by
   axl_protocol_register_name. Process-lifetime; consulted after the
   built-in table and before the FNV-1a fallback. Fixed-size for
   simplicity — pre-1.0 there's no use case approaching this many
   custom protocol names. */
#define AXL_PROTOCOL_CUSTOM_MAX 16

typedef struct {
    char     *name;
    EFI_GUID  guid;
    bool      active;
} CustomEntry;

static CustomEntry custom_table[AXL_PROTOCOL_CUSTOM_MAX];
static size_t      custom_count;

static const ProtocolEntry protocol_table[] = {
    { "smbios",         &EFI_SMBIOS_PROTOCOL_GUID },
    { "tcg2",           &TCG2_PROTOCOL_GUID },
    { "shell",          &gEfiShellProtocolGuid },
    { "shell-params",   &gEfiShellParametersProtocolGuid },
    { "simple-network", &EFI_SIMPLE_NETWORK_PROTOCOL_GUID },
    { "nii",            &gEfiNetworkInterfaceIdentifierProtocolGuid_31 },
    { "nii-legacy",     &gEfiNetworkInterfaceIdentifierProtocolGuid },
    { "device-path",    &EFI_DEVICE_PATH_PROTOCOL_GUID },
    { "loaded-image",   &EFI_LOADED_IMAGE_PROTOCOL_GUID },
    { "simple-fs",      &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID },
    { "ram-disk",       &EFI_RAM_DISK_PROTOCOL_GUID },
    { "tcp4",           &EFI_TCP4_PROTOCOL_GUID },
    { "tcp4-sb",        &EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID },
    { "ip4",            &EFI_IP4_PROTOCOL_GUID },
    { "ip4-config2",    &EFI_IP4_CONFIG2_PROTOCOL_GUID },
    { "dhcp4",          &EFI_DHCP4_PROTOCOL_GUID },
    { "dhcp4-sb",       &EFI_DHCP4_SERVICE_BINDING_PROTOCOL_GUID },
    { "dns4",           &EFI_DNS4_PROTOCOL_GUID },
    { "dns4-sb",        &EFI_DNS4_SERVICE_BINDING_PROTOCOL_GUID },
    { NULL,             NULL }
};

/**
 * @brief Generate a deterministic GUID from a name string.
 *
 * Simple hash-based GUID for custom/unknown protocol names.
 * Not cryptographic — just needs to be deterministic and unique.
 */
static EFI_GUID
guid_from_name(
    const char *name
    )
{
    EFI_GUID g = { 0 };
    uint32_t hash = 0x811C9DC5;  /* FNV-1a offset basis */

    for (const char *p = name; *p != '\0'; p++) {
        hash ^= (uint32_t)(unsigned char)*p;
        hash *= 0x01000193;  /* FNV-1a prime */
    }

    g.Data1 = hash;
    g.Data2 = (uint16_t)(hash >> 16);
    g.Data3 = (uint16_t)(hash ^ 0x4AE2);
    g.Data4[0] = 0xAA;  /* variant bits */
    g.Data4[1] = (uint8_t)(hash >> 8);
    g.Data4[2] = (uint8_t)(hash >> 16);
    g.Data4[3] = (uint8_t)(hash >> 24);
    g.Data4[4] = (uint8_t)(hash);
    g.Data4[5] = (uint8_t)(hash >> 4);
    g.Data4[6] = (uint8_t)(hash >> 12);
    g.Data4[7] = (uint8_t)(hash >> 20);

    return g;
}

/* Exposed to axl-sys.c for axl_handle_protocol */
const EFI_GUID *
axl_protocol_lookup_guid(
    const char *name,
    EFI_GUID   *fallback
    )
{
    if (name == NULL) {
        return NULL;
    }

    for (const ProtocolEntry *e = protocol_table; e->name != NULL; e++) {
        if (axl_strcmp(name, e->name) == 0) {
            return e->guid;
        }
    }

    /* Caller-pinned via axl_protocol_register_name — consulted before
       the FNV-1a fallback so a custom name resolves to the consumer's
       published GUID across all protocol-registry entry points. */
    for (size_t i = 0; i < custom_count; i++) {
        if (custom_table[i].active &&
            axl_strcmp(name, custom_table[i].name) == 0) {
            return &custom_table[i].guid;
        }
    }

    /* Custom name with no caller pinning — generate a deterministic
       GUID. Same name string always produces the same GUID, but
       external consumers can't reproduce it without the same string. */
    *fallback = guid_from_name(name);
    return fallback;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_protocol_register_name(
    const char    *name,
    const AxlGuid *guid
    )
{
    if (name == NULL || guid == NULL || *name == '\0') {
        return AXL_ERR;
    }

    /* Refuse to shadow built-in well-known names. The built-in table
       is the contract for portable protocol discovery — letting a
       consumer rebind "smbios" to a private GUID would silently break
       any other code that opens "smbios" expecting the spec protocol. */
    for (const ProtocolEntry *e = protocol_table; e->name != NULL; e++) {
        if (axl_strcmp(name, e->name) == 0) {
            return AXL_ERR;
        }
    }

    /* Idempotent for an exact (name, guid) match; reject conflicts. */
    for (size_t i = 0; i < custom_count; i++) {
        if (!custom_table[i].active ||
            axl_strcmp(name, custom_table[i].name) != 0) {
            continue;
        }
        if (axl_memcmp(&custom_table[i].guid, guid, sizeof(EFI_GUID)) == 0) {
            return AXL_OK;
        }
        return AXL_ERR;
    }

    /* Allocate before mutating any registry state — leaves the
       table untouched on OOM so the next call sees the same
       capacity rather than wasting a slot. */
    char *copy = axl_strdup(name);
    if (copy == NULL) {
        return AXL_ERR;
    }

    /* Find an inactive slot, or append a new one. */
    size_t slot = AXL_PROTOCOL_CUSTOM_MAX;
    for (size_t i = 0; i < custom_count; i++) {
        if (!custom_table[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == AXL_PROTOCOL_CUSTOM_MAX) {
        if (custom_count >= AXL_PROTOCOL_CUSTOM_MAX) {
            axl_free(copy);
            return AXL_ERR;
        }
        slot = custom_count++;
    }

    custom_table[slot].name = copy;
    axl_memcpy(&custom_table[slot].guid, guid, sizeof(EFI_GUID));
    custom_table[slot].active = true;
    return AXL_OK;
}

int
axl_protocol_find_guid(
    const AxlGuid *guid,
    void         **interface
    )
{
    if (guid == NULL || interface == NULL) {
        return AXL_ERR;
    }
    EFI_STATUS status = axl_bs()->LocateProtocol(
        (EFI_GUID *)guid, NULL, interface);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_protocol_find(
    const char *name,
    void      **interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;

    if (name == NULL || interface == NULL) {
        return AXL_ERR;
    }

    guid = axl_protocol_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    int rc = axl_protocol_find_guid((const AxlGuid *)guid, interface);
    if (rc != AXL_OK) {
        axl_debug("protocol '%s' not found", name);
    }
    return rc;
}

int
axl_protocol_enumerate_guid(
    const AxlGuid  *guid,
    void         ***handles,
    size_t         *count
    )
{
    if (guid == NULL || handles == NULL || count == NULL) {
        return AXL_ERR;
    }

    size_t      buf_size = 0;
    EFI_HANDLE *buf      = NULL;
    EFI_STATUS  status   = axl_bs()->LocateHandleBuffer(
        ByProtocol,
        (EFI_GUID *)guid,
        NULL,
        &buf_size,
        &buf);

    if (EFI_ERROR(status) || buf == NULL) {
        *handles = NULL;
        *count = 0;
        return (status == EFI_NOT_FOUND) ? AXL_OK : AXL_ERR;
    }

    /* LocateHandleBuffer allocates with gBS->AllocatePool, not axl_malloc.
       Copy into an axl_malloc'd buffer so the caller can axl_free() it. */
    if (buf_size > (SIZE_MAX / sizeof(void *))) {
        axl_backend_free(buf);
        *handles = NULL;
        *count = 0;
        return AXL_ERR;
    }
    void **copy = (void **)axl_malloc(buf_size * sizeof(void *));
    if (copy == NULL) {
        axl_backend_free(buf);
        *handles = NULL;
        *count = 0;
        return AXL_ERR;
    }
    for (size_t i = 0; i < buf_size; i++) {
        copy[i] = (void *)buf[i];
    }
    axl_backend_free(buf);

    *handles = copy;
    *count = (size_t)buf_size;
    return AXL_OK;
}

int
axl_protocol_enumerate(
    const char  *name,
    void      ***handles,
    size_t      *count
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;

    if (name == NULL || handles == NULL || count == NULL) {
        return AXL_ERR;
    }

    guid = axl_protocol_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    return axl_protocol_enumerate_guid((const AxlGuid *)guid, handles, count);
}

/* The name-based registry layer over the shared axl_protocol_install /
   _uninstall primitive (which is the single place that talks to the backend
   protocol seam). These add the protocol-NAME lookup; the GUID-based install
   itself lives in axl-driver.h, so there is no GUID-based register_guid /
   unregister_guid here anymore — call axl_protocol_install / _uninstall. */

int
axl_protocol_register(
    const char *name,
    void       *interface,
    void      **handle
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;

    if (name == NULL || interface == NULL || handle == NULL) {
        return AXL_ERR;
    }

    guid = axl_protocol_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    return axl_protocol_install((const AxlGuid *)guid, interface,
                                (AxlHandle *)handle);
}

int
axl_protocol_register_multiple(
    void **handle,
    ...
    )
{
    /* Collect name/interface pairs into arrays (max 8), then install each via
       the shared primitive; roll back on the first failure. (Can't forward
       variadics to InstallMultipleProtocolInterfaces.) */
    va_list ap;
    EFI_GUID   guids[8];
    EFI_GUID   fallbacks[8];
    void      *ifaces[8];
    int        count = 0;

    if (handle == NULL) {
        return AXL_ERR;
    }

    /* Collect name/interface pairs */
    va_start(ap, handle);
    for (;;) {
        /* clang-tidy 18's clang-analyzer-valist.Uninitialized loses
           track of ap inside this loop and reports a false positive.
           Fixed upstream by LLVM PR #156682 (merged 2025-09-08, valist
           checker consolidation); remove the NOLINT once the CI
           runner's clang-tidy is new enough. */
        // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
        const char *name = va_arg(ap, const char *);
        if (name == NULL) {
            break;
        }
        void *iface = va_arg(ap, void *);
        if (iface == NULL || count >= 8) {
            va_end(ap);
            return AXL_ERR;
        }
        const EFI_GUID *g = axl_protocol_lookup_guid(name, &fallbacks[count]);
        if (g == NULL) {
            va_end(ap);
            return AXL_ERR;
        }
        axl_memcpy(&guids[count], g, sizeof(EFI_GUID));
        ifaces[count] = iface;
        count++;
    }
    va_end(ap);

    if (count == 0) {
        return AXL_ERR;
    }

    for (int i = 0; i < count; i++) {
        if (axl_protocol_install((const AxlGuid *)&guids[i], ifaces[i],
                                 (AxlHandle *)handle) != AXL_OK) {
            /* Rollback: uninstall any we already installed (all on *handle). */
            for (int j = 0; j < i; j++) {
                (void)axl_protocol_uninstall(*handle,
                                             (const AxlGuid *)&guids[j],
                                             ifaces[j]);
            }
            return AXL_ERR;
        }
    }

    return AXL_OK;
}

int
axl_protocol_unregister(
    void       *handle,
    const char *name,
    void       *interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;

    if (handle == NULL || name == NULL || interface == NULL) {
        return AXL_ERR;
    }

    guid = axl_protocol_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    return axl_protocol_uninstall((AxlHandle)handle, (const AxlGuid *)guid,
                                  interface);
}
