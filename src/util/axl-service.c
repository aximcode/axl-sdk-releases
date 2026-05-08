/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-service.c
    Service registry — platform-agnostic service discovery and registration.
**/

#include <stdarg.h>
#include "../backend/axl-backend.h"
#include "axl-service-internal.h"
#include <axl/axl-sys.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("service");

// ---------------------------------------------------------------------------
// Name → GUID mapping
// ---------------------------------------------------------------------------

/* axl_service_lookup_guid prototype lives in axl-service-internal.h
   so axl-sys.c can call it without the prior extern-at-point-of-use
   workaround. */

typedef struct {
    const char     *name;
    const EFI_GUID *guid;
} ServiceEntry;

/* TCG2 (TPM 2.0) protocol GUID — not in generated headers */
static const EFI_GUID TCG2_PROTOCOL_GUID = {
    0x607f766c, 0x7455, 0x42be,
    { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f }
};

static const ServiceEntry service_table[] = {
    { "smbios",         &EFI_SMBIOS_PROTOCOL_GUID },
    { "tcg2",           &TCG2_PROTOCOL_GUID },
    { "shell",          &gEfiShellProtocolGuid },
    { "shell-params",   &gEfiShellParametersProtocolGuid },
    { "simple-network", &EFI_SIMPLE_NETWORK_PROTOCOL_GUID },
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
 * Simple hash-based GUID for custom/unknown service names.
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
axl_service_lookup_guid(
    const char *name,
    EFI_GUID   *fallback
    )
{
    if (name == NULL) {
        return NULL;
    }

    for (const ServiceEntry *e = service_table; e->name != NULL; e++) {
        if (axl_strcmp(name, e->name) == 0) {
            return e->guid;
        }
    }

    /* Custom name — generate deterministic GUID */
    *fallback = guid_from_name(name);
    return fallback;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_service_find(
    const char *name,
    void      **interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;

    if (name == NULL || interface == NULL) {
        return AXL_ERR;
    }

    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->LocateProtocol(
        (EFI_GUID *)guid, NULL, interface);

    if (EFI_ERROR(status)) {
        axl_debug("service '%s' not found", name);
    }

    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_service_enumerate(
    const char  *name,
    void      ***handles,
    size_t      *count
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;
    size_t          buf_size = 0;
    EFI_HANDLE     *buf = NULL;

    if (name == NULL || handles == NULL || count == NULL) {
        return AXL_ERR;
    }

    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->LocateHandleBuffer(
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
axl_service_register(
    const char *name,
    void       *interface,
    void      **handle
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;

    if (name == NULL || interface == NULL || handle == NULL) {
        return AXL_ERR;
    }

    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->InstallProtocolInterface(
        (EFI_HANDLE *)handle,
        (EFI_GUID *)guid,
        EFI_NATIVE_INTERFACE,
        interface);

    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_service_register_multiple(
    void **handle,
    ...
    )
{
    /* Can't forward variadics to InstallMultipleProtocolInterfaces,
       so collect pairs into arrays (max 8) and call individually.
       Use a two-pass approach: first install all, rollback on failure. */
    va_list ap;
    EFI_GUID   guids[8];
    EFI_GUID   fallbacks[8];
    void      *ifaces[8];
    int        count = 0;
    EFI_STATUS status;

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
        const EFI_GUID *g = axl_service_lookup_guid(name, &fallbacks[count]);
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

    /* Install all protocols. Use individual InstallProtocolInterface calls
       since we can't forward to the variadic Multi version. */
    for (int i = 0; i < count; i++) {
        status = axl_bs()->InstallProtocolInterface(
            (EFI_HANDLE *)handle,
            &guids[i],
            EFI_NATIVE_INTERFACE,
            ifaces[i]);
        if (EFI_ERROR(status)) {
            /* Rollback: uninstall any we already installed */
            for (int j = 0; j < i; j++) {
                axl_bs()->UninstallProtocolInterface(
                    *((EFI_HANDLE *)handle), &guids[j], ifaces[j]);
            }
            return AXL_ERR;
        }
    }

    return AXL_OK;
}

int
axl_service_unregister(
    void       *handle,
    const char *name,
    void       *interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;

    if (handle == NULL || name == NULL || interface == NULL) {
        return AXL_ERR;
    }

    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->UninstallProtocolInterface(
        (EFI_HANDLE)handle,
        (EFI_GUID *)guid,
        interface);

    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}
