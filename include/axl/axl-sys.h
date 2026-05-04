/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-sys.h:
 *
 * System operations — reset, device mapping refresh.
 * UEFI-specific, no GLib equivalent.
 */

#ifndef AXL_SYS_H
#define AXL_SYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// GUID
// ---------------------------------------------------------------------------

/**
 * @brief UEFI-compatible GUID in standard C types.
 *
 * Binary-compatible with EFI_GUID. Use in public API so consumer
 * apps don't need `<uefi/axl-uefi.h>` for GUID operations.
 */
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} AxlGuid;

/**
 * @brief Compare two GUIDs for equality.
 *
 * @return true if equal.
 */
static inline bool
axl_guid_cmp(
    const AxlGuid *a,
    const AxlGuid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof(AxlGuid); i++) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Initialize an AxlGuid from literal values.
 *
 * Usage: AxlGuid g = AXL_GUID(0x12345678, 0xABCD, 0xEF01,
 *                              0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01);
 */
#define AXL_GUID(d1, d2, d3, d4_0, d4_1, d4_2, d4_3, d4_4, d4_5, d4_6, d4_7) \
    { (d1), (d2), (d3), { (d4_0), (d4_1), (d4_2), (d4_3), (d4_4), (d4_5), (d4_6), (d4_7) } }

// ---------------------------------------------------------------------------
// Device path
// ---------------------------------------------------------------------------

/**
 * @brief Check if a device path contains a vendor node with the given GUID.
 *
 * Walks the device path node chain looking for a hardware vendor node
 * (type 0x01, subtype 0x04) whose GUID matches @p guid.
 *
 * @return true if a matching vendor node is found.
 */
bool
axl_device_path_has_vendor(
    void          *device_path,  ///< device path (from "device-path" service)
    const AxlGuid *guid          ///< vendor GUID to match
);

/**
 * @brief Per-node callback for @ref axl_device_path_for_each.
 *
 * Return 0 to continue iteration, any non-zero value to stop —
 * the return value is propagated back from `axl_device_path_for_each`
 * so callbacks can use it as a found-flag, error code, or count.
 *
 * @p node points at the full device-path node (4-byte header
 * followed by payload); cast it to the corresponding spec struct
 * (e.g. `VENDOR_DEVICE_PATH *`) once @p type and @p subtype have
 * confirmed the shape.
 */
typedef int (*AxlDevicePathFn)(
    uint8_t      type,
    uint8_t      subtype,
    const void  *node,
    void        *user
);

/**
 * @brief Walk a device-path node chain with bounded-step safety.
 *
 * Iterates from @p device_path through the END node, calling @p fn
 * on each node with its `(type, subtype, node)` triple. Stops early
 * when @p fn returns non-zero (and propagates that value), or when
 * a malformed node is hit (length < 4 or the chain doesn't terminate
 * within an internal step cap).
 *
 * Replaces hand-rolled `while (!EFI_DP_IS_END(node)) ...` loops —
 * those used to differ on whether they bounded the walk, leaving
 * malformed firmware data able to runaway.
 *
 * @return 0 on a clean traversal to END, the callback's non-zero
 *     return value if it stopped early, or -1 on malformed input.
 */
int
axl_device_path_for_each(
    const void       *device_path,  ///< device path (from "device-path" service)
    AxlDevicePathFn   fn,            ///< per-node callback
    void             *user           ///< opaque user pointer for the callback
);

/**
 * @brief Find the first device-path node matching (type, subtype).
 *
 * @return pointer to the node (cast to the corresponding spec
 *     struct by the caller), or NULL if no match.
 */
const void *
axl_device_path_find(
    const void *device_path,  ///< device path (from "device-path" service)
    uint8_t     type,         ///< node type to match
    uint8_t     subtype       ///< node subtype to match
);

/**
 * @brief Compute the total byte length of a device path INCLUDING
 *        the END node.
 *
 * Useful when copying / appending device paths, e.g. when building
 * a LoadImage argument out of an existing volume DP plus a file
 * suffix. Bounded by the same step cap as the iterator.
 *
 * @return size in bytes, or 0 on malformed input.
 */
size_t
axl_device_path_size(
    const void *device_path  ///< device path (from "device-path" service)
);

/**
 * @brief Render a device path as the firmware's canonical text form.
 *
 * Wraps the EFI_DEVICE_PATH_TO_TEXT_PROTOCOL the firmware exposes
 * (`ConvertDevicePathToText`) and converts the resulting UCS-2
 * string to UTF-8. The output is the same format `dh -d` and
 * `bcfg boot dump` produce — e.g.
 * `PciRoot(0x0)/Pci(0x3,0x0)/MAC(525400123456,0x1)`.
 *
 * Returns NULL when the firmware doesn't expose
 * EFI_DEVICE_PATH_TO_TEXT_PROTOCOL (some vintage UEFI 2.0 builds
 * omit it) or when @p device_path is NULL.
 *
 * @return UTF-8 string allocated with `axl_malloc`, or NULL on
 *     failure. Caller frees with `axl_free`.
 */
char *
axl_device_path_to_text(
    const void *device_path  ///< device path (from "device-path" service)
);

// ---------------------------------------------------------------------------
// System control
// ---------------------------------------------------------------------------

#define AXL_RESET_COLD      0  ///< cold reset (full power cycle)
#define AXL_RESET_WARM      1  ///< warm reset (CPU reset, memory preserved)
#define AXL_RESET_SHUTDOWN  2  ///< power off

/**
 * @brief Reset or shut down the system.
 *
 * Does not return on success.
 */
void
axl_reset(
    int type  ///< AXL_RESET_COLD, AXL_RESET_WARM, or AXL_RESET_SHUTDOWN
);

/**
 * @brief Rescan device-to-filesystem mappings.
 *
 * Equivalent to the Shell "map -r" command. Call after hot-plugging
 * a USB drive or after a driver installs a new filesystem.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_map_refresh(void);

// ---------------------------------------------------------------------------
// System information
// ---------------------------------------------------------------------------

/**
 * @brief Firmware information.
 */
typedef struct {
    char      vendor[64];          ///< firmware vendor name (UTF-8)
    uint32_t  firmware_revision;   ///< vendor firmware revision
    uint16_t  spec_major;          ///< UEFI spec major version
    uint16_t  spec_minor;          ///< UEFI spec minor version
} AxlFirmwareInfo;

/**
 * @brief Get firmware information (vendor, revision, spec version).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_sys_get_firmware_info(
    AxlFirmwareInfo *info  ///< [out] receives firmware info
);

/**
 * @brief Get total usable memory size in bytes.
 *
 * Queries the firmware memory map and sums all usable regions.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_sys_get_memory_size(
    uint64_t *total_bytes  ///< [out] receives total usable RAM
);

/**
 * @brief Get a service interface from a specific handle.
 *
 * @return AXL_OK on success, AXL_ERR if not found.
 */
int
axl_handle_get_service(
    void        *handle,     ///< handle from axl_service_enumerate
    const char  *name,       ///< service name (e.g., "device-path", "simple-fs")
    void       **interface   ///< [out] service interface pointer
);

// ---------------------------------------------------------------------------
// Service registry
// ---------------------------------------------------------------------------

/**
 * @brief Find a system service by name.
 *
 * Looks up a named service in the platform service registry.
 * Well-known names: "smbios", "shell", "simple-network", "simple-fs".
 *
 * @return AXL_OK on success, AXL_ERR if not found.
 */
int
axl_service_find(
    const char *name,       ///< service name
    void      **interface   ///< [out] service interface pointer
);

/**
 * @brief Enumerate all handles providing a named service.
 *
 * Caller frees the returned handles array with axl_free().
 *
 * @return AXL_OK on success (count may be 0), AXL_ERR on error.
 */
int
axl_service_enumerate(
    const char  *name,      ///< service name
    void      ***handles,   ///< [out] array of handles
    size_t      *count      ///< [out] number of handles
);

/**
 * @brief Register a service on a handle.
 *
 * Creates a new handle if @a *handle is NULL.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_service_register(
    const char *name,       ///< service name
    void       *interface,  ///< service interface to install
    void      **handle      ///< [in/out] handle (NULL to create new)
);

/**
 * @brief Unregister a service from a handle.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_service_unregister(
    void       *handle,     ///< handle from axl_service_register
    const char *name,       ///< service name
    void       *interface   ///< interface to remove
);

/**
 * @brief Register multiple services on a handle atomically.
 *
 * Installs one or more services on the same handle in one operation.
 * If any fails, none are installed. Creates a new handle if
 * @a *handle is NULL. Pass name/interface pairs followed by NULL:
 *
 * @code
 * void *h = NULL;
 * axl_service_register_multiple(&h,
 *     "simple-fs", &my_fs,
 *     "device-path", &my_dp,
 *     NULL);
 * @endcode
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_service_register_multiple(
    void      **handle,  ///< [in/out] handle (NULL to create new)
    ...                  ///< name, interface pairs, terminated by NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SYS_H */
