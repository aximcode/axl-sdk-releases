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
 * 16-bit segment matches the UEFI MCFG / PCIe spec width so multi-
 * segment platforms are addressable directly — every lookup, walk,
 * and find helper takes segment as part of the address tuple.
 * Single-segment systems leave it at 0. Bus / dev / func are the
 * standard 8/5/3-bit fields.
 */
typedef struct {
    uint16_t  seg;    ///< PCI segment group
    uint8_t   bus;    ///< bus number (0..255)
    uint8_t   dev;    ///< device number (0..31)
    uint8_t   func;   ///< function number (0..7)
} AxlPciAddr;

/// Buffer size that fits the canonical "SSSS:BB:DD.F" form plus NUL.
#define AXL_PCI_ADDR_STR_MAX  16

/**
 * @brief Parse a textual PCI address into an AxlPciAddr.
 *
 * Accepts two hex-only formats:
 *   - `bus:dev.func`             — segment defaults to 0
 *   - `seg:bus:dev.func`         — explicit segment
 *
 * Components are bounded at parse time (bus 0..0xFF, dev 0..0x1F,
 * func 0..0x07, seg 0..0xFFFF); out-of-range or malformed input
 * returns -1 with @p out left unmodified.
 *
 * @return 0 on success, -1 on malformed input.
 */
int
axl_pci_addr_parse(
    const char  *s,    ///< input string (NUL-terminated, hex digits + `:` + `.`)
    AxlPciAddr  *out   ///< [out] parsed address (untouched on error)
);

/**
 * @brief Write an AxlPciAddr in canonical `SSSS:BB:DD.F` form.
 *
 * Always emits the 4-digit segment — round-trips with @ref
 * axl_pci_addr_parse. NUL-terminates @p buf when @p buflen >= 13.
 *
 * @return number of bytes written excluding NUL, or -1 if @p buflen
 *     is too small (need >= AXL_PCI_ADDR_STR_MAX).
 */
int
axl_pci_addr_format(
    AxlPciAddr  addr,    ///< address to format
    char       *buf,     ///< destination buffer
    size_t      buflen   ///< capacity of @p buf
);

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

/// Capacity of a function's full PCIe ECAM config space.
#define AXL_PCI_CONFIG_SPACE_MAX  4096

/**
 * @brief Read up to @p bytes of PCI configuration space into @p buf.
 *
 * Walks the function's config space in 32-bit ECAM-natural chunks
 * (little-endian on the wire, packed into @p buf at offsets 0..bytes).
 * @p bytes is rounded down to a multiple of 4 and capped at
 * @ref AXL_PCI_CONFIG_SPACE_MAX. The function is treated as **absent**
 * if zero successful reads occur (vendor ID at offset 0x00 reads as
 * 0xFFFFFFFF) — returns -1 with @c *out_read = 0. On a partial dump
 * (some reads succeeded, some failed), @c *out_read tracks how many
 * bytes the caller can safely consume; the remaining buffer bytes are
 * zeroed.
 *
 * Replaces the per-tool hand-rolled `for (reg = 0; reg + 4 <= bytes;
 * reg += 4) read32; pack into buf` loop.
 *
 * @return 0 on success (one or more successful reads), -1 if the
 *     function is absent or @p buf is NULL or MCFG isn't available.
 */
int
axl_pci_dump(
    AxlPciAddr   addr,       ///< target function
    uint8_t     *buf,        ///< destination buffer
    size_t       bytes,      ///< capacity (rounded down to 4, capped at AXL_PCI_CONFIG_SPACE_MAX)
    size_t      *out_read    ///< [out, optional] bytes successfully populated
);

// ---------------------------------------------------------------------------
// Common header reads (boilerplate-killer wrappers)
// ---------------------------------------------------------------------------

/**
 * @brief Read vendor ID and device ID from a function's standard header.
 *
 * Reads offsets 0x00 and 0x02. The "function absent" sentinel
 * (vendor ID == 0xFFFF) is folded into the return code so callers
 * don't have to special-case it.
 *
 * @return 0 on success (both fields populated), -1 if the function
 *     is absent or any bus error is encountered.
 */
int
axl_pci_get_vid_did(
    AxlPciAddr   addr,   ///< target function
    uint16_t    *vid,    ///< [out] vendor ID
    uint16_t    *did     ///< [out] device ID
);

/**
 * @brief Read the 24-bit class code from a function's standard header.
 *
 * Folds the three bytes at offsets 0x09 (programming interface),
 * 0x0A (subclass), and 0x0B (base class) into the canonical
 * `(base << 16) | (sub << 8) | prog_if` form — same shape consumed
 * by @ref axl_pci_find_by_class.
 *
 * @return 0 on success, -1 on bus error.
 */
int
axl_pci_get_class_code(
    AxlPciAddr   addr,    ///< target function
    uint32_t    *class_code  ///< [out] 24-bit class code
);

/**
 * @brief Format a 24-bit PCI class code as a human-readable string.
 *
 * Decodes per the PCI Code and ID Assignment Specification — three
 * tiers: base class (`(class_code >> 16) & 0xFF`), subclass
 * (`(class_code >> 8) & 0xFF`), and programming interface
 * (`class_code & 0xFF`). Output shape: `"<base> / <sub> / <prog>"`
 * (e.g. `"Display controller / VGA-compatible / standard"`); a tier
 * with no recognized name renders as `"<unknown>"`. Always
 * NUL-terminates @p buf (snprintf-shape).
 *
 * Vendor/device-name lookup (the `pci.ids` database) is intentionally
 * out of scope — too large for AXL, and consumers grep their own.
 *
 * @return number of bytes written excluding NUL, or -1 if @p buf is
 *     NULL or @p buflen is 0.
 */
int
axl_pci_class_string(
    uint32_t   class_code,   ///< 24-bit class code
    char      *buf,       ///< destination buffer
    size_t     buflen     ///< capacity of @p buf
);

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
    uint32_t     class_code,   ///< 24-bit class code
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

/**
 * @brief Walk every keyword in a function's VPD area and dispatch to
 *     a callback.
 *
 * Complements @ref axl_pci_vpd_read for tools that want "show me
 * everything that's there" rather than "fetch this specific
 * keyword." Visits both the Read-Only (PN/EC/SN/MN/RV/V0..V9/...)
 * and Read-Write (Y0..Y9/RW/...) resource sections in document
 * order. Vendor-specific keywords (V0..V9, Y0..Y9) reach the
 * callback alongside the standard ones.
 *
 * The data buffer passed to @p cb is owned by the implementation
 * and is only valid for the duration of the call — the callback
 * must copy bytes it wants to retain. Returning non-zero from
 * @p cb stops iteration; that value becomes the iter return.
 *
 * @return 0 if iteration completed without the callback stopping
 *     it, the callback's non-zero return if it stopped early, or
 *     -1 if VPD is unsupported or any bus error is encountered.
 */
int
axl_pci_vpd_iter(
    AxlPciAddr   addr,         ///< target function
    int        (*cb)(          ///< per-keyword callback
        const char     keyword[2],  ///< 2-char ASCII keyword
        const uint8_t *data,        ///< keyword data (impl-owned)
        size_t         len,         ///< data length in bytes
        void          *ctx          ///< caller's ctx
    ),
    void        *ctx           ///< opaque context forwarded to @p cb
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PCI_H */
