/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi.h
    ACPI table discovery and typed readers.

    Locates ACPI tables published in the firmware's configuration
    table, walks the RSDT (ACPI 1.0) or XSDT (ACPI 2.0+) automatically,
    and returns header pointers identity-mapped into the running
    address space. Cursor-style iteration matches the SMBIOS module:

    @code
    AxlAcpiHeader *h = NULL;
    while ((h = axl_acpi_find_next("APIC", h)) != NULL) {
        // process each MADT (rare to have more than one, but correct)
    }
    @endcode

    Typed readers populate caller-allocated structs for the small
    set of tables the SDK consumes directly: MCFG (PCIe ECAM),
    MADT (interrupt controllers), and FACP/FADT (fixed-feature
    pointers). Other tables stay raw — consumers can walk them via
    the `AxlAcpiHeader` cursor and the table's own definitions.

    Scope is discovery + typed readers; AML interpretation is out
    of scope (that's ACPICA-sized and not what diagnostic tools
    need).
**/

#ifndef AXL_ACPI_H
#define AXL_ACPI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Standard ACPI table header
// ---------------------------------------------------------------------------

/**
 * @brief Common ACPI table header (System Description Table).
 *
 * Every ACPI table starts with this 36-byte header. None of the
 * fixed-size character arrays are nul-terminated.
 */
typedef struct {
    char     signature[4];        ///< table signature, NOT nul-terminated
    uint32_t length;              ///< total table length in bytes (incl. header)
    uint8_t  revision;            ///< table-specific revision
    uint8_t  checksum;            ///< whole-table sum mod 256 must equal 0
    char     oem_id[6];           ///< NOT nul-terminated
    char     oem_table_id[8];     ///< NOT nul-terminated
    uint32_t oem_revision;
    char     creator_id[4];       ///< NOT nul-terminated
    uint32_t creator_revision;
} AxlAcpiHeader;

// ---------------------------------------------------------------------------
// Discovery + iteration
// ---------------------------------------------------------------------------

/**
 * @brief Find the first ACPI table with the given 4-byte signature.
 *
 * `_find()` resolves RSDT versus XSDT internally based on the RSDP
 * revision the firmware published — consumers don't choose.
 *
 * @return pointer to the table header, or NULL if not found.
 */
AxlAcpiHeader *
axl_acpi_find(
    const char  sig[4]   ///< 4-byte signature, NOT nul-terminated (e.g. {'M','C','F','G'})
);

/**
 * @brief Find the next ACPI table with @p sig after @p prev.
 *
 * Pass NULL as @p prev to find the first (same as axl_acpi_find).
 * Use in a loop to enumerate all tables sharing a signature.
 *
 * @return pointer to next table header, or NULL if no more.
 */
AxlAcpiHeader *
axl_acpi_find_next(
    const char      sig[4],   ///< 4-byte signature, NOT nul-terminated
    AxlAcpiHeader  *prev      ///< previous result, or NULL to start
);

/**
 * @brief Iterate every ACPI table regardless of signature.
 *
 * Pass NULL for the first call; pass the previous result for
 * subsequent calls. Returns NULL when there are no more.
 *
 * @return pointer to next table header, or NULL if no more.
 */
AxlAcpiHeader *
axl_acpi_next(
    AxlAcpiHeader  *prev   ///< previous result, or NULL to start
);

/**
 * @brief Get the RSDP revision the firmware published.
 *
 * 0 = ACPI 1.0 (32-bit RSDT pointers only).
 * 2+ = ACPI 2.0 or later (64-bit XSDT preferred when present).
 *
 * @return AXL_OK on success, AXL_ERR if RSDP is not available.
 */
int
axl_acpi_revision(
    uint8_t  *rev   ///< [out] receives RSDP revision byte
);

/**
 * @brief Verify a table's whole-table checksum.
 *
 * Computes the unsigned-byte sum across @c h->length bytes; valid
 * tables sum to 0 modulo 256. Useful before trusting the typed
 * reader output on suspect firmware.
 *
 * @return true if the checksum is valid; false if @p h is NULL,
 *     the length field is too small to be a valid table, or the
 *     bytes don't sum to zero.
 */
bool
axl_acpi_checksum_ok(
    const AxlAcpiHeader  *h   ///< table header
);

// ---------------------------------------------------------------------------
// Typed readers — MCFG (PCIe ECAM)
// ---------------------------------------------------------------------------

/// Maximum PCI segments captured from MCFG. Multi-segment platforms
/// rarely exceed a handful; 16 is room to spare.
#define AXL_ACPI_MCFG_MAX_SEGMENTS  16

/**
 * @brief One MCFG configuration-space allocation entry.
 */
typedef struct {
    uint64_t  base_addr;   ///< ECAM base physical address for this segment
    uint16_t  segment;     ///< PCI segment group number
    uint8_t   start_bus;   ///< first bus number covered (inclusive)
    uint8_t   end_bus;     ///< last bus number covered (inclusive)
} AxlAcpiMcfgEntry;

/**
 * @brief Decoded MCFG table.
 */
typedef struct {
    size_t            count;                                  ///< populated entries
    AxlAcpiMcfgEntry  segments[AXL_ACPI_MCFG_MAX_SEGMENTS];
} AxlAcpiMcfg;

/**
 * @brief Read and decode the MCFG table.
 *
 * @return AXL_OK on success, AXL_ERR if MCFG is missing or malformed.
 */
int
axl_acpi_read_mcfg(
    AxlAcpiMcfg  *out   ///< [out] receives decoded MCFG
);

// ---------------------------------------------------------------------------
// Typed readers — MADT (interrupt controllers)
// ---------------------------------------------------------------------------

/// Caps on per-arch MADT entries we capture. Platforms with more
/// stuff still get the first N.
#define AXL_ACPI_MADT_MAX_IOAPICS    16
#define AXL_ACPI_MADT_MAX_GIC_REGS    8

/**
 * @brief x86 IOAPIC entry (MADT subtable type 1).
 */
typedef struct {
    uint8_t   id;                ///< IOAPIC ID
    uint32_t  addr;              ///< IOAPIC physical address
    uint32_t  global_irq_base;   ///< first global system interrupt the IOAPIC covers
} AxlAcpiIoapic;

/**
 * @brief AArch64 GIC redistributor / distributor region (MADT subtable
 *     types 12 = GICD, 14 = GICR).
 */
typedef struct {
    uint8_t   subtable_type;     ///< 12=GICD, 14=GICR
    uint64_t  addr;              ///< region physical base
    uint32_t  length;            ///< region length (GICR only; GICD is a fixed 64KiB)
} AxlAcpiGicRegion;

/**
 * @brief Decoded MADT table.
 *
 * x86 firmware populates `ioapics`; AArch64 firmware populates
 * `gic_regions`. Diagnostic code checks which set is non-empty for
 * the running arch; the unused side stays zero.
 */
typedef struct {
    uint32_t          local_apic_addr;   ///< x86 LAPIC base (0 on AArch64)
    uint32_t          flags;             ///< MADT flags field
    size_t            ioapic_count;
    AxlAcpiIoapic     ioapics[AXL_ACPI_MADT_MAX_IOAPICS];
    size_t            gic_region_count;
    AxlAcpiGicRegion  gic_regions[AXL_ACPI_MADT_MAX_GIC_REGS];
} AxlAcpiMadt;

/**
 * @brief Read and decode the MADT (APIC) table.
 *
 * @return AXL_OK on success, AXL_ERR if MADT is missing or malformed.
 */
int
axl_acpi_read_madt(
    AxlAcpiMadt  *out   ///< [out] receives decoded MADT
);

// ---------------------------------------------------------------------------
// Typed readers — FACP/FADT (fixed-feature pointers)
// ---------------------------------------------------------------------------

/**
 * @brief Decoded FADT (Fixed ACPI Description Table).
 *
 * Captures the small set of fields diagnostic code reaches for —
 * SMI port + ACPI enable/disable values for power-state interaction,
 * PM1 event/control block addresses for SCI handling, DSDT pointer
 * for callers that walk AML themselves. The full FADT has many more
 * fields; consumers needing them can read the table directly via
 * `axl_acpi_find("FACP", NULL)`.
 */
typedef struct {
    uint32_t  firmware_ctrl;    ///< 32-bit FACS pointer (legacy field)
    uint64_t  x_firmware_ctrl;  ///< 64-bit FACS pointer (ACPI 2.0+, 0 if absent)
    uint32_t  dsdt;             ///< 32-bit DSDT pointer (legacy field)
    uint64_t  x_dsdt;           ///< 64-bit DSDT pointer (ACPI 2.0+, 0 if absent)
    uint32_t  smi_cmd;          ///< SMI command port
    uint8_t   acpi_enable;      ///< value to write to smi_cmd to enable ACPI mode
    uint8_t   acpi_disable;     ///< value to write to smi_cmd to disable ACPI mode
    uint16_t  pm1a_evt_blk;     ///< PM1a event block port
    uint16_t  pm1b_evt_blk;     ///< PM1b event block port (0 if not present)
    uint16_t  pm1a_cnt_blk;     ///< PM1a control block port
    uint16_t  pm1b_cnt_blk;     ///< PM1b control block port (0 if not present)
    uint8_t   pm1_evt_len;      ///< length of PM1 event block in bytes
    uint8_t   pm1_cnt_len;      ///< length of PM1 control block in bytes
    uint16_t  iapc_boot_arch;   ///< IA-PC boot architecture flags (legacy enables)
    uint16_t  arm_boot_arch;    ///< ARM boot architecture flags (PSCI capabilities)
} AxlAcpiFacp;

/**
 * @brief Read and decode the FADT.
 *
 * @return AXL_OK on success, AXL_ERR if the table is missing or malformed.
 */
int
axl_acpi_read_facp(
    AxlAcpiFacp  *out   ///< [out] receives decoded FADT
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ACPI_H */
