/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver-info.h
    Read-only driver / device discovery — the UEFI Shell `drivers`,
    `devices`, and "is this controller bound?" views as an API.

    The companion to @ref axl-driver.h (which *authors* and drives driver
    lifecycle): this header answers "what drivers are present, what are
    they managing, and is a given device bound?" — the recurring
    real-world question "the network driver is on the box but it's not
    bound and I can't find it."

    Built over the firmware's driver model: `EFI_DRIVER_BINDING_PROTOCOL`
    handles for the loaded-driver list, `EFI_COMPONENT_NAME2_PROTOCOL` for
    human names, `OpenProtocolInformation` for who-manages-whom, and
    `EFI_PCI_IO_PROTOCOL.GetLocation` to map a PCI address to its
    controller handle. No EDK2 types cross the API — opaque @ref AxlHandle
    and standard C types only.
**/

#ifndef AXL_DRIVER_INFO_H
#define AXL_DRIVER_INFO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlHandle */
#include <axl/axl-pci.h>   /* AxlPciAddr */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// P1 — the `drivers` view: enumerate loaded DriverBinding drivers
// ---------------------------------------------------------------------------

/**
 * @brief A loaded driver (one EFI_DRIVER_BINDING_PROTOCOL handle).
 */
typedef struct {
    AxlHandle handle;       ///< the driver's DriverBinding handle (for axl_driver_bind / targeting)
    char      name[64];     ///< ComponentName2 driver name (English), or "" if none
    uint32_t  version;      ///< EFI_DRIVER_BINDING_PROTOCOL.Version
    uint32_t  num_devices;  ///< controllers this driver is currently managing (0 = idle)
    bool      is_network;   ///< true if it currently manages a network controller (see note)
} AxlDriverInfo;

/**
 * @brief List every loaded driver (DriverBinding handle) on the system.
 *
 * The UEFI Shell `drivers` command as an API: walks all
 * `EFI_DRIVER_BINDING_PROTOCOL` handles, reads each driver's
 * ComponentName2 name and version, and counts the controllers it is
 * currently managing (via `OpenProtocolInformation` BY_DRIVER). Lets a
 * caller answer "what drivers are present, and which are idle vs.
 * managing devices?"
 *
 * @note @c is_network reflects whether the driver is managing a network
 *     controller *right now* (one whose handle exposes SimpleNetwork /
 *     NII / ManagedNetwork / IP4). An idle network driver (managing
 *     nothing) cannot be classified without invoking its Supported()
 *     against each controller, so it reports @c is_network == false; pair
 *     with @c name and @c num_devices to find idle candidates.
 *
 * @return AXL_OK on success (@p count set; entries up to @p cap written —
 *     @p count may exceed @p cap, signalling truncation). AXL_ERR on NULL
 *     @p count or firmware enumeration failure.
 */
int
axl_driver_list_loaded(
    AxlDriverInfo *out,    ///< [out] caller array (may be NULL to just count)
    size_t         cap,    ///< capacity of @p out in entries
    size_t        *count   ///< [out] total drivers found (may exceed cap)
);

// ---------------------------------------------------------------------------
// P1a — name a handle (ComponentName2 + device-path fallback)
// ---------------------------------------------------------------------------

/**
 * @brief Best-effort human name for a handle.
 *
 * If @p h carries an `EFI_COMPONENT_NAME2_PROTOCOL` (i.e. it is a driver),
 * returns its English driver name. Otherwise falls back to the handle's
 * device-path text. Always NUL-terminates @p out.
 *
 * @return AXL_OK if a name was written, AXL_ERR on NULL args or if neither
 *     a component name nor a device path is available.
 */
int
axl_handle_name(
    AxlHandle  h,     ///< handle to name (non-NULL)
    char      *out,   ///< [out] UTF-8 name buffer
    size_t     cap    ///< capacity of @p out in bytes
);

// ---------------------------------------------------------------------------
// P2 / P5 — PCI address <-> controller handle, and bound-driver query
// ---------------------------------------------------------------------------

/**
 * @brief Resolve a PCI function to its controller handle.
 *
 * Matches @p addr against every `EFI_PCI_IO_PROTOCOL` handle via
 * `GetLocation` (segment/bus/device/function). The returned handle is the
 * controller you pass to @ref axl_driver_bind or
 * @ref axl_driver_disconnect_handle, and that
 * @ref axl_pci_driver_bound queries.
 *
 * @return AXL_OK on a unique match (@p out set); AXL_ERR if no PCI_IO
 *     handle reports that address (e.g. the function exists in config
 *     space but no driver published PciIo on it) or on NULL @p out.
 */
int
axl_pci_to_handle(
    AxlPciAddr  addr,   ///< segment:bus:dev:func to resolve
    AxlHandle  *out     ///< [out] controller handle
);

/**
 * @brief Is a driver bound to this PCI function, and which one?
 *
 * The direct answer to "is this NIC managed?" — replaces dev/func
 * string-matching heuristics. Resolves @p addr to its controller handle,
 * then checks whether any agent holds a protocol on it BY_DRIVER; if so,
 * reports the managing driver's ComponentName2 name.
 *
 * @return AXL_OK on success (@p bound set; @p drv_name filled when bound
 *     and a name is available, else ""). AXL_ERR on NULL @p bound or if
 *     @p addr has no PCI_IO controller handle (treat as "not bindable
 *     here" — distinct from bound==false on a present-but-unbound device).
 */
int
axl_pci_driver_bound(
    AxlPciAddr  addr,       ///< segment:bus:dev:func to query
    bool       *bound,      ///< [out] true if a driver manages the controller
    char       *drv_name,   ///< [out] managing driver name (NULL allowed)
    size_t      drv_cap     ///< capacity of @p drv_name in bytes
);

// ---------------------------------------------------------------------------
// P3 — bind a specific driver to a specific controller
// ---------------------------------------------------------------------------

/**
 * @brief Connect a controller, optionally with one specific driver.
 *
 * Targeted `ConnectController`. With @p driver == NULL this is the same as
 * @ref axl_driver_connect_handle (let any applicable driver bind). With
 * @p driver set (a DriverBinding handle, e.g. `AxlDriverInfo.handle` or a
 * handle from @ref axl_driver_load) the firmware is asked to bind *that*
 * driver to @p controller — "use this exact driver on this exact NIC."
 *
 * @return AXL_OK if a driver bound the controller (or it was already
 *     started); AXL_ERR on NULL @p controller or if no driver could bind.
 */
int
axl_driver_bind(
    AxlHandle controller,   ///< controller handle (e.g. from axl_pci_to_handle)
    AxlHandle driver        ///< driver-image/DriverBinding handle, or NULL for any
);

// ---------------------------------------------------------------------------
// P1b / P1c — generic handle + protocol enumeration (the Devices tab and the
// per-NIC protocol-stack view).
//
// All four follow the same fixed-buffer truncation contract as
// axl_driver_list_loaded: pass @p out == NULL to count only, and @p count is
// always set to the FULL total even when it exceeds @p cap (so @p count > @p cap
// signals truncation; only @p cap entries were written).
// ---------------------------------------------------------------------------

/**
 * @brief Enumerate handles — every handle, or only those exposing a protocol.
 *
 * The `LocateHandleBuffer` core as an API. With @p protocol == NULL this is
 * the firmware's whole handle database (the shell `dh` list). With @p protocol
 * set it is the by-GUID subset — every handle that publishes that protocol
 * (the GUID-keyed, fixed-buffer counterpart to @ref axl_protocol_enumerate,
 * which is name-keyed and heap-allocates).
 *
 * @return AXL_OK on success (@p count set; up to @p cap entries written —
 *     @p count may exceed @p cap, signalling truncation). An empty database /
 *     no matching handle is AXL_OK with @p count == 0. AXL_ERR on NULL
 *     @p count or firmware enumeration failure.
 */
int
axl_handle_list(
    const AxlGuid *protocol,  ///< protocol GUID to filter by, or NULL for all handles
    AxlHandle     *out,       ///< [out] caller array (may be NULL to just count)
    size_t         cap,       ///< capacity of @p out in entries
    size_t        *count      ///< [out] total handles found (may exceed cap)
);

/**
 * @brief List the GUIDs of every protocol installed on a handle.
 *
 * `ProtocolsPerHandle` as an API — the reverse of @ref axl_protocol_enumerate
 * (which goes protocol → handles). Given a handle, returns the GUIDs of the
 * protocols *it* exposes. This is the core of the protocol-stack view: resolve
 * a NIC to its handle(s), list their protocol GUIDs, and name them with
 * @ref axl_net_protocol_name.
 *
 * @return AXL_OK on success (@p count set; up to @p cap GUIDs written —
 *     @p count may exceed @p cap, signalling truncation). AXL_ERR on NULL
 *     @p h / NULL @p count or firmware failure.
 */
int
axl_handle_protocols(
    AxlHandle  h,       ///< handle to inspect (non-NULL)
    AxlGuid   *out,     ///< [out] caller array of GUIDs (may be NULL to just count)
    size_t     cap,     ///< capacity of @p out in entries
    size_t    *count    ///< [out] total protocols on @p h (may exceed cap)
);

/**
 * @brief Short name for a well-known networking-stack protocol GUID.
 *
 * Maps the GUID of an EFI network protocol to a stable short label so the
 * stack reads "Ip4Config2" rather than a raw GUID. Recognised: SimpleNetwork,
 * ManagedNetwork, Arp, Ip4, Ip4Config2, Dhcp4, Tcp4, Udp4, Dns4, Mtftp4, and
 * the IPv6 peers (Ip6, Dhcp6, Tcp6, Udp6, Dns6). Always NUL-terminates @p out
 * on AXL_OK.
 *
 * @return AXL_OK with the name written for a recognised GUID; AXL_ERR for an
 *     unknown GUID (the caller formats the raw GUID itself) or NULL args.
 */
int
axl_net_protocol_name(
    const AxlGuid *guid,   ///< protocol GUID to name (non-NULL)
    char          *out,    ///< [out] short-name buffer
    size_t         cap     ///< capacity of @p out in bytes
);

/**
 * @brief The driver(s) currently managing a controller.
 *
 * Walks @p controller's protocols and collects the distinct agent handles
 * that hold any of them BY_DRIVER (`OpenProtocolInformation`). This is the
 * Devices-tab "managing driver(s)" column and the "unmanaged?" test: a
 * @p count of 0 means no driver is bound. Name each returned handle with
 * @ref axl_handle_name. Usually one driver; the array form handles the rare
 * multi-manager case.
 *
 * @return AXL_OK on success (@p count set; up to @p cap handles written —
 *     @p count may exceed @p cap; @p count == 0 means unmanaged). AXL_ERR on
 *     NULL @p controller / NULL @p count or firmware failure.
 */
int
axl_handle_drivers(
    AxlHandle  controller,   ///< controller handle (non-NULL)
    AxlHandle *drivers,      ///< [out] caller array (may be NULL to just count)
    size_t     cap,          ///< capacity of @p drivers in entries
    size_t    *count         ///< [out] managing drivers (may exceed cap; 0 = unmanaged)
);

/**
 * @brief The child controllers a controller has produced.
 *
 * Walks @p controller's protocols and collects the distinct controller handles
 * that opened any of them BY_CHILD_CONTROLLER (`OpenProtocolInformation`) — the
 * child handles a bus/stack driver created beneath @p controller. The UEFI
 * networking stack is installed on such child handles (the service-binding
 * children: ManagedNetwork → Arp → Ip4 → Ip4Config2 → Dhcp4 → Tcp4/Udp4 → Dns4),
 * so a full per-NIC stack view walks from the NIC controller into its children
 * and lists each child's protocols. Also the `devtree` descent primitive.
 *
 * @return AXL_OK on success (@p count set; up to @p cap handles written —
 *     @p count may exceed @p cap; @p count == 0 means no children). AXL_ERR on
 *     NULL @p controller / NULL @p count or firmware failure.
 */
int
axl_handle_children(
    AxlHandle  controller,   ///< controller handle (non-NULL)
    AxlHandle *out,          ///< [out] caller array (may be NULL to just count)
    size_t     cap,          ///< capacity of @p out in entries
    size_t    *count         ///< [out] child controllers (may exceed cap; 0 = none)
);

/**
 * @brief Short name for a well-known UEFI protocol GUID (any kind).
 *
 * The Devices-tab counterpart to @ref axl_net_protocol_name(): a broader table
 * covering the common device / driver / bus / console protocols — DevicePath,
 * LoadedImage, DriverBinding, ComponentName2, SimpleFileSystem, BlockIo,
 * DiskIo, PciIo, GraphicsOutput, SerialIo, UsbIo, NvmExpressPassThru,
 * AtaPassThru, and more — plus everything @ref axl_net_protocol_name knows
 * (the networking stack, which it consults first). Always NUL-terminates
 * @p out on AXL_OK.
 *
 * @return AXL_OK with the name written for a recognised GUID; AXL_ERR for an
 *     unknown GUID (the caller formats the raw GUID itself) or NULL args.
 */
int
axl_protocol_guid_name(
    const AxlGuid *guid,   ///< protocol GUID to name (non-NULL)
    char          *out,    ///< [out] short-name buffer
    size_t         cap     ///< capacity of @p out in bytes
);

/**
 * @brief The parent controller(s) of a controller.
 *
 * The inverse of @ref axl_handle_children(): scans the handle database for every
 * controller that opened one of its protocols BY_CHILD_CONTROLLER with
 * @p controller as the child. Together with @ref axl_handle_children this is
 * the `devtree` parent/child walk. A @p count of 0 means @p controller is a
 * root (no parent produced it). Same fixed-buffer truncation contract as the
 * other enumerators.
 *
 * @note Walks the whole handle database (O(handles)); intended for an
 *     interactive `devtree` view, not a hot path.
 *
 * @return AXL_OK on success (@p count set; up to @p cap handles written —
 *     @p count may exceed @p cap; @p count == 0 means no parent). AXL_ERR on
 *     NULL @p controller / NULL @p count or firmware failure.
 */
int
axl_handle_parents(
    AxlHandle  controller,   ///< child controller handle (non-NULL)
    AxlHandle *out,          ///< [out] caller array (may be NULL to just count)
    size_t     cap,          ///< capacity of @p out in entries
    size_t    *count         ///< [out] parent controllers (may exceed cap; 0 = root)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DRIVER_INFO_H */
