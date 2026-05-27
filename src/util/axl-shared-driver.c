/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-shared-driver.c
    Convenience layer for the "thin launcher + resident driver"
    pattern — see @c <axl/axl-shared-driver.h> for the consumer API.

    The exported functions are thin wrappers over existing
    primitives:

      - publish   = axl_protocol_register_guid + axl_guid_v5(name);
                    defaults `*out_handle` to the driver's
                    `gImageHandle` so `unload` can resolve the
                    image-bearing handle via LocateHandleBuffer
      - unpublish = axl_protocol_unregister_guid + same GUID
      - locate    = axl_driver_ensure_with_embedded
                  + axl_protocol_find_guid + same GUID
      - unload    = LocateHandleBuffer(ByProtocol, GUID)
                  + axl_driver_unload (which fires the driver's
                  registered unload callback, which calls unpublish)

    The only state owned by this file is the namespace UUID used to
    derive consumer identities. Everything else is composed.
**/

#include "../backend/axl-backend.h"     /* gBS, EFI_HANDLE, ByProtocol */
#include <axl/axl-shared-driver.h>
#include <axl/axl-driver.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("shared-drv");

// ---------------------------------------------------------------------------
// Namespace UUID for v5 derivation. Mirrors AXL_SERVICE_NAMESPACE in
// axl-service.c: a fixed v4-random GUID that seeds the identity space.
// Changing this would break every shared-driver consumer's published
// GUID, so it's effectively frozen.
// {dea63ed1-0d4d-4537-adf1-19cec90744f1} — generated via `uuidgen`.
// ---------------------------------------------------------------------------

static const AxlGuid AXL_SHARED_DRIVER_NAMESPACE = AXL_GUID(
    0xdea63ed1, 0x0d4d, 0x4537,
    0xad, 0xf1, 0x19, 0xce, 0xc9, 0x07, 0x44, 0xf1);

int
axl_shared_driver_guid(
    const char *name,
    AxlGuid    *out
    )
{
    if (name == NULL || out == NULL) {
        return AXL_ERR;
    }
    return axl_guid_v5(&AXL_SHARED_DRIVER_NAMESPACE, name, out);
}

int
axl_shared_driver_publish(
    const char  *name,
    void        *iface,
    AxlHandle   *out_handle
    )
{
    if (name == NULL || iface == NULL || out_handle == NULL) {
        return AXL_ERR;
    }
    AxlGuid guid;
    if (axl_shared_driver_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }
    /* axl_protocol_register_guid takes void** for the handle — null
       in-slot means "create a new handle", non-null means "use this
       one". We default to the driver's own image handle (gImageHandle,
       declared by axl-backend / axl-uefi-extra) so the protocol lives
       on the same handle UnloadImage can act on; that is what makes
       axl_shared_driver_unload able to locate and unload the driver
       by name (LocateHandleBuffer → image handle → UnloadImage).
       Consumers that want a separate handle can pre-set *out_handle
       to a specific value (or to a non-null sentinel like a fresh
       axl_protocol_register_guid output) and this default is
       skipped. */
    if (*out_handle == NULL) {
        *out_handle = (AxlHandle)gImageHandle;
    }
    int rc = axl_protocol_register_guid(&guid, iface, (void **)out_handle);
    if (rc != AXL_OK) {
        axl_warning("axl_shared_driver_publish: install failed for '%s'",
                    name);
    }
    return rc;
}

int
axl_shared_driver_unpublish(
    const char  *name,
    AxlHandle    handle,
    void        *iface
    )
{
    if (name == NULL || handle == NULL || iface == NULL) {
        return AXL_ERR;
    }
    AxlGuid guid;
    if (axl_shared_driver_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = axl_protocol_unregister_guid(handle, &guid, iface);
    if (rc != AXL_OK) {
        axl_warning("axl_shared_driver_unpublish: uninstall failed for '%s'",
                    name);
    }
    return rc;
}

int
axl_shared_driver_unload(
    const char *name
    )
{
    if (name == NULL) {
        return AXL_ERR;
    }
    AxlGuid guid;
    if (axl_shared_driver_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }

    /* Find the image handle that installed the protocol. publish
       defaults to gImageHandle, so there's exactly one handle for
       the GUID — but ask for the buffer form so we can free it
       cleanly even on the edge case of zero (driver not loaded). */
    UINTN          handle_count = 0;
    EFI_HANDLE    *handles      = NULL;
    EFI_STATUS     status       = axl_bs()->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || handle_count == 0 || handles == NULL) {
        /* Not resident — post-condition (driver not loaded) already
           holds. axl_setenv-style return: success when the work was
           a no-op because the desired state is already in effect. */
        return AXL_OK;
    }

    EFI_HANDLE driver_handle = handles[0];
    if (handle_count > 1) {
        /* Defensive: publish defaults to gImageHandle so only one
           handle should carry the protocol. Log and use the first
           handle anyway — the others would be stale state we can't
           safely guess about. */
        axl_warning("axl_shared_driver_unload: '%s' resolved %lu handles, "
                    "expected 1; unloading the first only",
                    name, (unsigned long)handle_count);
    }
    axl_bs()->FreePool(handles);

    int rc = axl_driver_unload((AxlDriverHandle)driver_handle);
    if (rc != AXL_OK) {
        axl_warning("axl_shared_driver_unload: axl_driver_unload "
                    "failed for '%s'", name);
    }
    return rc;
}

int
axl_shared_driver_locate_with_load_options(
    const char           *name,
    const char           *driver_filename,
    const unsigned char  *embed_blob,
    size_t                embed_len,
    const void           *load_options,
    size_t                load_options_size,
    void                **out_iface
    )
{
    if (name == NULL || driver_filename == NULL || out_iface == NULL) {
        return AXL_ERR;
    }
    *out_iface = NULL;

    AxlGuid guid;
    if (axl_shared_driver_guid(name, &guid) != AXL_OK) {
        return AXL_ERR;
    }

    /* ensure-with-embedded handles all four resolution steps:
       LocateProtocol short-circuit → on-disk → embedded blob.
       load_options are installed on the on-disk and embedded paths;
       the resident-driver short-circuit (step 1) leaves the
       previously-installed options intact — consumers that need
       per-invocation args should send them through the vtable
       call, not through LoadOptions. */
    if (axl_driver_ensure_with_embedded(
            &guid, driver_filename,
            embed_blob, embed_len,
            /* override_name */ NULL,
            load_options, load_options_size) != AXL_OK) {
        axl_warning("axl_shared_driver_locate: failed to load '%s'",
                    driver_filename);
        return AXL_ERR;
    }

    /* Defensive: axl_protocol_find_guid is documented to populate
       *out_iface on AXL_OK, but the explicit check keeps a single
       branch from masking a future contract drift. Cheap. */
    if (axl_protocol_find_guid(&guid, out_iface) != AXL_OK
        || *out_iface == NULL) {
        axl_warning("axl_shared_driver_locate: '%s' loaded but "
                    "protocol for '%s' not published",
                    driver_filename, name);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_shared_driver_locate(
    const char           *name,
    const char           *driver_filename,
    const unsigned char  *embed_blob,
    size_t                embed_len,
    void                **out_iface
    )
{
    return axl_shared_driver_locate_with_load_options(
        name, driver_filename, embed_blob, embed_len,
        /* load_options */ NULL, 0,
        out_iface);
}
