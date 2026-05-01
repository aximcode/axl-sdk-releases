/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci.h
    PCI/PCIe configuration-space access via ECAM.

    Configuration space is reached through the MCFG-described
    Enhanced Configuration Access Mechanism (ECAM) on both x86 and
    AArch64 — never via the legacy 0xCF8/0xCFC port pair. The MCFG
    discovery is lazy on first access; if the firmware did not
    publish an MCFG table, all `axl_pci_*` calls fail with -1
    rather than silently falling back to legacy ports.

    Cursor-style iteration mirrors `axl_smbios_find_next` and
    `axl_acpi_find_next`:

    @code
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        uint16_t vid;
        axl_pci_read_config_16(*p, 0x00, &vid);
        // ...
    }
    @endcode
**/

#ifndef AXL_PCI_H
#define AXL_PCI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Address tuple
// ---------------------------------------------------------------------------

/**
 * @brief A PCI configuration-space address (segment:bus:dev:func).
 *
 * 16-bit segment future-proofs against multi-segment platforms;
 * single-segment systems leave it at 0. Bus / dev / func are the
 * standard 8/5/3-bit fields.
 */
typedef struct {
    uint16_t  seg;    ///< PCI segment group
    uint8_t   bus;    ///< bus number (0..255)
    uint8_t   dev;    ///< device number (0..31)
    uint8_t   func;   ///< function number (0..7)
} AxlPciAddr;

// ---------------------------------------------------------------------------
// Config-space read/write
// ---------------------------------------------------------------------------

/**
 * @brief Read a byte from PCI configuration space.
 *
 * @return 0 on success, -1 if the address is outside any MCFG
 *     segment or MCFG isn't available.
 */
int
axl_pci_read_config_8(
    AxlPciAddr  addr,    ///< target function
    uint16_t    reg,     ///< register offset (0..4095 for ECAM)
    uint8_t    *out      ///< [out] receives the value
);

/// 16-bit variant of axl_pci_read_config_8. Register should be 16-bit aligned.
int
axl_pci_read_config_16(AxlPciAddr addr, uint16_t reg, uint16_t *out);

/// 32-bit variant of axl_pci_read_config_8. Register should be 32-bit aligned.
int
axl_pci_read_config_32(AxlPciAddr addr, uint16_t reg, uint32_t *out);

/// Write counterpart to axl_pci_read_config_8.
int
axl_pci_write_config_8 (AxlPciAddr addr, uint16_t reg, uint8_t  value);

/// Write counterpart to axl_pci_read_config_16.
int
axl_pci_write_config_16(AxlPciAddr addr, uint16_t reg, uint16_t value);

/// Write counterpart to axl_pci_read_config_32.
int
axl_pci_write_config_32(AxlPciAddr addr, uint16_t reg, uint32_t value);

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

/**
 * @brief Iterate every responding PCI function across all MCFG segments.
 *
 * Returns a pointer to a static internal cursor; the storage is
 * reused across calls and is invalidated by the next call. Pass
 * NULL to start the walk fresh, or the previous non-NULL return
 * value to advance — passing any other pointer (including a
 * caller-allocated AxlPciAddr) restarts iteration silently. The
 * caller never owns the cursor's storage.
 *
 * Empty slots (vendor ID 0xFFFF) are skipped. Single-function
 * devices are detected via the header-type byte and their
 * functions 1–7 are skipped.
 *
 * @return pointer to the next populated function, or NULL when
 *     enumeration is complete (or MCFG is unavailable).
 */
AxlPciAddr *
axl_pci_next(
    AxlPciAddr  *prev   ///< previous result, or NULL to start
);

/**
 * @brief Find the @p nth function with matching vendor+device IDs.
 *
 * @return 0 on success, -1 if no @p nth match exists.
 */
int
axl_pci_find_by_vid_did(
    uint16_t     vid,    ///< vendor ID
    uint16_t     did,    ///< device ID
    uint16_t     nth,    ///< 0-based match index
    AxlPciAddr  *out     ///< [out] address of the matching function
);

/**
 * @brief Find the @p nth function with a matching class triplet.
 *
 * The 24-bit class is `(base_class << 16) | (subclass << 8) | prog_if`,
 * matching how `lspci -vvv` prints it. Pass `0xFFFFFF` to match any.
 *
 * @return 0 on success, -1 if no @p nth match exists.
 */
int
axl_pci_find_by_class(
    uint32_t     class24,   ///< 24-bit class code
    uint16_t     nth,       ///< 0-based match index
    AxlPciAddr  *out        ///< [out] address of the matching function
);

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/**
 * @brief Iterate the standard PCI capability list.
 *
 * Pass 0 in @p prev_off to start (the function's capability list
 * pointer at 0x34 is consulted automatically). On the first call
 * the function returns the first capability; on subsequent calls
 * pass @p out_off from the previous return value to advance.
 *
 * @return 0 on success (capability found), -1 when no more
 *     capabilities exist or the function has no capabilities.
 */
int
axl_pci_cap_next(
    AxlPciAddr  addr,        ///< target function
    uint16_t    prev_off,    ///< previous offset, or 0 to start
    uint16_t   *out_off,     ///< [out] offset of the next capability
    uint16_t   *out_id       ///< [out] capability ID
);

/**
 * @brief Iterate the PCIe extended capability list (offsets 0x100..).
 *
 * Same conventions as axl_pci_cap_next, but operates on the PCIe
 * extended capability chain. Returns -1 if the device is not
 * PCIe (no extended caps).
 *
 * @return 0 on success, -1 when the chain ends or no extended
 *     caps are present.
 */
int
axl_pci_ext_cap_next(
    AxlPciAddr  addr,        ///< target function
    uint16_t    prev_off,    ///< previous offset, or 0 to start
    uint16_t   *out_off,     ///< [out] offset of the next capability
    uint16_t   *out_id       ///< [out] extended capability ID
);

// ---------------------------------------------------------------------------
// VPD
// ---------------------------------------------------------------------------

/**
 * @brief Read a VPD keyword from a function's Vital Product Data area.
 *
 * Walks the VPD capability (PCI 3.0 §6.4) — keyword-tagged blocks
 * inside the Read-Only and Read/Write resource sections. Keyword
 * is exactly 2 ASCII characters (e.g. "PN" for part number,
 * "EC" for engineering change, "SN" for serial). The function
 * locates the matching keyword in either RO or RW area and copies
 * up to @p buflen bytes of its data into @p buf.
 *
 * @return 0 on success, -1 if VPD is unsupported, the keyword is
 *     not present, or any bus error is encountered. On success,
 *     @c *out_len is set to the keyword's actual data length
 *     (which may exceed @p buflen — in which case the buffer was
 *     truncated).
 */
int
axl_pci_vpd_read(
    AxlPciAddr   addr,         ///< target function
    const char   keyword[2],   ///< 2-char ASCII keyword (NOT nul-terminated)
    uint8_t     *buf,          ///< destination buffer
    size_t       buflen,       ///< capacity of @p buf
    size_t      *out_len       ///< [out] keyword's actual length
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PCI_H */
