/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-shared-driver.c
    Convenience layer for the "thin launcher + resident driver"
    pattern — see @c <axl/axl-shared-driver.h> for the consumer API.

    The three exported functions are thin wrappers over existing
    primitives:

      - publish   = axl_protocol_register_guid + axl_guid_v5(name)
      - unpublish = axl_protocol_unregister_guid + same GUID
      - locate    = axl_driver_ensure_with_embedded
                  + axl_protocol_find_guid + same GUID

    The only state owned by this file is the namespace UUID used to
    derive consumer identities. Everything else is composed.
**/

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
       in-slot means "create new handle", non-null means "reuse".
       We pass *out_handle through unchanged so consumers that want
       to pin their own handle can do so by pre-setting *out_handle. */
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
