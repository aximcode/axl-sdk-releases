/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd.h
    JEDEC Serial Presence Detect (SPD) reader for DDR4/DDR5 DIMMs.

    SPD EEPROMs sit on the platform SMBus at addresses 0x50–0x57 (one
    per DIMM slot) and carry the JEDEC-defined module-identification
    block written by the DIMM vendor at manufacture time. AxlSpd
    talks to them over AxlSmbus (so HC and I2C-Master transports
    both work), branches the codec on the memory-type byte at offset
    2, and surfaces a decoded AxlSpdInfo struct.

    Manufacturer fields are exposed as raw 16-bit JEP-106 codes
    (high byte = continuation-bank index, low byte = position in
    bank). For human-readable rendering, the @c axl_spd_ids_* API
    loads a curated JSON5 sidecar (`jedec.json5`) into a process-
    global table and exposes lookup / format helpers parallel to
    axl_pci_ids_load and axl_pci_format_name. Consumers
    that want a different DB (private OEM sheet, vendor-restricted
    list) layer their own handle on top via
    axl_spd_ids_open_from_buffer — same shape as AxlPciIds.

    DDR3 is intentionally out of scope for v1. DDR5 modules use the
    SPD5118 hub protocol: the lower 128 bytes of each 128-byte page
    are addressed directly; the upper window is paged via MR11
    (register 0x0B). AxlSpd handles the page selection internally.

    Iteration mirrors the established cursor pattern (see
    axl_smbios_find_next, axl_pci_next):

    @code
    uint8_t *slot = NULL;
    while ((slot = axl_spd_next(slot)) != NULL) {
        AxlSpdInfo info;
        if (axl_spd_read(*slot, &info) == 0) {
            // ... decoded fields available in `info`
        }
    }
    @endcode

    There is no @c axl_spd_init() entry point — first call to a
    public function lazily opens an AxlSmbus session.

    @section spd_limitations Platform limitations

    Direct SPD reads work only when the platform's SMBus exposes
    the DIMM EEPROMs to non-firmware software. On many server
    boards this isn't true and the API returns 0 populated slots.
    Known affected classes (verified empirically):

    -# **OEM-CPLD-routed DIMMs on some AMD server boards
       (FCH 1022:790b family)** — DDR5 SPDs are NOT on the FCH
       AUX SMBus on these platforms. They're physically routed
       through an OEM-specific CPLD ("FPGA hub PLD") behind a
       vendor UEFI protocol; BIOS reads them there and
       publishes the results via SMBIOS Type 17. The FCH AUX
       SMBus is electrically present but empty, returning the
       chipset-default "all-ACK + zero-data" pattern Linux
       warns about in `drivers/i2c/busses/i2c-piix4.c` lines
       968-974. Linux's spd5118 driver fails to bind for the
       same reason — verified on an AMD-EPYC server with
       kernel 6.19.10.

    -# **Platforms where the SMBus exposing DIMM SPDs isn't
       published as `EFI_SMBUS_HC_PROTOCOL` or
       `EFI_I2C_MASTER_PROTOCOL`** — AxlSpd's auto-detect can't
       find a transport. The PIIX4 direct-I/O fallback (when
       AMD FCH SMBus PCI device is present) helps on boards
       where firmware is opinionated about which controller it
       advertises, but the chipset still has to deliver real
       bytes (see #1).

    -# **DDR3 / pre-2014 modules** — codec coverage is DDR4 +
       DDR5 only. SPD3 EEPROMs respond but `axl_spd_decode`
       returns `AxlSpdInfo{ ddr_generation = 0 }` for them.

    @section spd_fallback Recommended consumer pattern

    Treat AxlSpd as opportunistic and always have an SMBIOS
    Type 17 fallback ready. Per-DIMM SMBIOS data — manufacturer,
    part number, capacity, configured speed, ECC bits, slot
    locator — is BIOS-populated on every system that ships
    SMBIOS, including all the platforms where AxlSpd silently
    finds nothing. See `tools/memspd.c` for the reference
    "try AxlSpd first, fall back to SMBIOS Type 17" pattern.

    The cases where AxlSpd genuinely beats SMBIOS Type 17 are
    rare: live timing parameters (tCL, tRCD, tRP, tRAS) that
    aren't exposed via Type 17, and platforms where the BIOS
    publishes a stale or partial Type 17 (uncommon on modern
    firmware).
**/

#ifndef AXL_SPD_H
#define AXL_SPD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#include <axl/axl-sidecar.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// First SMBus address scanned for an SPD EEPROM.
#define AXL_SPD_ADDR_FIRST       0x50

/// Last SMBus address scanned for an SPD EEPROM (inclusive).
#define AXL_SPD_ADDR_LAST        0x57

/// Maximum part-number length across DDR3/4/5 (DDR5 is 30 ASCII chars).
#define AXL_SPD_PART_NUMBER_MAX  31  /* 30 chars + NUL */

/// Maximum raw SPD payload size (DDR5 = 1024 bytes; DDR3/4 = 256/512).
#define AXL_SPD_RAW_MAX          1024

// ---------------------------------------------------------------------------
// Decoded info
// ---------------------------------------------------------------------------

/**
 * @brief Decoded module identification block.
 *
 * Populated by axl_spd_read or axl_spd_decode. Caller-owned
 * (no allocations); zero-initialise before passing in.
 *
 * @c mfg_code_module and @c mfg_code_dram are packed JEP-106 codes:
 * high byte is the continuation-bank index (0-based), low byte is the
 * position within that bank. Look up the human-readable name via a
 * vendor table at the tool layer.
 */
typedef struct {
    uint8_t   ddr_generation;        ///< 4 = DDR4, 5 = DDR5; 0 = unknown
    uint16_t  mfg_code_module;       ///< JEP-106 (bank<<8 | id) for module manufacturer; 0 if unset
    uint16_t  mfg_code_dram;         ///< JEP-106 for DRAM die manufacturer; 0 if unset
    uint8_t   mfg_location;          ///< Vendor-defined site code
    uint16_t  mfg_year;              ///< Four-digit year (2000 + BCD year byte); 0 if unset
    uint8_t   mfg_week;              ///< ISO week (1..53) decoded from BCD
    uint32_t  serial;                ///< 4 bytes, big-endian on the wire
    char      part_number[AXL_SPD_PART_NUMBER_MAX]; ///< ASCII, NUL-terminated, trimmed
    uint64_t  capacity_bytes;        ///< Module capacity in bytes; 0 if not decodable
    uint16_t  speed_mts;             ///< JEDEC speed grade (MT/s); 0 if not decodable
    bool      has_ecc;               ///< True if the module exposes ECC bits
    bool      registered;            ///< True for RDIMM / LRDIMM
} AxlSpdInfo;

// ---------------------------------------------------------------------------
// Iteration + read
// ---------------------------------------------------------------------------

/**
 * @brief Walk populated SPD slots on the platform SMBus.
 *
 * Probes 0x50..0x57 in order and returns each address that responds
 * with a valid memory-type byte (DDR4=0x0C, DDR5=0x12). The returned
 * pointer references an internal static cursor; pass NULL to restart,
 * or the previous non-NULL return value to advance. The caller never
 * owns the cursor's storage.
 *
 * Lazy: opens an AxlSmbus session on first call.
 *
 * @return pointer to the next populated SMBus address, or NULL when
 *     enumeration is complete (or no SMBus controller is available).
 */
uint8_t *
axl_spd_next(
    uint8_t  *prev   ///< previous result, or NULL to start
);

/**
 * @brief Read and decode the SPD at a specific SMBus address.
 *
 * Issues the right sequence of SMBus byte reads for the device's
 * generation (auto-detected from the memory-type byte at offset 2),
 * including SPD5118 page selection for DDR5 modules. On success
 * @p out is populated; on failure @p out is left in an unspecified
 * state.
 *
 * @return AXL_OK on success (an unrecognised memory type also returns AXL_OK
 *     with @p out zeroed, ddr_generation = 0); AXL_INVALID on NULL @p out,
 *     AXL_UNSUPPORTED if no SMBus controller is present, AXL_NOT_FOUND if the
 *     slot is empty.
 */
AxlStatus
axl_spd_read(
    uint8_t      addr,   ///< 7-bit SMBus address (0x50..0x57)
    AxlSpdInfo  *out     ///< receives the decoded SPD on success
);

/**
 * @brief Read raw SPD bytes for offline analysis.
 *
 * For DDR4 reads the lower 256 bytes (or 512 if @p cap allows and the
 * device supports it). For DDR5 reads up to 1024 bytes across all
 * eight 128-byte pages, switching pages via MR11 as needed.
 *
 * The buffer can later be fed to axl_spd_decode to obtain the
 * same decoded view as axl_spd_read — useful for capturing SPDs
 * on real hardware and decoding them off-box.
 *
 * @param addr  7-bit SMBus address.
 * @param buf   output buffer.
 * @param cap   buffer capacity in bytes.
 * @param len   (out) bytes actually written.
 *
 * @return AXL_OK on success; AXL_INVALID on bad args, AXL_UNSUPPORTED if no
 *     SMBus controller is present, AXL_NOT_FOUND if the slot is empty.
 */
AxlStatus
axl_spd_dump_raw(
    uint8_t   addr,
    uint8_t  *buf,
    size_t    cap,
    size_t   *len
);

/**
 * @brief Decode an SPD buffer captured from a real DIMM.
 *
 * Pure function — no SMBus, no allocations. Branches on @c buf[2]
 * (the memory-type byte) to select the DDR4 or DDR5 codec.
 *
 * @param buf   raw SPD bytes (256+ for DDR4, 1024 for full DDR5).
 * @param len   bytes available in @p buf.
 * @param out   (out) decoded info; zero-initialised by this call.
 *
 * @return AXL_OK on success (an unrecognised memory type also returns AXL_OK
 *     with @p out zeroed, ddr_generation = 0); AXL_INVALID on NULL args or a
 *     buffer shorter than 3 bytes; otherwise the DDR4/DDR5 decoder's status.
 */
AxlStatus
axl_spd_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
);

// ---------------------------------------------------------------------------
// JEDEC vendor-name database (JSON5 sidecar)
// ---------------------------------------------------------------------------

/// Maximum bytes (including NUL) any vendor-name lookup can return.
/// Sized for the longest real JEP-106 entry plus headroom.
#define AXL_SPD_VENDOR_NAME_MAX  64u

/// Maximum bytes (including NUL) for axl_spd_ids_format_name output.
/// Vendor name plus the "<unknown>" → "0xCCCC" numeric fallback fit.
#define AXL_SPD_NAME_COMPOSED_MAX  80u

/**
 * @brief Opaque handle to a loaded JEDEC vendor-name database.
 *
 * Created by axl_spd_ids_open or
 * axl_spd_ids_open_from_buffer; destroyed by
 * axl_spd_ids_close. Multiple handles can coexist — a consumer
 * that ships an internal OEM sheet on top of the public set loads
 * two handles and queries them in priority order, mirroring
 * AxlPciIds.
 *
 * The process-global API (axl_spd_ids_load and friends) wraps
 * a single internal handle for the common case.
 */
typedef struct AxlSpdIds AxlSpdIds;

/**
 * @brief Open a JEDEC vendor database from a JSON5 file.
 *
 * @return @c AXL_SIDECAR_OK on success (handle returned via @p out),
 *     @c AXL_SIDECAR_FILE_MISSING if @p path does not exist,
 *     @c AXL_SIDECAR_PARSE_ERROR on JSON5 / schema rejection.
 */
AxlSidecarStatus
axl_spd_ids_open(
    const char   *path,
    AxlSpdIds   **out
);

/**
 * @brief Open a JEDEC vendor database from an in-memory JSON5 buffer.
 *
 * @return @c AXL_SIDECAR_OK on success, @c AXL_SIDECAR_PARSE_ERROR
 *     on parse / schema error. (No @c FILE_MISSING — the buffer is
 *     the input.)
 */
AxlSidecarStatus
axl_spd_ids_open_from_buffer(
    const char   *json5,
    size_t        len,
    AxlSpdIds   **out
);

/**
 * @brief Free a database handle. NULL-safe.
 */
void
axl_spd_ids_close(
    AxlSpdIds  *ids
);

/**
 * @brief Vendor lookup against an explicit handle.
 * @return database-owned string or NULL if unknown / handle empty.
 */
const char *
axl_spd_ids_vendor_name(
    const AxlSpdIds  *ids,     ///< handle from axl_spd_ids_load
    uint16_t          code     ///< packed JEP-106 (bank<<8 | id)
);

/**
 * @name Database iteration
 * @brief Walk every vendor entry. Non-zero callback return stops
 *     iteration and propagates as the iter rc.
 * @{
 */
typedef int (*AxlSpdIdsVendorFn)(uint16_t code, const char *name, void *ctx);

int
axl_spd_ids_foreach_vendor(
    const AxlSpdIds   *ids,
    AxlSpdIdsVendorFn  fn,
    void              *ctx
);
/** @} */

/**
 * @brief Compose a "vendor name or numeric fallback" display string.
 *
 * Centralizes the rendering convention so every consumer prints the
 * same string for the same JEP-106 code:
 *   - vendor known   → `"<vendor>"`
 *   - vendor unknown → `"0xCCCC"` (uppercase 4-digit hex)
 *
 * @return number of bytes written excluding NUL (snprintf shape),
 *     or -1 on bad arguments.
 */
int
axl_spd_ids_format_name(
    const AxlSpdIds  *ids,
    uint16_t          code,
    char             *buf,
    size_t            buflen
);

// ---------------------------------------------------------------------------
// Process-global singleton
// ---------------------------------------------------------------------------

/**
 * @brief Load the curated JEDEC database into the process-global slot.
 *
 * Two modes selected by @p override_path:
 *   - **Explicit** (`override_path` non-NULL): use exactly that path.
 *   - **Autodiscover** (`override_path` NULL): try `jedec.json5`
 *     next to the running .efi, then in the current working
 *     directory.
 *
 * Idempotent: a successful load is a no-op on subsequent calls. On
 * the first successful load, registers an axl_atexit cleanup
 * so the parsed table is freed at runtime cleanup automatically.
 */
AxlSidecarStatus
axl_spd_ids_load(
    const char  *override_path
);

/**
 * @brief Free the loaded database. Safe to call when none is loaded.
 */
void
axl_spd_ids_free(
    void
);

/**
 * @brief Singleton-backed vendor lookup.
 * @return database-owned string or NULL if no database loaded or
 *     @p code is not present.
 */
const char *
axl_spd_vendor_name(
    uint16_t  code
);

/**
 * @brief Singleton-backed convenience wrapper for
 *     axl_spd_ids_format_name.
 */
int
axl_spd_format_name(
    uint16_t  code,
    char     *buf,
    size_t    buflen
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SPD_H */
