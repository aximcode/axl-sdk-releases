/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-usb.h
    USB device enumeration via `EFI_USB_IO_PROTOCOL` handles.

    The UEFI USB stack exposes one `EFI_USB_IO_PROTOCOL` handle per
    *interface* of every enumerated device — multiple handles therefore
    refer to the same physical device when it has more than one
    function (e.g. a composite keyboard+mouse, or a USB-net adapter
    with separate control / data interfaces). AxlUsb derives stable
    `(bus, addr, intf)` ordinals from each handle's device path so
    consumers can present a Linux-lsusb-shaped view: one row per
    interface, deduplicated and ordered by bus/device/interface.

    Cursor-style iteration mirrors axl_pci_next:

    @code
    AxlUsbAddr *u = NULL;
    while ((u = axl_usb_next(u)) != NULL) {
        uint16_t vid, pid;
        if (axl_usb_get_vid_pid(*u, &vid, &pid) == 0) {
            // ...
        }
    }
    @endcode

    Phase A: enumeration + vendor/product ID readout.
    Phase B: interface class triplet (class / subclass / protocol)
    decode via the same compiled-in tables AxlPci uses for PCI
    classes.
    Phase C: string descriptor reads — manufacturer / product /
    serial via @c UsbGetStringDescriptor + UCS-2 → UTF-8.
    Phase D: vendor/device-name database via @c usb-ids.json5
    sidecar (handle + singleton API mirroring AxlPciIds). See
    `docs/AXL-Usb-Handoff.md`.
**/

#ifndef AXL_USB_H
#define AXL_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#include <axl/axl-sidecar.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Address tuple
// ---------------------------------------------------------------------------

/**
 * @brief A USB controller address (bus / device / interface).
 *
 * `bus` is a 1-based ordinal of the host controller (xHCI/EHCI/UHCI/
 * OHCI) the device is behind, in EFI device-path order. `addr` is a
 * 1-based ordinal of the physical device within that bus, deduplicated
 * across interfaces. `intf` is the real interface number returned by
 * `UsbGetInterfaceDescriptor` — multiple `(bus, addr, *)` entries with
 * different `intf` values refer to interfaces of the same physical
 * device.
 *
 * Ordinals are stable within a single boot but not across boots — the
 * underlying EFI handle order can change. Consumers that need a
 * persistent identity should hash the device descriptor (vid:pid:serial)
 * instead.
 */
typedef struct {
    uint8_t  bus;    ///< host controller ordinal (1-based)
    uint8_t  addr;   ///< device ordinal within bus (1-based)
    uint8_t  intf;   ///< interface number from interface descriptor
} AxlUsbAddr;

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

/**
 * @brief Iterate every USB interface exposed by `EFI_USB_IO_PROTOCOL`.
 *
 * Returns a pointer to a static internal cursor; the storage is
 * reused across calls and is invalidated by the next call. Pass NULL
 * to start the walk fresh, or the previous non-NULL return value to
 * advance — passing any other pointer (including a caller-allocated
 * AxlUsbAddr) restarts iteration silently. The caller never owns the
 * cursor's storage.
 *
 * Iteration order is `(bus, addr, intf)` ascending. Returns NULL when
 * the walk is complete or no `EFI_USB_IO_PROTOCOL` handles are
 * installed (the platform has no USB stack, or the firmware did not
 * enumerate any USB devices).
 *
 * @return pointer to the next interface, or NULL when enumeration is
 *     complete (or the USB stack is unavailable).
 */
AxlUsbAddr *
axl_usb_next(
    AxlUsbAddr  *prev   ///< previous result, or NULL to start
);

// ---------------------------------------------------------------------------
// Per-device introspection
// ---------------------------------------------------------------------------

/**
 * @brief Read vendor ID and product ID from a USB device descriptor.
 *
 * Reads the `idVendor` / `idProduct` fields of the standard 18-byte
 * device descriptor via `EFI_USB_IO_PROTOCOL.UsbGetDeviceDescriptor`.
 * All interfaces of one physical device share the same `(vid, pid)`
 * — the per-interface granularity of `AxlUsbAddr` does not change the
 * descriptor that backs this call.
 *
 * @return AXL_OK on success (both fields populated), AXL_ERR if @p addr is not
 *     a known interface or the firmware fails the descriptor read.
 */
int
axl_usb_get_vid_pid(
    AxlUsbAddr   addr,   ///< target interface
    uint16_t    *vid,    ///< [out] vendor ID
    uint16_t    *pid     ///< [out] product ID
);

/**
 * @brief Read the interface class triplet for a USB interface.
 *
 * Reads `bInterfaceClass` / `bInterfaceSubClass` / `bInterfaceProtocol`
 * from the interface descriptor via
 * `EFI_USB_IO_PROTOCOL.UsbGetInterfaceDescriptor`. Composite devices
 * (DeviceDescriptor.bDeviceClass = 0) drive their class identity per
 * interface — the right granularity to query for tools that present
 * one row per interface (Linux lsusb -v shape).
 *
 * Out parameters may be individually NULL if the caller doesn't need
 * that field.
 *
 * @return AXL_OK on success, AXL_ERR if @p addr is not a known interface or the
 *     firmware fails the descriptor read.
 */
int
axl_usb_get_class(
    AxlUsbAddr   addr,   ///< target interface
    uint8_t     *class_,  ///< [out, optional] base class (e.g. 0x03 for HID)
    uint8_t     *sub,    ///< [out, optional] subclass
    uint8_t     *prot    ///< [out, optional] protocol
);

/**
 * @brief Curated device-level fields not covered by the per-interface
 *     accessors.
 *
 * A projection of the standard device descriptor (one per physical
 * device, shared by all its interfaces) — the fields an lsusb-style
 * consumer wants beyond vendor/product ID (which `axl_usb_get_vid_pid`
 * already covers). Not the full 18-byte wire structure.
 */
typedef struct {
    uint16_t  bcd_usb;             ///< USB spec version, BCD (0x0200 = "2.0", 0x0110 = "1.1")
    uint8_t   device_class;        ///< bDeviceClass; 0 on a composite device (class is per-interface)
    uint8_t   device_sub_class;    ///< bDeviceSubClass
    uint8_t   device_protocol;     ///< bDeviceProtocol
    uint8_t   num_configurations;  ///< bNumConfigurations
} AxlUsbDeviceInfo;

/**
 * @brief Read curated device-level descriptor fields for a USB device.
 *
 * Reads the device descriptor via
 * `EFI_USB_IO_PROTOCOL.UsbGetDeviceDescriptor` and copies out the
 * fields in @ref AxlUsbDeviceInfo. All interfaces of one physical
 * device share these values (the descriptor is per-device, not
 * per-interface).
 *
 * Fields are returned **raw**. In particular `device_class` is 0 on a
 * composite device, where the class identity lives per interface — a
 * consumer that wants a single "device class" should fall back to
 * `axl_usb_get_class` when `device_class == 0`. Likewise `bcd_usb` is
 * the raw BCD value; format it yourself (e.g. high byte `.` low byte as
 * hex: 0x0200 -> "2.0", 0x0110 -> "1.1"). The library returns
 * mechanism, not policy.
 *
 * @return AXL_OK on success (@p out fully populated), AXL_ERR if @p out
 *     is NULL, @p addr is not a known interface, or the firmware fails
 *     the descriptor read.
 */
int
axl_usb_get_device_info(
    AxlUsbAddr         addr,   ///< target interface (any interface of the device)
    AxlUsbDeviceInfo  *out     ///< [out] populated on success
);

/**
 * @brief Read the endpoint count of a USB interface.
 *
 * Reads `bNumEndpoints` from the interface descriptor via
 * `EFI_USB_IO_PROTOCOL.UsbGetInterfaceDescriptor`. This is the count of
 * non-control endpoints the interface declares (the control endpoint 0
 * is implicit and not included, per USB 2.0 §9.6.5).
 *
 * @return AXL_OK on success, AXL_ERR if @p out is NULL, @p addr is not a
 *     known interface, or the firmware fails the descriptor read.
 */
int
axl_usb_get_num_endpoints(
    AxlUsbAddr   addr,   ///< target interface
    uint8_t     *out     ///< [out] bNumEndpoints
);

/// Maximum hub depth the topology walker descends, and the basis for
/// the port-path buffer cap below. USB 2.0/3.x caps the bus depth at 5
/// hubs between root and device (= 6 USB device-path nodes including the
/// leaf), so 8 levels is generous headroom against malformed firmware
/// paths. (Also used by axl_usb_tree_for_each, further down.)
#define AXL_USB_TREE_MAX_DEPTH  8u

/// Buffer cap for a port-path string: up to AXL_USB_TREE_MAX_DEPTH port
/// numbers (3 digits each) joined by '.' separators, plus NUL. Sized
/// 4 bytes/port (digit-triple + separator) + 1, one byte generous since
/// the last port has no trailing separator.
#define AXL_USB_PORT_PATH_MAX  ((AXL_USB_TREE_MAX_DEPTH * 4u) + 1u)

/**
 * @brief Read a USB device's hub-port location.
 *
 * Surfaces the port chain AxlUsb already parses from the device path
 * (the same chain `axl_usb_tree_for_each` walks): the sequence of
 * `ParentPortNumber` values from the root-hub port down to the port the
 * device is plugged into on its immediate parent.
 *
 *  - @p parent_port — the port number on the device's **immediate**
 *    parent (root hub or an intermediate hub); the last element of the
 *    chain. This is what the app cannot derive on its own. For a device
 *    attached directly to the root hub this equals the whole path.
 *  - @p port_path — the full chain rendered as `'.'`-joined port
 *    numbers, root first (e.g. `"4"` for a root-port device, `"4.1"`
 *    for a device on port 1 of a hub on root port 4). Linux `lsusb -t`
 *    / sysfs port-path shape. Always NUL-terminated; truncated
 *    (snprintf-style) if @p port_path_len is too small — and truncation
 *    is **not** reflected in the return value, so size with
 *    @ref AXL_USB_PORT_PATH_MAX to never truncate.
 *
 * Both out-parameters are optional (pass NULL to skip either); passing
 * NULL for both is a successful no-op.
 *
 * @return AXL_OK on success, AXL_ERR if @p addr is not a known
 *     interface (the port chain is recovered at enumeration time, so a
 *     known interface always has one — there is no firmware call here).
 */
int
axl_usb_get_port_info(
    AxlUsbAddr   addr,           ///< target interface
    uint8_t     *parent_port,    ///< [out, optional] immediate-parent port number
    char        *port_path,      ///< [out, optional] '.'-joined port chain, root first
    size_t       port_path_len   ///< capacity of @p port_path in bytes (incl. NUL)
);

// ---------------------------------------------------------------------------
// Class triplet decode (compiled-in tables)
// ---------------------------------------------------------------------------

/// Buffer cap for a class-triplet decoded string. Sized for the
/// longest plausible "Base / Sub / Prot" composition.
#define AXL_USB_CLASS_NAME_MAX  128u

/**
 * @brief Output shape selector for axl_usb_class_string_fmt.
 *
 * USB class triplets decode into base / subclass / protocol tiers.
 * Verbose tools want the full slash-joined triplet; row-oriented
 * tools want the subclass alone (matches Linux lsusb shape); coarse
 * categorization wants just the base. Mirrors AxlPciClassFmt.
 */
typedef enum {
    AXL_USB_CLASS_FMT_FULL     = 0,  ///< "Human Interface Device / Boot Interface / Mouse"
    AXL_USB_CLASS_FMT_SUBCLASS = 1,  ///< "Boot Interface" (collapses to base if unknown)
    AXL_USB_CLASS_FMT_BASE     = 2,  ///< "Human Interface Device" (collapses to numeric if unknown)
} AxlUsbClassFmt;

/**
 * @brief Format a USB class triplet as a human-readable string.
 *
 * Decodes per the USB-IF Defined Class Codes spec
 * (https://www.usb.org/defined-class-codes) — up to three tiers:
 * base class, subclass, and protocol. Tiers with no spec-defined
 * name are omitted rather than rendered as `<unknown>` placeholders.
 *
 * Output shapes (FMT_FULL):
 *   - All known: `"<base> / <sub> / <prot>"`
 *     (e.g. `"Human Interface Device / Boot Interface / Mouse"`)
 *   - Known base+sub, unknown prot: `"<base> / <sub>"`
 *   - Known base, unknown sub: `"<base>"`
 *   - Wholly unknown: `"Class XXXXXX"` (numeric hex, in the spirit
 *     of Linux lsusb's numeric fallback).
 *
 * Always NUL-terminates @p buf (snprintf-shape).
 *
 * @return number of bytes written excluding NUL, or -1 if @p buf is
 *     NULL or @p buflen is 0 or @p fmt is unrecognized.
 */
int
axl_usb_class_string_fmt(
    uint8_t          class_,   ///< base class
    uint8_t          sub,     ///< subclass
    uint8_t          prot,    ///< protocol
    AxlUsbClassFmt   fmt,     ///< output shape selector
    char            *buf,     ///< destination buffer
    size_t           buflen   ///< capacity of @p buf
);

/// Convenience wrapper for axl_usb_class_string_fmt with `FMT_FULL`.
int
axl_usb_class_string(
    uint8_t  class_,
    uint8_t  sub,
    uint8_t  prot,
    char    *buf,
    size_t   buflen
);

// ---------------------------------------------------------------------------
// String descriptor reads (Phase C)
// ---------------------------------------------------------------------------

/// Buffer cap for UTF-8 string-descriptor output. USB string
/// descriptors max out at 254 bytes of UCS-2 payload (127 BMP code
/// points); a BMP code point in U+0800..U+FFFF expands to 3 UTF-8
/// bytes, so the worst-case payload is 127 * 3 = 381 bytes plus NUL.
/// BMP only — `axl_ucs2_to_utf8_buf` does not decode surrogate pairs,
/// so supplementary-plane code points (U+10000+) are not supported.
#define AXL_USB_STRING_MAX  384u

/**
 * @brief Read a USB string descriptor by index, decoded to UTF-8.
 *
 * Indices are unsigned bytes in the device's string descriptor table:
 * the device descriptor's @c iManufacturer / @c iProduct /
 * @c iSerialNumber fields name three of them by convention; class-
 * specific descriptors (HID, Audio, Configuration) reference others.
 *
 * Index 0 is reserved for the language-ID table (USB 2.0 §9.6.7);
 * `axl_usb_get_string` handles that internally — pass 1..255 for
 * actual strings. The library picks the first language ID the
 * device advertises and caches it per interface; multi-language
 * devices that need a specific LANGID are out of scope for this
 * helper.
 *
 * Output is NUL-terminated UTF-8 in @p buf. Truncation is silent at
 * the byte boundary (never writes a partial multi-byte sequence) —
 * `axl_ucs2_to_utf8_buf`'s contract.
 *
 * @return number of UTF-8 bytes written excluding NUL on success,
 *     -1 if @p addr is unknown, the device has no string descriptors,
 *     index 0 was passed, the firmware fails the descriptor read,
 *     or @p buf / @p buflen are bad.
 */
int
axl_usb_get_string(
    AxlUsbAddr   addr,           ///< target interface
    uint8_t      string_index,   ///< 1..255; 0 is reserved
    char        *buf,            ///< destination UTF-8 buffer
    size_t       buflen          ///< capacity in bytes (incl. NUL)
);

/**
 * @brief Read the device's manufacturer string (UTF-8).
 *
 * Convenience over axl_usb_get_string — reads the
 * @c iManufacturer index from the device descriptor, then fetches
 * that string. Returns -1 if the device descriptor declares no
 * manufacturer string (index = 0), mirroring `axl_usb_get_string`.
 *
 * @return UTF-8 byte count or -1 (see axl_usb_get_string).
 */
int
axl_usb_get_manufacturer(
    AxlUsbAddr   addr,
    char        *buf,
    size_t       buflen
);

/// Read the device's product string (UTF-8). See axl_usb_get_manufacturer.
int
axl_usb_get_product(
    AxlUsbAddr   addr,
    char        *buf,
    size_t       buflen
);

/// Read the device's serial-number string (UTF-8). See axl_usb_get_manufacturer.
int
axl_usb_get_serial(
    AxlUsbAddr   addr,
    char        *buf,
    size_t       buflen
);

// ---------------------------------------------------------------------------
// Control transfers
// ---------------------------------------------------------------------------

/// Direction of USB data transfer. Values match the USB-IO protocol's
/// `EFI_USB_DATA_DIRECTION` ordering exactly so the implementation can
/// pass through without translation.
typedef enum {
    AXL_USB_DATA_IN  = 0,  ///< host reads data from device
    AXL_USB_DATA_OUT = 1,  ///< host writes data to device
    AXL_USB_NO_DATA  = 2,  ///< setup-only; @p data must be NULL, @p data_len 0
} AxlUsbDataDir;

/**
 * @brief Issue a USB control transfer to a specific interface.
 *
 * Wraps `EFI_USB_IO_PROTOCOL.ControlTransfer` for a target identified
 * by `AxlUsbAddr`. Useful for class-specific control requests (CDC
 * SEND_ENCAPSULATED_COMMAND, HID SET_REPORT, etc.) that need to talk
 * to a device after enumeration but without going through a higher-
 * level driver.
 *
 * For class-targeted requests, build the setup packet's
 * @p request_type with `0x21` (Class | Interface | Out) or `0xA1`
 * (Class | Interface | In), and pass the interface number — typically
 * @p addr.intf — in @p index.
 *
 * @param addr          target interface
 * @param request_type  bmRequestType byte of the USB setup packet
 * @param request       bRequest byte of the USB setup packet
 * @param value         wValue field of the setup packet
 * @param index         wIndex field of the setup packet
 * @param direction     data-stage direction (in / out / none)
 * @param timeout_ms    transfer timeout in milliseconds
 * @param data          buffer for the data stage; NULL if @p direction
 *                      is `AXL_USB_NO_DATA`
 * @param data_len      bytes to transfer in the data stage
 * @return AXL_OK on success, AXL_ERR if @p addr is unknown, the
 *         underlying `ControlTransfer` returns an EFI error, OR the
 *         per-transfer USB status word is non-zero (any of
 *         `EFI_USB_ERR_STALL` / `_NAK` / `_CRC` / `_TIMEOUT` /
 *         `_BABBLE` / etc.). Callers that need to distinguish
 *         transient NAKs from hard stalls would need a richer API —
 *         this helper collapses everything non-zero into AXL_ERR.
 */
int
axl_usb_control_transfer(
    AxlUsbAddr      addr,
    uint8_t         request_type,
    uint8_t         request,
    uint16_t        value,
    uint16_t        index,
    AxlUsbDataDir   direction,
    uint32_t        timeout_ms,
    void           *data,
    size_t          data_len
);

// ---------------------------------------------------------------------------
// Topology walk (Phase F — real hub-port chain)
// ---------------------------------------------------------------------------

/* AXL_USB_TREE_MAX_DEPTH is defined up in the per-device-introspection
   section (the port-path buffer cap needs it). */

/**
 * @brief Per-node callback for axl_usb_tree_for_each.
 *
 * @param addr   the interface being visited (same shape
 *               axl_usb_next emits)
 * @param depth  USB hub depth — 0 means the device is directly
 *               attached to the host controller's root hub; 1
 *               means the device is one hub deep; etc. Different
 *               from @c addr.bus (which is the controller ordinal).
 * @param ctx    caller's opaque context
 *
 * @return non-zero to stop the walk early; the value becomes the
 *     return of axl_usb_tree_for_each. Return 0 to continue.
 */
typedef int (*AxlUsbTreeFn)(
    AxlUsbAddr   addr,
    unsigned     depth,
    void        *ctx
);

/**
 * @brief Walk the USB topology in tree order, depth-first per bus.
 *
 * Each `EFI_USB_IO_PROTOCOL` handle's device path encodes its full
 * port chain via consecutive USB messaging-type nodes
 * (`PciRoot/.../Pci(usb_ctrl)/USB(parent_port, intf)/USB(...)`).
 * The walker recovers that chain at ingest time and emits entries
 * in (bus, port_chain, intf) ascending order — guaranteeing parents
 * arrive before children, so a renderer can print indentation
 * derived from @p depth without lookahead.
 *
 * Hubs that have no leaf descendants still appear (depth = N for an
 * N-deep hub); their interface is enumerable via the same
 * `EFI_USB_IO_PROTOCOL` handle every other USB device exposes. The
 * caller can disambiguate hubs from leaves by reading the interface
 * class via axl_usb_get_class — class 0x09 is the USB Hub
 * class.
 *
 * The callback may freely call AxlUsb read-only APIs against the
 * visited @p addr (vid/pid, class, string descriptors) but must
 * not invoke axl_usb_next during the walk — the cursor it
 * uses is module-static and a re-entrant call would corrupt the
 * outer iteration.
 *
 * @return 0 on a clean walk, the callback's first non-zero return
 *     if it stopped early, or -1 if @p fn is NULL or no USB stack
 *     is available.
 */
int
axl_usb_tree_for_each(
    AxlUsbTreeFn  fn,
    void         *ctx
);

// ---------------------------------------------------------------------------
// USB vendor / device name database (Phase D — JSON5 sidecar)
// ---------------------------------------------------------------------------

/**
 * @name USB-name length contracts
 * @brief Maximum bytes (including NUL) any USB-IDs database lookup
 *     can return.
 *
 * Sized comfortably for real usb.ids entries — vendor strings like
 * "Logitech, Inc." run ~20 bytes, device strings frequently 60-100,
 * the rare full descriptive name fits in 192. Composed name covers
 * vendor + device + small fixed overhead, so axl_usb_format_name
 * never truncates non-truncated inputs.
 * @{
 */
#define AXL_USB_VENDOR_NAME_MAX     128u   ///< vendor entry max bytes
#define AXL_USB_DEVICE_NAME_MAX     192u   ///< device entry max bytes
#define AXL_USB_NAME_COMPOSED_MAX   384u   ///< axl_usb_format_name output max
/** @} */

/**
 * @brief Opaque handle to a loaded USB vendor/device-name database.
 *
 * Created by axl_usb_ids_open or axl_usb_ids_open_from_buffer,
 * destroyed by axl_usb_ids_close. Multiple handles can coexist —
 * a consumer that wants a "public + private" overlay loads two
 * handles and queries them in priority order. Mirrors AxlPciIds and
 * AxlSpdIds.
 *
 * Schema 1 is the only supported layout. The structure is
 * hierarchical from the start (vendors with nested devices) — USB
 * has no subsystem dimension that motivated PCI's v1→v2 split, so
 * there's no flat-vs-hierarchical schema dispatch.
 *
 * @code{.js}
 * { schema: 1,
 *   vendors: [
 *     { id: 0x046D, name: 'Logitech, Inc.',
 *       devices: [
 *         { pid: 0xC52B, name: 'Unifying Receiver' },
 *       ],
 *     },
 *   ],
 * }
 * @endcode
 */
typedef struct AxlUsbIds AxlUsbIds;

/**
 * @brief Open a USB-IDs database from a JSON5 file.
 *
 * @return @c AXL_SIDECAR_OK on success (handle returned via @p out),
 *     @c AXL_SIDECAR_FILE_MISSING if @p path does not exist,
 *     @c AXL_SIDECAR_PARSE_ERROR on JSON5 / schema rejection.
 */
AxlSidecarStatus
axl_usb_ids_open(
    const char   *path,
    AxlUsbIds   **out
);

/**
 * @brief Open a USB-IDs database from an in-memory JSON5 buffer.
 *
 * @return @c AXL_SIDECAR_OK on success, @c AXL_SIDECAR_PARSE_ERROR
 *     on parse / schema error.
 */
AxlSidecarStatus
axl_usb_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlUsbIds   **out
);

/**
 * @brief Free a database handle. NULL-safe.
 */
void
axl_usb_ids_close(
    AxlUsbIds  *ids
);

/**
 * @brief Vendor lookup against an explicit handle.
 * @return database-owned string or NULL if unknown / handle empty.
 */
const char *
axl_usb_ids_vendor_name(
    const AxlUsbIds  *ids,
    uint16_t          vid
);

/**
 * @brief Device lookup against an explicit handle.
 * @return database-owned string or NULL if (vid, pid) is unknown.
 */
const char *
axl_usb_ids_device_name(
    const AxlUsbIds  *ids,
    uint16_t          vid,
    uint16_t          pid
);

/**
 * @name Database iteration callbacks
 * @brief Non-zero callback return stops iteration and propagates as
 *     the iter rc.
 * @{
 */
typedef int (*AxlUsbIdsVendorFn)(uint16_t vid,
                                 const char *name, void *ctx);
typedef int (*AxlUsbIdsDeviceFn)(uint16_t vid, uint16_t pid,
                                 const char *name, void *ctx);

int
axl_usb_ids_foreach_vendor(
    const AxlUsbIds   *ids,
    AxlUsbIdsVendorFn  fn,
    void              *ctx
);

int
axl_usb_ids_foreach_device(
    const AxlUsbIds   *ids,
    AxlUsbIdsDeviceFn  fn,
    void              *ctx
);
/** @} */

/**
 * @brief Compose a "vendor + device" display string against a handle.
 *
 * Centralizes the rendering convention every consumer would otherwise
 * reinvent — every tool prints the same string for the same
 * @c (vid, pid) pair:
 *   - vendor known + device known   → `"<vendor> <device>"`
 *   - vendor known + device unknown → `"<vendor> Device <PID hex>"`
 *   - vendor unknown                → `"<VID>:<PID>"`
 *
 * Hex literals are lowercase, 4-wide, zero-padded (matches Linux
 * lsusb's `-d` filter form). Output never exceeds
 * AXL_USB_NAME_COMPOSED_MAX bytes.
 *
 * @return number of bytes written excluding NUL (snprintf shape),
 *     or -1 on bad arguments.
 */
int
axl_usb_ids_format_name(
    const AxlUsbIds  *ids,
    uint16_t          vid,
    uint16_t          pid,
    char             *buf,
    size_t            buflen
);

// ---------------------------------------------------------------------------
// Process-global singleton
// ---------------------------------------------------------------------------

/**
 * @brief Load the curated USB vendor/device name database into the
 *     process-global slot.
 *
 * Two modes selected by @p override_path:
 *   - **Explicit** (`override_path` non-NULL): use exactly that path.
 *   - **Autodiscover** (`override_path` NULL): try `usb-ids.json5`
 *     next to the running .efi (companion path), then in the
 *     current working directory.
 *
 * Idempotent — a successful load is a no-op on subsequent calls.
 * Registers an axl_atexit cleanup on first success so the
 * parsed table is freed automatically at runtime cleanup.
 */
AxlSidecarStatus
axl_usb_ids_load(
    const char  *override_path
);

/**
 * @brief Free the loaded database. Safe to call when none is loaded.
 */
void
axl_usb_ids_free(
    void
);

/**
 * @brief Singleton-backed vendor lookup.
 * @return database-owned string, or NULL if no database loaded or
 *     @p vid is not present.
 */
const char *
axl_usb_vendor_name(
    uint16_t  vid
);

/**
 * @brief Singleton-backed device lookup. Does not fall back to the
 *     vendor name when the device is unknown — callers compose their
 *     own "vendor name + numeric PID" via axl_usb_format_name.
 */
const char *
axl_usb_device_name(
    uint16_t  vid,
    uint16_t  pid
);

/**
 * @brief Singleton-backed convenience wrapper for
 *     axl_usb_ids_format_name.
 */
int
axl_usb_format_name(
    uint16_t  vid,
    uint16_t  pid,
    char     *buf,
    size_t    buflen
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_USB_H */
