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
#include <stddef.h>
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
    AXL_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY = 16, ///< Alias matching the spec's full name
    AXL_SMBIOS_TYPE_MEMORY_DEVICE       = 17,   ///< Memory Device (per DIMM)
    AXL_SMBIOS_TYPE_MEMORY_ARRAY_MAP    = 19,   ///< Memory Array Mapped Address
    AXL_SMBIOS_TYPE_MEMORY_DEVICE_MAP   = 20,   ///< Memory Device Mapped Address
    AXL_SMBIOS_TYPE_SYSTEM_BOOT         = 32,   ///< System Boot Information
    AXL_SMBIOS_TYPE_IPMI_DEVICE_INFO    = 38,   ///< IPMI Device Information (transport, address)
    AXL_SMBIOS_TYPE_ONBOARD_DEVICE_EXT  = 41,   ///< Onboard Devices Extended Information
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

/// SMBIOS Type 2 BoardType values (Table 14). The canonical
/// "is this a server blade?" detector — `BoardType == 3`. Type 3
/// chassis 0x1C/0x1D blade bits are unreliable on real Dell firmware
/// (see ADDF/Libs/SAL/AddfSAL.cpp:fIsBladeSmbios), so callers wanting
/// blade detection should always check Type 2 BoardType, not Type 3.
/// 0x00 is our "not published" sentinel (record too short to carry the
/// BoardType byte at offset 0x0D, rare since the field has been part
/// of Type 2 since spec 2.0). 0x02 is the spec's explicit "Unknown"
/// — semantically distinct, but we expose both via the same UNKNOWN
/// enum and let callers compare the raw byte against 0x02 directly
/// if they care.
enum {
    AXL_SMBIOS_BOARD_TYPE_UNKNOWN              = 0x00,
    AXL_SMBIOS_BOARD_TYPE_OTHER                = 0x01,
    AXL_SMBIOS_BOARD_TYPE_SPEC_UNKNOWN         = 0x02,
    AXL_SMBIOS_BOARD_TYPE_SERVER_BLADE         = 0x03,
    AXL_SMBIOS_BOARD_TYPE_CONNECTIVITY_SWITCH  = 0x04,
    AXL_SMBIOS_BOARD_TYPE_SYS_MGMT_MODULE      = 0x05,
    AXL_SMBIOS_BOARD_TYPE_PROCESSOR_MODULE     = 0x06,
    AXL_SMBIOS_BOARD_TYPE_IO_MODULE            = 0x07,
    AXL_SMBIOS_BOARD_TYPE_MEMORY_MODULE        = 0x08,
    AXL_SMBIOS_BOARD_TYPE_DAUGHTER_BOARD       = 0x09,
    AXL_SMBIOS_BOARD_TYPE_MOTHERBOARD          = 0x0A,
    AXL_SMBIOS_BOARD_TYPE_PROC_MEM_MODULE      = 0x0B,
    AXL_SMBIOS_BOARD_TYPE_PROC_IO_MODULE       = 0x0C,
    AXL_SMBIOS_BOARD_TYPE_INTERCONNECT_BOARD   = 0x0D,
};

/// Type 2 — Baseboard Information.
typedef struct {
    const char  *manufacturer;
    const char  *product_name;
    const char  *version;
    const char  *serial_number;
    const char  *asset_tag;
    uint8_t      board_type;       ///< SMBIOS spec Table 14 (AXL_SMBIOS_BOARD_TYPE_*).
                                   ///< 0 if firmware didn't publish (record too short
                                   ///< to carry the field at offset 0x0D, which is rare —
                                   ///< the field has been part of Type 2 since spec 2.0).
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
 * @brief Format a 16-byte SMBIOS UUID as a canonical string.
 *
 * SMBIOS §7.2.1 (System UUID) specifies that the first three fields
 * (Data1: 4 bytes, Data2: 2 bytes, Data3: 2 bytes) are stored
 * little-endian, while the remaining 8 bytes (Data4: 2 bytes +
 * Node: 6 bytes) are stored big-endian. The canonical printed form
 * applies the field-order swap on the first three fields, so a raw
 * memory dump differs from the printed string. Output matches the
 * `dmidecode` (Linux) and Windows `wmic csproduct get UUID` formats.
 *
 * @param bytes  raw 16 bytes from the SMBIOS Type 1 UUID field
 * @param out    output buffer of at least 37 bytes (36 + NUL)
 * @return 0 on success, -1 on NULL args.
 */
int axl_smbios_format_uuid(const uint8_t bytes[16], char out[37]);

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

/// Type 8 — Port Connector Information.
typedef struct {
    const char  *internal_designator;
    const char  *external_designator;
    uint8_t      internal_connector_type;   ///< SMBIOS spec Table 8 (e.g. 0x12 = Mini Centronics Type-26)
    uint8_t      external_connector_type;   ///< Same enum
    uint8_t      port_type;                 ///< SMBIOS spec Table 9 (e.g. 0x10 = Network Port)
} AxlSmbiosPortConnector;

/// Type 9 — System Slots. Spec-version-creep poster child: fields beyond
/// offset 0x0C arrived in 2.6, 3.2, and 3.4. Sentinels distinguish "not
/// published" from real values:
///   - segment_group / bus / device_function: 0xFFFF / 0xFF when not in record
///   - data_bus_width_base / peer_grouping_count: 0 when not in record
///
/// current_usage uses spec values 0x03..0x05. The typed reader returns
/// the raw byte, so callers that want a vendor-specific rendering
/// (e.g. "CPU NOT INSTALLED" for socket-associated 0x05 slots) can
/// translate downstream.
typedef struct {
    const char  *designation;
    uint8_t      slot_type;            ///< PCI/PCIe variant (SMBIOS spec Table 11)
    uint8_t      slot_data_bus_width;  ///< 0x08=1x..0x0E=32x (decoded width via the standard table)
    uint8_t      current_usage;        ///< 0x03=Available, 0x04=InUse, 0x05=Unavailable
    uint8_t      slot_length;
    uint16_t     slot_id;
    uint16_t     segment_group;        ///< 0xFFFF if not published (spec 2.6+ field)
    uint8_t      bus;                  ///< 0xFF if not published
    uint8_t      device_function;      ///< 0xFF if not published; bits 7:3 = device, bits 2:0 = function
    uint8_t      data_bus_width_base;  ///< SMBIOS 3.2+ field; 0 if not published
    uint8_t      peer_grouping_count;  ///< SMBIOS 3.2+ field; 0 if not published
    /* Variable-length peer grouping array intentionally omitted; consumers
       that need it can hand-walk via the raw header. */
} AxlSmbiosSystemSlot;

/// Type 11 — OEM Strings. Each string is accessed by 1-based index in the
/// SMBIOS spec; the typed reader exposes them as an array for ergonomics.
/// Real systems publish 4-8 entries; the cap of 16 covers everything we've
/// seen in the wild.
typedef struct {
    uint8_t       count;            ///< Number of strings
    const char   *strings[16];      ///< 1-based: strings[0] is what the spec calls index 1
} AxlSmbiosOemStrings;

/// Type 16 — Physical Memory Array. The 32→64-bit max-capacity fallback is
/// resolved internally: max_capacity_bytes is always populated correctly.
typedef struct {
    uint8_t      location;            ///< 0x03=System board, 0x0A=PC Card, ...
    uint8_t      use;                 ///< 0x03=System Memory, 0x04=Video, ...
    uint8_t      ecc_type;
    uint64_t     max_capacity_bytes;  ///< Resolves the 32→64-bit fallback
                                      ///< (uses ExtendedMaxCapacity at offset 0x0F when
                                      ///< MaxCapacity field == 0x80000000). 0 if the
                                      ///< sentinel is set but the record is too short
                                      ///< to carry the Extended field.
    uint16_t     error_handle;        ///< 0xFFFE = no error info, 0xFFFF = unknown
    uint16_t     num_devices;
} AxlSmbiosPhysicalMemoryArray;

/// Type 19 — Memory Array Mapped Address. The 32→64-bit address fallback
/// is resolved internally via the ExtendedStartingAddress / ExtendedEndingAddress
/// fields (spec 2.7+, used when the 32-bit fields == 0xFFFFFFFF). When the
/// sentinel is set but the record is too short to carry the Extended field
/// (malformed firmware), the address is reported as 0 rather than the
/// literal 4 TB - 1 KB the sentinel × 1024 would produce.
typedef struct {
    uint64_t     starting_address;    ///< Byte address (resolved from extended field if needed); 0 = unresolvable
    uint64_t     ending_address;      ///< Byte address (resolved from extended field if needed); 0 = unresolvable
    uint16_t     array_handle;        ///< Handle of the Type 16 record this maps
    uint8_t      partition_width;     ///< Number of Type 17s feeding this region
} AxlSmbiosMemoryArrayMap;

/// Type 20 — Memory Device Mapped Address. Same 64-bit fallback story as Type 19.
typedef struct {
    uint64_t     starting_address;    ///< Byte address (resolved from extended field if needed); 0 = unresolvable
    uint64_t     ending_address;      ///< Byte address (resolved from extended field if needed); 0 = unresolvable
    uint16_t     device_handle;       ///< Handle of the Type 17 record this maps
    uint16_t     array_map_handle;    ///< Handle of the Type 19 record this is a child of
    uint8_t      partition_row_pos;   ///< 0xFF = N/A
    uint8_t      interleave_position; ///< 0 = non-interleaved, 0xFF = unknown
    uint8_t      interleave_data_depth; ///< 0xFF = unknown
} AxlSmbiosMemoryDeviceMap;

/// Type 41 — Onboard Devices Extended Information.
typedef struct {
    const char  *reference_designation;
    uint8_t      device_type;          ///< Bit 7 = Status (0=Disabled, 1=Enabled), low 7 bits = type
                                       ///< (0x05=Ethernet, 0x07=SAS, etc.)
    uint8_t      device_type_instance; ///< 1-based per-type index
    uint16_t     segment_group;
    uint8_t      bus;
    uint8_t      device_function;      ///< Packed: bits 7:3 = device, bits 2:0 = function
} AxlSmbiosOnboardDeviceExt;

/**
 * @brief Read a Type 8 (Port Connector) record the caller already located.
 *
 * Firmware publishes one Type 8 per physical connector. Enumerate with
 * `axl_smbios_find_next(AXL_SMBIOS_TYPE_PORT_CONNECTOR, prev)` and call
 * this for each result.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_port_connector(AxlSmbiosHeader *hdr, AxlSmbiosPortConnector *out);

/**
 * @brief Read a Type 9 (System Slot) record the caller already located.
 *
 * Length-aware: fields added in spec 2.6 / 3.2 / 3.4 fall through to the
 * documented sentinels (0xFFFF / 0xFF / 0) when the firmware's record is
 * too short to carry them.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_system_slot(AxlSmbiosHeader *hdr, AxlSmbiosSystemSlot *out);

/**
 * @brief Read a Type 11 (OEM Strings) record the caller already located.
 *
 * Populates `count` with the number of strings the record advertises and
 * `strings[]` with pointers into the SMBIOS table memory (valid for the
 * life of the app). Caps at 16 entries — anything beyond is ignored.
 *
 * @return 0 on success, -1 if @a hdr is NULL or wrong type.
 */
int axl_smbios_read_oem_strings(AxlSmbiosHeader *hdr, AxlSmbiosOemStrings *out);

/**
 * @brief Read one Type 11 OEM string by 1-based global index.
 *
 * Walks Type 11 records in firmware order and treats the strings
 * across all records as one contiguous 1-based list. Index 1 is
 * the first string of the first Type 11 record; if that record
 * publishes 4 strings, index 5 is the first string of the second
 * record, and so on. Most platforms ship a single Type 11 record;
 * this is mostly a robustness convenience for callers that don't
 * want to enumerate records themselves.
 *
 * On success the string is copied into @p buf and NUL-terminated.
 * If the string (including the NUL) doesn't fit in @p buf_cap,
 * the function returns -1 without copying — the caller is
 * expected to retry with a larger buffer rather than receive a
 * silently-truncated value. When @p required is non-NULL it is
 * set to the byte count needed (string length + 1 for the NUL),
 * letting callers size a follow-up allocation exactly. Other
 * failure modes (no Type 11 record, index out of range, bad args)
 * leave @p *required unchanged.
 *
 * @return 0 on success, -1 if no Type 11 record exists, the
 *     index is out of range, @p buf / @p buf_cap are bad, or
 *     @p buf_cap is too small for the matched string.
 */
int
axl_smbios_get_oem_string(
    uint8_t  index_one_based,   ///< 1-based string index per SMBIOS §7.12
    char    *buf,               ///< [out] receives the NUL-terminated string
    size_t   buf_cap,           ///< capacity of @p buf in bytes (must be >= 1)
    size_t  *required           ///< [out, NULL OK] byte count needed (string + NUL) on too-small
);

/**
 * @brief Read a Type 16 (Physical Memory Array) record.
 *
 * Resolves the 32→64-bit max-capacity fallback automatically.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_physical_memory_array(AxlSmbiosHeader *hdr, AxlSmbiosPhysicalMemoryArray *out);

/**
 * @brief Read a Type 19 (Memory Array Mapped Address) record.
 *
 * Resolves the 32→64-bit address fallback automatically.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_memory_array_map(AxlSmbiosHeader *hdr, AxlSmbiosMemoryArrayMap *out);

/**
 * @brief Read a Type 20 (Memory Device Mapped Address) record.
 *
 * Resolves the 32→64-bit address fallback automatically.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_memory_device_map(AxlSmbiosHeader *hdr, AxlSmbiosMemoryDeviceMap *out);

/**
 * @brief Read a Type 41 (Onboard Devices Extended) record.
 *
 * @return 0 on success, -1 if @a hdr is NULL, wrong type, or too short.
 */
int axl_smbios_read_onboard_device_ext(AxlSmbiosHeader *hdr, AxlSmbiosOnboardDeviceExt *out);

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
 * Returns a direct pointer into the SMBIOS table memory, which persists
 * for the life of the app. Reentrant — multiple calls in one printf are
 * safe.
 *
 * @return pointer into the SMBIOS table, or "" if not found.
 */
const char *
axl_smbios_get_string_utf8(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    unsigned char     string_index   ///< 1-based string index (0 returns "")
);

/**
 * @brief Copy a string from an SMBIOS table's string area into a caller buffer.
 *
 * Reentrant alternative for callers that want a writable copy or need
 * length-bounded handling. Truncates safely on overflow and always
 * NUL-terminates if @a buf_size > 0.
 *
 * @return number of bytes written (excluding the terminating NUL), or 0
 *         if the string was not found / @a hdr is NULL / @a string_index
 *         is 0. When the source string would be longer than @a buf_size - 1,
 *         the return value is @a buf_size - 1 (the truncated length).
 */
size_t
axl_smbios_copy_string_utf8(
    AxlSmbiosHeader  *hdr,           ///< SMBIOS table header
    uint8_t           string_index,  ///< 1-based string index (0 writes empty + returns 0)
    char             *buf,           ///< destination buffer
    size_t            buf_size       ///< destination buffer capacity
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
 * @brief Byte length of an SMBIOS record's string-region payload.
 *
 * Walks from `hdr->Length` to the spec's end-of-region double-NUL
 * (0x00 0x00) and returns the number of bytes in the strings region —
 * including each string's NUL terminator, excluding the final extra
 * NUL that ends the region.
 *
 * For records with zero strings (formatted area immediately followed
 * by the two-byte 0x00 0x00 sentinel), returns 0. NULL @a hdr returns 0.
 *
 * Useful for "raw record" dumps that need to know the full record
 * span on disk including its inline strings.
 *
 * @return byte count of the strings region (may be 0).
 */
size_t
axl_smbios_strings_byte_len(
    AxlSmbiosHeader  *hdr  ///< SMBIOS table header
);

// ---------------------------------------------------------------------------
// SMBIOS Type 9 (System Slots) spec-value decoders
//
// Pure spec-table lookups, no allocation, return a static const string
// or NULL for unrecognized values. Callers that want to render unknown
// values can fall back to printing raw "0x%02X".
// ---------------------------------------------------------------------------

/**
 * @brief Slot type code → display string.
 *
 * Covers the full SMBIOS Table 13 enumeration including modern
 * additions tracked by recent vendor fixes:
 *   - PCIe Gen 1..6 (legacy 0xA1-0xA6, Gen 2-6 at 0xA7-0xBF)
 *   - 0x25 — M.2 Socket 3 (Mech Key M) Gen 5 (the historical
 *     "Gen 4 vs Gen 5" mismapping fixed in Dell aab01c48d)
 *   - 0x26 / 0x27 / 0x28 — OCP NIC 3.0 SFF / LFF / Prior to 3.0
 *     (Dell 0c558a930)
 *   - 0x29 / 0x2A — EDSFF E1.S / E1.L
 *   - 0x2B / 0x2C — EDSFF E3.S / E3.L
 *   - 0x22 / 0x23 / 0x24 / 0x25 — M.2 Mech Keys A / E / B / M
 *   - 0x06 / 0x07 / 0x0F — PCI / PCI-X / AGP
 *
 * @return static string, or NULL if @a type isn't a recognized value.
 */
const char *
axl_smbios_slot_type_str(
    uint8_t type   ///< SMBIOS Type 9 slot_type field
);

/**
 * @brief Slot data-bus width code → display string ("1x", "8x", "16x", ...).
 *
 * SMBIOS Table 11. Returns NULL for unknown values.
 */
const char *
axl_smbios_slot_width_str(
    uint8_t bw   ///< SMBIOS Type 9 slot_data_bus_width field
);

/**
 * @brief Slot current-usage code → display string.
 *
 * SMBIOS Table 12. Returns "Other" / "Unknown" / "Empty" / "InUse" /
 * "Unavailable" — strings match the SMBIOS 3.7 spec. Vendor-specific
 * renderings (e.g. "CPU NOT INSTALLED" for socket-associated 0x05
 * slots) belong in consumer code; read AxlSmbiosSystemSlot.current_usage
 * and translate. Returns NULL for unknown values.
 */
const char *
axl_smbios_slot_usage_str(
    uint8_t cu   ///< SMBIOS Type 9 current_usage field
);

// ---------------------------------------------------------------------------
// SMBIOS Type 3 (Chassis) classification
// ---------------------------------------------------------------------------

/// Coarse classification of an SMBIOS Type 3 chassis type byte.
/// Pure-spec interpretation; vendor-specific overrides (e.g. "is server"
/// detection that uses sysId tables or PCI audio-device probes on top
/// of the spec class) live in consumer code.
typedef enum {
    AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN  = 0,
    AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,    ///< 0x03/04/05/06/07 — desktop / SFF / mini-tower / tower
                                  ///<   (also 0x18 Sealed-case PC — desktop, NOT server)
    AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,   ///< 0x08/09/0A/0C/0E/1E/1F/20 — portable / laptop /
                                  ///<   notebook / docking / sub-notebook / tablet /
                                  ///<   convertible / detachable
    AXL_SMBIOS_CHASSIS_CLASS_SERVER,     ///< 0x17/19/1B/1C/1D — rack / multi-system /
                                  ///<   pizza box / blade / blade enclosure
    AXL_SMBIOS_CHASSIS_CLASS_EMBEDDED,   ///< 0x21 IoT Gateway, 0x22 Embedded PC,
                                  ///<   0x23 Mini PC (per SMBIOS 3.7)
    AXL_SMBIOS_CHASSIS_CLASS_OTHER,      ///< Recognized chassis type outside the above buckets
} AxlSmbiosChassisClass;

/**
 * @brief Classify SMBIOS Type 3 chassis type into a coarse class.
 *
 * Pure-spec interpretation of the chassis-type byte. Strips the high
 * 0x80 lock bit before classifying. See AxlSmbiosChassisClass for
 * the bucket assignments.
 *
 * Pitfalls worth knowing:
 *   - 0x18 ("Sealed-case PC") is desktop/SFF, not server.
 *   - 0x23 ("Mini PC" per SMBIOS 3.7) is EMBEDDED, not IoT Gateway.
 *
 * @return matching AxlSmbiosChassisClass, or AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN
 *         if @a type is 0 / out of range / explicitly the spec's
 *         "Unknown" (0x02).
 */
AxlSmbiosChassisClass
axl_smbios_chassis_class(
    uint8_t type   ///< SMBIOS Type 3 type byte (lock bit allowed; will be stripped)
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
