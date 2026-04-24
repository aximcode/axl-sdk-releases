/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-smbios.h:
 *
 * UEFI SMBIOS helpers — string extraction and table lookup.
 */

#ifndef AXL_SMBIOS_H
#define AXL_SMBIOS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// SMBIOS table header (standard C types, matches SMBIOS spec layout).
typedef struct {
    uint8_t   Type;
    uint8_t   Length;
    uint16_t  Handle;
} AxlSmbiosHeader;

/// Common SMBIOS table types. Values match the SMBIOS specification;
/// use these constants instead of bare numbers for readability.
enum {
    AXL_SMBIOS_TYPE_BIOS_INFO           = 0,    ///< BIOS Information
    AXL_SMBIOS_TYPE_SYSTEM_INFO         = 1,    ///< System Information (manufacturer, product, UUID)
    AXL_SMBIOS_TYPE_BASEBOARD           = 2,    ///< Baseboard / Module
    AXL_SMBIOS_TYPE_CHASSIS             = 3,    ///< System Enclosure / Chassis
    AXL_SMBIOS_TYPE_PROCESSOR           = 4,    ///< Processor
    AXL_SMBIOS_TYPE_CACHE               = 7,    ///< Cache
    AXL_SMBIOS_TYPE_PORT_CONNECTOR      = 8,    ///< Port Connector
    AXL_SMBIOS_TYPE_SYSTEM_SLOTS        = 9,    ///< System Slots
    AXL_SMBIOS_TYPE_OEM_STRINGS         = 11,   ///< OEM Strings
    AXL_SMBIOS_TYPE_BIOS_LANGUAGE       = 13,   ///< BIOS Language Information
    AXL_SMBIOS_TYPE_PHYSICAL_MEMORY     = 16,   ///< Physical Memory Array
    AXL_SMBIOS_TYPE_MEMORY_DEVICE       = 17,   ///< Memory Device (per DIMM)
    AXL_SMBIOS_TYPE_MEMORY_ARRAY_MAP    = 19,   ///< Memory Array Mapped Address
    AXL_SMBIOS_TYPE_SYSTEM_BOOT         = 32,   ///< System Boot Information
    AXL_SMBIOS_TYPE_IPMI_DEVICE_INFO    = 38,   ///< IPMI Device Information (transport, address)
    AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE = 42,   ///< Management Controller Host Interface (Redfish, OEM, ...)
    AXL_SMBIOS_TYPE_END                 = 127,  ///< End-of-table sentinel
};

/// IPMI interface type codes (Type 38 offset 0x04).
enum {
    AXL_SMBIOS_IPMI_UNKNOWN             = 0x00,
    AXL_SMBIOS_IPMI_KCS                 = 0x01,
    AXL_SMBIOS_IPMI_SMIC                = 0x02,
    AXL_SMBIOS_IPMI_BT                  = 0x03,
    AXL_SMBIOS_IPMI_SSIF                = 0x04,
};

/// Management Controller Host Interface types (Type 42 offset 0x04).
enum {
    AXL_SMBIOS_HIF_KCS                  = 0x02,  ///< Keyboard Controller Style
    AXL_SMBIOS_HIF_UART_8250            = 0x03,
    AXL_SMBIOS_HIF_UART_16450           = 0x04,
    AXL_SMBIOS_HIF_UART_16550           = 0x05,
    AXL_SMBIOS_HIF_UART_16650           = 0x06,
    AXL_SMBIOS_HIF_UART_16750           = 0x07,
    AXL_SMBIOS_HIF_UART_16850           = 0x08,
    AXL_SMBIOS_HIF_NETWORK              = 0x40,  ///< Network Host Interface (used for Redfish)
    AXL_SMBIOS_HIF_OEM                  = 0xF0,  ///< OEM-defined
};

/// Management Controller Host Interface protocols (Type 42 protocol record).
enum {
    AXL_SMBIOS_HIP_IPMI                 = 0x02,
    AXL_SMBIOS_HIP_MCTP                 = 0x03,
    AXL_SMBIOS_HIP_REDFISH_OVER_IP      = 0x04,  ///< Requires SMBIOS 3.2+
    AXL_SMBIOS_HIP_OEM                  = 0xF0,
};

// ---------------------------------------------------------------------------
// Typed record accessors
//
// Populate a caller-provided struct with every field this SDK cares about
// for the corresponding SMBIOS record type. String pointers reference the
// live SMBIOS table memory (valid for the life of the app). Numeric fields
// are set to 0 or a sentinel when the firmware didn't publish them (older
// spec versions or firmware that reports "unknown").
// ---------------------------------------------------------------------------

/// Type 0 — BIOS Information.
typedef struct {
    const char  *vendor;
    const char  *version;
    const char  *release_date;
    uint8_t      major_release;   ///< 0xFF if firmware didn't publish
    uint8_t      minor_release;   ///< 0xFF if firmware didn't publish
} AxlSmbiosBiosInfo;

/// Type 1 — System Information.
typedef struct {
    const char  *manufacturer;
    const char  *product_name;
    const char  *version;
    const char  *serial_number;
    const char  *sku;             ///< NULL if not published (spec 2.4+)
    const char  *family;          ///< NULL if not published (spec 2.4+)
    uint8_t      uuid[16];        ///< RFC 4122 byte order (see note below)
    bool         has_uuid;        ///< false if UUID field is unset (all 0x00 or 0xFF)
} AxlSmbiosSystemInfo;

/// Type 2 — Baseboard Information.
typedef struct {
    const char  *manufacturer;
    const char  *product_name;
    const char  *version;
    const char  *serial_number;
    const char  *asset_tag;
} AxlSmbiosBaseboardInfo;

/// Type 3 — System Enclosure / Chassis.
typedef struct {
    const char  *manufacturer;
    const char  *version;
    const char  *serial_number;
    const char  *asset_tag;
    uint8_t      type;            ///< Low 7 bits; 0x80 bit (lock) stripped
} AxlSmbiosChassisInfo;

/// Type 4 — Processor.
typedef struct {
    const char  *socket_designation;
    const char  *manufacturer;
    const char  *version;
    const char  *serial_number;
    const char  *asset_tag;
    const char  *part_number;
    uint8_t      family;          ///< SMBIOS processor-family code
    uint16_t     current_speed_mhz;
    uint16_t     max_speed_mhz;
    uint8_t      core_count;      ///< 0 if not published (spec 2.5+)
    uint8_t      thread_count;    ///< 0 if not published
    uint8_t      status;          ///< CPU socket populated + enabled bits
} AxlSmbiosProcessorInfo;

/// Type 17 — Memory Device (per DIMM slot).
typedef struct {
    const char  *device_locator;
    const char  *bank_locator;
    const char  *manufacturer;
    const char  *part_number;
    const char  *serial_number;
    const char  *asset_tag;
    uint32_t     size_mb;         ///< 0 if slot is empty
    uint16_t     speed_mhz;       ///< 0 if unknown
    uint8_t      memory_type;     ///< SMBIOS memory type (DDR4=0x1A, DDR5=0x22, ...)
} AxlSmbiosMemoryDevice;

/**
 * @brief Read Type 0 (BIOS Information) from the SMBIOS table.
 * @return 0 on success, -1 if no Type 0 record is present.
 */
int axl_smbios_read_bios_info(AxlSmbiosBiosInfo *out);

/**
 * @brief Read Type 1 (System Information) from the SMBIOS table.
 * @return 0 on success, -1 if no Type 1 record is present.
 */
int axl_smbios_read_system_info(AxlSmbiosSystemInfo *out);

/**
 * @brief Read Type 2 (Baseboard Information) from the SMBIOS table.
 * @return 0 on success, -1 if no Type 2 record is present.
 */
int axl_smbios_read_baseboard(AxlSmbiosBaseboardInfo *out);

/**
 * @brief Read Type 3 (System Enclosure / Chassis) from the SMBIOS table.
 * @return 0 on success, -1 if no Type 3 record is present.
 */
int axl_smbios_read_chassis(AxlSmbiosChassisInfo *out);

/**
 * @brief Read a Type 4 (Processor) record the caller already located.
 *
 * Firmware publishes one Type 4 per socket. Enumerate with
 * `axl_smbios_find_next(AXL_SMBIOS_TYPE_PROCESSOR, prev)` and call this
 * for each result.
 *
 * @return 0 on success, -1 if @a hdr is NULL or the wrong record type.
 */
int axl_smbios_read_processor(AxlSmbiosHeader *hdr, AxlSmbiosProcessorInfo *out);

/**
 * @brief Read a Type 17 (Memory Device) record the caller already located.
 *
 * Firmware publishes one Type 17 per DIMM slot (populated or not).
 * Enumerate with `axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_DEVICE, prev)`
 * and call this for each result.
 *
 * @return 0 on success, -1 if @a hdr is NULL or the wrong record type.
 */
int axl_smbios_read_memory_device(AxlSmbiosHeader *hdr, AxlSmbiosMemoryDevice *out);

/// Type 38 — IPMI Device Information.
typedef struct {
    uint8_t     interface_type;      ///< AXL_SMBIOS_IPMI_* (KCS, SMIC, BT, SSIF, ...)
    uint8_t     spec_major;          ///< IPMI spec revision major (high nibble of byte 0x05)
    uint8_t     spec_minor;          ///< IPMI spec revision minor (low nibble of byte 0x05)
    uint8_t     i2c_target_address;  ///< BMC slave address on SMBus/SSIF (0 if N/A)
    uint8_t     nv_storage_address;  ///< NV storage device address (0xFF = none)
    uint64_t    base_address;        ///< Interface base address (I/O or MMIO, see is_memory_mapped)
    bool        is_memory_mapped;    ///< true = MMIO, false = I/O port
    uint8_t     interrupt_number;    ///< 0 = no interrupt
} AxlSmbiosIpmiDeviceInfo;

/// Type 42 — Management Controller Host Interface, one protocol record.
typedef struct {
    uint8_t         protocol_type;       ///< AXL_SMBIOS_HIP_* value
    uint8_t         data_len;            ///< length of `data`
    const uint8_t  *data;                ///< protocol-specific payload (pointer into SMBIOS table)
} AxlSmbiosHostInterfaceProtocol;

/// Type 42 — Management Controller Host Interface.
typedef struct {
    uint8_t                             interface_type;     ///< AXL_SMBIOS_HIF_*
    uint8_t                             interface_data_len; ///< length of `interface_data`
    const uint8_t                      *interface_data;    ///< interface-specific bytes (pointer into SMBIOS table)
    uint8_t                             protocol_count;    ///< number of entries in `protocols`
    AxlSmbiosHostInterfaceProtocol      protocols[8];      ///< capped at 8 — real firmware emits 1-2
} AxlSmbiosHostInterface;

/**
 * @brief Read Type 38 (IPMI Device Information).
 *
 * Firmware publishes at most one Type 38 record (the single BMC).
 * This is what AxlIpmi's transport auto-detector reads to decide
 * between KCS, SMIC, BT, and SSIF.
 *
 * @return 0 on success, -1 if no Type 38 record is present.
 */
int axl_smbios_read_ipmi_device_info(AxlSmbiosIpmiDeviceInfo *out);

/**
 * @brief Read a Type 42 (Management Controller Host Interface) record.
 *
 * Firmware may publish multiple Type 42 records (one per interface);
 * enumerate with `axl_smbios_find_next(AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE, prev)`
 * and call this for each result. Requires SMBIOS 3.0 or later for the
 * modern variable-length layout; older spec versions of Type 42 are not
 * supported and return -1.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or the record
 *         layout is too old to decode.
 */
int axl_smbios_read_host_interface(AxlSmbiosHeader *hdr, AxlSmbiosHostInterface *out);

/**
 * @brief Find the first Type 42 record advertising Redfish over IP.
 *
 * Scans Type 42s for `interface_type == AXL_SMBIOS_HIF_NETWORK` with a
 * `AXL_SMBIOS_HIP_REDFISH_OVER_IP` protocol entry. Common use: an
 * in-band tool that wants to talk Redfish to the local BMC without
 * network probing.
 *
 * @param hdr_out    optional — receives the matching Type 42 header
 * @param iface_out  optional — receives the parsed interface (typed reader output)
 * @return 0 on success, -1 if nothing matches.
 */
int axl_smbios_find_redfish_host_interface(
    AxlSmbiosHeader        **hdr_out,
    AxlSmbiosHostInterface  *iface_out
);

/**
 * @brief Get the system UUID with the endian-swap SMBIOS requires applied.
 *
 * SMBIOS stores the UUID's first three fields little-endian on the wire
 * (since spec 2.6), but the RFC 4122 "standard" / `dmidecode` display
 * form expects big-endian. This helper returns the RFC 4122 byte order
 * so you can feed the result to anything expecting canonical UUID bytes.
 *
 * @return 0 on success, -1 if no Type 1 record or UUID is unset
 *         (all 0x00 or all 0xFF per the spec's "not present" markers).
 */
int axl_smbios_get_system_uuid(uint8_t out[16]);

/**
 * @brief Get a string from an SMBIOS table's string area (UCS-2).
 *
 * Returns a pointer to a static 128-char unsigned short buffer — caller
 * must use the value before the next call (not reentrant).
 *
 * @return pointer to static unsigned short buffer.
 */
unsigned short *
axl_smbios_get_string(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    unsigned char     string_index   ///< 1-based string index (0 returns empty string)
);

/**
 * @brief Get a string from an SMBIOS table's string area (UTF-8).
 *
 * Returns a pointer to a static 128-char buffer — caller must use
 * the value before the next call (not reentrant).
 *
 * @return pointer to static char buffer, or "" if not found.
 */
const char *
axl_smbios_get_string_utf8(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    unsigned char     string_index   ///< 1-based string index (0 returns "")
);

/**
 * @brief Find the first SMBIOS table of a given type.
 *
 * @return pointer to table header, or NULL if not found.
 */
AxlSmbiosHeader *
axl_smbios_find(
    unsigned char type  ///< SMBIOS table type (e.g. 0 for BIOS, 1 for System)
);

/**
 * @brief Find the next SMBIOS table of a given type after @a prev.
 *
 * Pass NULL as @a prev to find the first (same as axl_smbios_find).
 * Use in a loop to enumerate all tables of a type:
 *
 * @code
 * AxlSmbiosHeader *h = NULL;
 * while ((h = axl_smbios_find_next(17, h)) != NULL) {
 *     // process each Type 17 (Memory Device) entry
 * }
 * @endcode
 *
 * @return pointer to next table header, or NULL if no more.
 */
AxlSmbiosHeader *
axl_smbios_find_next(
    unsigned char     type,  ///< SMBIOS table type
    AxlSmbiosHeader  *prev   ///< previous result (NULL to start from beginning)
);

/**
 * @brief Iterate every SMBIOS record regardless of type.
 *
 * Pass NULL for the first call; pass the previous result for subsequent
 * calls. Returns NULL when there are no more records. Walks the table
 * in the order firmware published it, stopping at the Type 127
 * end-of-table sentinel.
 *
 * @code
 * AxlSmbiosHeader *h = NULL;
 * while ((h = axl_smbios_next(h)) != NULL) {
 *     axl_printf("Type %u  Handle 0x%04x  Length %u\n",
 *                h->Type, h->Handle, h->Length);
 * }
 * @endcode
 *
 * @return pointer to next table header, or NULL if no more.
 */
AxlSmbiosHeader *
axl_smbios_next(
    AxlSmbiosHeader  *prev   ///< previous result (NULL to start from beginning)
);

/**
 * @brief Report the SMBIOS specification version published by firmware.
 *
 * Useful for gating features on the spec version — some Type 17 Memory
 * Device fields were added in SMBIOS 2.7, and Type 43 TPM Device requires
 * 3.1 or later.
 *
 * @return 0 on success, -1 if no SMBIOS table was found.
 */
int
axl_smbios_version(
    unsigned char *major,  ///< written with major version (e.g. 3)
    unsigned char *minor   ///< written with minor version (e.g. 1)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SMBIOS_H */
