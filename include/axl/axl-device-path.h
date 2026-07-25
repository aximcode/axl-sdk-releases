/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-device-path.h:
 *
 * Surfaces the small handful of UEFI-device-path primitives that
 * consumers building publishers (filesystem, block-io, vendor
 * protocols) need to construct fresh device-path chains, without
 * having to `#include <uefi/...>`.
 *
 * Today this header only ships the vendor-path constructor —
 * historically the one chunk of `EFI_DEVICE_PATH_PROTOCOL` plumbing
 * every consumer hand-rolled. Future additions (PCI-path,
 * file-path, fan-out helpers) live here.
 */

#ifndef AXL_DEVICE_PATH_H
#define AXL_DEVICE_PATH_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>      /* AxlGuid */

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to an EFI_DEVICE_PATH_PROTOCOL chain. The pointer
/// returned by the constructors below is suitable to pass to
/// `axl_protocol_register("device-path", ...)` or to
/// `axl_protocol_install(&EFI_DEVICE_PATH_PROTOCOL_GUID, ...)`.
typedef struct AxlDevicePath AxlDevicePath;

/**
 * @brief Allocate a vendor device-path node + END terminator.
 *
 * Builds the two-node chain that vendor-defined protocols (axl-webfs,
 * crashhandler reports, RAM-disk publishers, ...) install on the
 * handle they create:
 *
 * ```
 *   [HARDWARE_DEVICE_PATH / HW_VENDOR_DP]   ← carries vendor_guid
 *   [END_DEVICE_PATH_TYPE / END_ENTIRE]     ← terminator
 * ```
 *
 * The two nodes are laid out in a single contiguous allocation;
 * caller frees with `axl_free(*out)`.
 *
 * @return AXL_OK on success; AXL_ERR if @p vendor_guid or @p out
 *     is NULL or allocation fails.
 */
AXL_WARN_UNUSED int
axl_device_path_new_vendor(
    const AxlGuid  *vendor_guid,  ///< identifies the vendor / instance
    AxlDevicePath **out           ///< [out] freshly-allocated chain
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DEVICE_PATH_H */
