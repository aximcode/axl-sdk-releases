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
 * @return AXL_OK on success, AXL_ERR on malformed input.
 */
int
axl_pci_addr_parse(
    const char  *s,    ///< input string (NUL-terminated, hex digits + `:` + `.`)
    AxlPciAddr  *out   ///< [out] parsed address (untouched on error)
);

/**
 * @brief Write an AxlPciAddr in canonical `SSSS:BB:DD.F` form.
 *
 * Always emits the 4-digit segment — round-trips with axl_pci_addr_parse. NUL-terminates @p buf when @p buflen >= 13.
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
 * @return AXL_OK on success, AXL_ERR if the address is outside any MCFG
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
 * AXL_PCI_CONFIG_SPACE_MAX. The function is treated as **absent**
 * if zero successful reads occur (vendor ID at offset 0x00 reads as
 * 0xFFFFFFFF) — returns -1 with @c *out_read = 0. On a partial dump
 * (some reads succeeded, some failed), @c *out_read tracks how many
 * bytes the caller can safely consume; the remaining buffer bytes are
 * zeroed.
 *
 * Replaces the per-tool hand-rolled `for (reg = 0; reg + 4 <= bytes;
 * reg += 4) read32; pack into buf` loop.
 *
 * @return AXL_OK on success (one or more successful reads), AXL_ERR if the
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
 * @return AXL_OK on success (both fields populated), AXL_ERR if the function
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
 * by axl_pci_find_by_class.
 *
 * @return AXL_OK on success, AXL_ERR on bus error.
 */
int
axl_pci_get_class_code(
    AxlPciAddr   addr,    ///< target function
    uint32_t    *class_code  ///< [out] 24-bit class code
);

/// PCI configuration-space header type, decoded from the low 7 bits
/// of offset 0x0E. The high bit (0x80) is the multi-function flag and
/// is exposed separately by axl_pci_get_header_type.
typedef enum {
    AXL_PCI_HEADER_TYPE_NORMAL  = 0x00,  ///< Type 0: regular function (BARs, SVID/SDID, etc.)
    AXL_PCI_HEADER_TYPE_BRIDGE  = 0x01,  ///< Type 1: PCI-PCI bridge
    AXL_PCI_HEADER_TYPE_CARDBUS = 0x02,  ///< Type 2: CardBus bridge
} AxlPciHeaderType;

/// PCI base-class code for network controllers (the high byte of the
/// 24-bit class). A function whose `base_class` is this is a NIC / network
/// device; consumers (netcfg, netinfo) match on it instead of a literal.
#define AXL_PCI_CLASS_NETWORK  0x02

/**
 * @brief Read the configuration-space header type and multi-function bit.
 *
 * Splits the byte at offset 0x0E into the type enum (low 7 bits) and
 * the multi-function flag (bit 7). Eliminates the manual `& 0x7F`
 * masking and `& 0x80` bit test that every consumer rolling its own
 * type detection writes. Either out parameter may be NULL.
 *
 * If the firmware reports a header-type byte the spec doesn't define
 * (anything outside 0x00..0x02 in the low 7 bits) the call still
 * returns 0 and @p type is set to the raw value cast through the
 * enum — callers can compare against the named constants and treat
 * unknown values as opaque. Bus error returns -1.
 *
 * @return AXL_OK on success, AXL_ERR on bus error.
 */
int
axl_pci_get_header_type(
    AxlPciAddr         addr,                 ///< target function
    AxlPciHeaderType  *type,                 ///< [out] header type (NULL allowed)
    bool              *is_multi_function     ///< [out] bit 7 of offset 0x0E (NULL allowed)
);

/**
 * @brief Read a Type 0 function's Subsystem Vendor ID and Subsystem ID.
 *
 * Only Type 0 functions (regular endpoints) carry SVID/SDID at config
 * offsets 0x2C / 0x2E; Type 1 (PCI-PCI bridge) and Type 2 (CardBus)
 * use those bytes for other purposes. The header-type check is baked
 * in: a non-zero header type returns -1 with @p svid / @p sdid
 * untouched.
 *
 * @return AXL_OK on success (both fields populated), AXL_ERR if the function
 *     is absent, has a non-Type-0 header, or any bus error is
 *     encountered.
 */
int
axl_pci_get_subsystem(
    AxlPciAddr   addr,    ///< target function (must be header-type 0)
    uint16_t    *svid,    ///< [out] subsystem vendor ID
    uint16_t    *sdid     ///< [out] subsystem device ID
);

/**
 * @brief Format a 24-bit PCI class code as a human-readable string.
 *
 * Decodes per the PCI Code and ID Assignment Specification — up to
 * three tiers: base class (`(class_code >> 16) & 0xFF`), subclass
 * (`(class_code >> 8) & 0xFF`), and programming interface
 * (`class_code & 0xFF`). Tiers with no spec-defined name are
 * omitted rather than rendered as `<unknown>` placeholders. This
 * mirrors Linux lspci's *posture* (no placeholder noise) but not
 * its output shape — lspci collapses the triplet to a single
 * subclass string ("Host bridge"), while AXL keeps the slash-joined
 * triplet so the base class stays visible. Output shapes:
 *
 *   - All known: `"<base> / <sub> / <prog>"`
 *     (e.g. `"Display controller / VGA-compatible / standard"`)
 *   - Known base+sub, unknown prog: `"<base> / <sub>"`
 *     (e.g. `"Bridge / Host bridge"`)
 *   - Known base, unknown sub: `"<base>"`
 *     (e.g. `"Bridge"`)
 *   - Wholly unknown class: `"Class XXXXXX"` (numeric hex), in the
 *     spirit of lspci's numeric fallback for unidentified classes.
 *
 * Always NUL-terminates @p buf (snprintf-shape).
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

/**
 * @brief Output shape selector for axl_pci_class_string_fmt.
 *
 * Different consumers want different verbosity from the same class
 * code. Verbose tools want the full slash-joined triplet; row-oriented
 * tools where the class column blows out the right margin want the
 * subclass alone (matches Linux lspci's output shape); coarse
 * categorization wants just the base.
 */
typedef enum {
    AXL_PCI_CLASS_FMT_FULL     = 0,  ///< "Bridge / Host bridge" (axl_pci_class_string default)
    AXL_PCI_CLASS_FMT_SUBCLASS = 1,  ///< "Host bridge" (subclass tier alone)
    AXL_PCI_CLASS_FMT_BASE     = 2,  ///< "Bridge" (base tier alone)
} AxlPciClassFmt;

/**
 * @brief Format a class code with a chosen output shape.
 *
 * Behavior in each mode follows the same "omit unknown tiers,
 * fall back to numeric `Class XXXXXX` when wholly unknown" posture
 * as axl_pci_class_string:
 *
 *   - `FMT_FULL` is identical to axl_pci_class_string.
 *   - `FMT_SUBCLASS` emits just the subclass name. If the subclass
 *     isn't in the table, falls back to the base name; if the base
 *     is also unknown, falls back to numeric.
 *   - `FMT_BASE` emits just the base name. If unknown, numeric
 *     fallback.
 *
 * @return number of bytes written excluding NUL, or -1 on bad args
 *     or unknown @p fmt.
 */
int
axl_pci_class_string_fmt(
    uint32_t        class_code,
    AxlPciClassFmt  fmt,
    char           *buf,
    size_t          buflen
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
 * Empty slots are skipped: both vendor ID 0xFFFF (the bus "no
 * device" sentinel) and 0x0000 (a reserved vendor ID — some chipsets
 * return all-zero config reads for disconnected slots, producing
 * "phantom" 0000:0000 devices). Single-function devices are detected
 * via the header-type byte and their functions 1–7 are skipped.
 *
 * Use @ref axl_pci_next_unfiltered if you need to see 0x0000 slots.
 *
 * @return pointer to the next populated function, or NULL when
 *     enumeration is complete (or MCFG is unavailable).
 */
AxlPciAddr *
axl_pci_next(
    AxlPciAddr  *prev   ///< previous result, or NULL to start
);

/**
 * @brief Like @ref axl_pci_next, but does NOT skip 0x0000 phantom
 *     slots (only 0xFFFF absent slots are skipped).
 *
 * Opt-in for the rare consumer that must enumerate raw config space
 * including slots a quirky chipset reports as 0000:0000. Most callers
 * want @ref axl_pci_next, which filters phantoms by default. Shares
 * the same static cursor as @ref axl_pci_next — do not interleave the
 * two within a single walk.
 *
 * @return pointer to the next responding function (vendor ID
 *     != 0xFFFF), or NULL when enumeration is complete.
 */
AxlPciAddr *
axl_pci_next_unfiltered(
    AxlPciAddr  *prev   ///< previous result, or NULL to start
);

/**
 * @brief Find the @p nth function with matching vendor+device IDs.
 *
 * @return AXL_OK on success, AXL_ERR if no @p nth match exists.
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
 * @return AXL_OK on success, AXL_ERR if no @p nth match exists.
 */
int
axl_pci_find_by_class(
    uint32_t     class_code,   ///< 24-bit class code
    uint16_t     nth,       ///< 0-based match index
    AxlPciAddr  *out        ///< [out] address of the matching function
);

// ---------------------------------------------------------------------------
// Bridges and topology
// ---------------------------------------------------------------------------

/**
 * @brief Per-bridge bus-number tuple.
 *
 * For a PCI-PCI bridge function (header type 1), these three bytes
 * live at config-space offsets 0x18 / 0x19 / 0x1A. The bridge claims
 * config-space transactions for buses in the inclusive range
 * `[secondary, subordinate]` and forwards them downstream.
 */
typedef struct {
    uint8_t  primary;       ///< upstream bus the bridge sits on
    uint8_t  secondary;     ///< first bus on the downstream side
    uint8_t  subordinate;   ///< highest bus number behind this bridge
} AxlPciBridge;

/**
 * @brief Read the bridge bus tuple, if @p addr is a PCI-PCI bridge.
 *
 * Reads the header-type byte first; on a non-bridge function (type 0
 * endpoint or type 2 CardBus), returns -1 without touching @p out.
 * Successful return guarantees @p addr is header type 1 and the
 * three bus-number bytes are populated.
 *
 * @return AXL_OK on success, AXL_ERR if @p addr is not a PCI-PCI bridge or any
 *     bus error is encountered.
 */
int
axl_pci_bridge_info(
    AxlPciAddr      addr,   ///< target function
    AxlPciBridge   *out     ///< [out] primary/secondary/subordinate
);

#define AXL_PCI_TREE_MAX_DEPTH  16u  ///< depth backstop for tree walks

/**
 * @brief Per-node callback for axl_pci_tree_for_each.
 *
 * @param addr      function being visited
 * @param depth     0 for root-bus devices, 1 for first-level bridge
 *                  children, etc.
 * @param is_bridge `true` if the function is a PCI-PCI bridge whose
 *                  secondary bus is about to be descended into
 * @param ctx       caller's opaque context
 *
 * @return non-zero to stop the walk early; the value becomes the
 *     return of axl_pci_tree_for_each. Return 0 to continue.
 */
typedef int (*AxlPciTreeFn)(
    AxlPciAddr  addr,
    unsigned    depth,
    bool        is_bridge,
    void       *ctx
);

/**
 * @brief Walk the PCI topology in tree order, depth-first per segment.
 *
 * Builds an in-memory model of the topology by enumerating every
 * responding function once via axl_pci_next, then identifying
 * root buses per segment (any bus that's not the secondary bus of
 * some bridge) and recursing through bridges. Functions on the same
 * bus are visited in `(dev, func)` order; bridge children are
 * visited immediately after their bridge.
 *
 * Multi-segment platforms are walked one segment at a time; segments
 * are visited in MCFG-table order.
 *
 * Defensive against malformed or hostile topologies: per-segment
 * visited-bus bitmaps detect cycles, and a recursion-depth cap
 * (`AXL_PCI_TREE_MAX_DEPTH`) backstops pathological chains. Same
 * posture as `AXL_DP_MAX_NODES` for device-path iteration and the
 * cap-walk self-loop / offset-range guards (which allow descending
 * cap chains but still terminate on malformed ones).
 *
 * Not reentrant against axl_pci_next — the walker drives
 * `axl_pci_next` internally and they share a static cursor. The
 * callback must not call `axl_pci_next` (the tree walk itself uses
 * `axl_pci_*` config-space reads, which are fine).
 *
 * @return 0 on a clean walk, the callback's first non-zero return
 *     if it stopped early, or -1 if MCFG is unavailable / any
 *     internal allocation fails.
 */
int
axl_pci_tree_for_each(
    AxlPciTreeFn  fn,    ///< per-node callback (must not be NULL)
    void         *ctx    ///< opaque context forwarded to @p fn
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
 * @return AXL_OK on success (capability found), AXL_ERR when no more
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
 * @return AXL_OK on success, AXL_ERR when the chain ends or no extended
 *     caps are present.
 */
int
axl_pci_ext_cap_next(
    AxlPciAddr  addr,        ///< target function
    uint16_t    prev_off,    ///< previous offset, or 0 to start
    uint16_t   *out_off,     ///< [out] offset of the next capability
    uint16_t   *out_id       ///< [out] extended capability ID
);

/**
 * @brief Look up a human-readable name for a legacy PCI capability ID.
 *
 * Covers the standard IDs from the PCI Local Bus Specification (PM,
 * AGP, VPD, Slot ID, MSI, CompactPCI HotSwap, PCI-X, HyperTransport,
 * Vendor-Specific, Debug, CompactPCI Resource, PCI HotPlug,
 * Bridge Subsystem ID, AGP 8x, Secure, PCI Express, MSI-X, SATA,
 * Advanced Features, Enhanced Allocation, FPB).
 *
 * @return A pointer to a static string. Always non-NULL — unknown
 *     IDs return "<unknown>".
 */
const char *
axl_pci_cap_id_str(
    uint8_t  cap_id          ///< legacy capability ID (8 bits)
);

/**
 * @brief Look up a human-readable name for a PCIe extended capability ID.
 *
 * Covers the standard IDs from PCIe Base Specification — AER, Virtual
 * Channel, Serial Number, Power Budgeting, ACS, ATS, SR-IOV, MR-IOV,
 * Multicast, Resizable BAR, DPA, TPH, LTR, Secondary PCIe, PMUX, PASID,
 * LNR, DPC, L1 PM Substates, PTM, Frame Capability, ReadyToReset,
 * Designated Vendor-Specific, VF Resizable BAR, Data Link Feature,
 * Physical Layer 16/32 GT/s, Lane Margining, Hierarchy ID, NPEM, etc.
 *
 * @return A pointer to a static string. Always non-NULL — unknown
 *     IDs return "<unknown>".
 */
const char *
axl_pci_ext_cap_id_str(
    uint16_t  cap_id         ///< extended capability ID (16 bits)
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
 * @return AXL_OK on success, AXL_ERR if VPD is unsupported, the keyword is
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
 * Complements axl_pci_vpd_read for tools that want "show me
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
 * @param addr     target function
 * @param cb       per-keyword callback. Receives keyword (2-char
 *                 ASCII), data (impl-owned bytes), len, and ctx.
 * @param ctx      opaque context forwarded to @p cb
 * @return 0 if iteration completed without the callback stopping
 *     it, the callback's non-zero return if it stopped early, or
 *     -1 if VPD is unsupported or any bus error is encountered.
 */
int
axl_pci_vpd_iter(
    AxlPciAddr   addr,
    int        (*cb)(
        const char     keyword[2],
        const uint8_t *data,
        size_t         len,
        void          *ctx
    ),
    void        *ctx
);

// ---------------------------------------------------------------------------
// Vendor / device / subsystem name database (optional sidecar JSON5)
// ---------------------------------------------------------------------------

/**
 * @name Per-name length contracts
 * @brief Maximum bytes (including NUL) any database lookup can return.
 *
 * Documented caps so consumers can stack-allocate buffers at compile
 * time. The loader truncates over-cap entries on the way in (silent
 * — `axl_json_get_string` does the truncation), so lookup return
 * values are always within bounds. Forward-looking: the curated
 * `share/pci-ids.json5` is well under these numbers today.
 *
 * @{
 */
/* Sized to comfortably hold real pci.ids entries — vendor strings
   like "Advanced Micro Devices, Inc. [AMD/ATI]" run ~40 bytes,
   device strings frequently 100-130 bytes, subsystem strings
   similar. Composed name ≥ vendor + device + small fixed overhead
   so axl_pci_format_name never truncates non-truncated inputs. */
#define AXL_PCI_VENDOR_NAME_MAX     128u   ///< vendor entry max bytes
#define AXL_PCI_DEVICE_NAME_MAX     192u   ///< device entry max bytes
#define AXL_PCI_SUBSYS_NAME_MAX     192u   ///< subsystem entry max bytes
#define AXL_PCI_CLASS_NAME_MAX      128u   ///< class entry max bytes
#define AXL_PCI_NAME_COMPOSED_MAX   384u   ///< axl_pci_format_name output max
/** @} */

/**
 * @brief Opaque handle to a loaded vendor/device/subsystem database.
 *
 * Created by axl_pci_ids_open or axl_pci_ids_open_from_buffer,
 * destroyed by axl_pci_ids_close. Multiple handles can coexist —
 * a consumer that wants a "public + private" overlay loads two
 * handles and queries them in priority order, so internal/OEM names
 * shadow the public set on collisions.
 *
 * The process-global API (axl_pci_ids_load and friends) wraps
 * a single internal handle for the common case.
 */
typedef struct AxlPciIds AxlPciIds;

/**
 * @brief Open a database handle by reading a JSON5 file at @p path.
 *
 * @return @c AXL_SIDECAR_OK (handle returned via @p out),
 *         @c AXL_SIDECAR_FILE_MISSING if @p path does not exist or
 *           is unreadable,
 *         @c AXL_SIDECAR_PARSE_ERROR if the file was found but JSON5
 *           parsing or schema validation failed.
 *
 * The @c FILE_MISSING / @c PARSE_ERROR split lets tools log
 * differently — "no database shipped" is a deployment problem
 * (numeric fallback is fine), while "parse error" is an authoring
 * problem that should be loud.
 */
AxlSidecarStatus
axl_pci_ids_open(
    const char   *path,    ///< path to JSON5 file
    AxlPciIds   **out      ///< [out] handle on success
);

/**
 * @brief Open a database handle from an in-memory JSON5 buffer.
 *
 * Identical semantics to axl_pci_ids_open but reads from a
 * caller-owned buffer instead of a file. Useful for embedded or
 * test fixtures that ship the database compiled in.
 *
 * @return @c AXL_SIDECAR_OK on success, @c AXL_SIDECAR_PARSE_ERROR
 *     on parse / schema error. (No @c FILE_MISSING return — the
 *     buffer is the input, so "not found" doesn't apply.)
 */
AxlSidecarStatus
axl_pci_ids_open_from_buffer(
    const char   *json5,   ///< JSON5 source (no NUL required)
    size_t        len,     ///< buffer length in bytes
    AxlPciIds   **out      ///< [out] handle on success
);

/**
 * @brief Free a database handle.
 *
 * NULL-safe. After calling, every pointer previously returned by
 * the @c axl_pci_ids_*_name lookups against this handle is invalid.
 */
void
axl_pci_ids_close(
    AxlPciIds  *ids        ///< handle (NULL-safe)
);

/**
 * @brief Vendor lookup against an explicit handle.
 * @return database-owned string or NULL if unknown / handle empty.
 */
const char *
axl_pci_ids_vendor_name(
    const AxlPciIds  *ids,
    uint16_t          vid
);

/**
 * @brief Device lookup against an explicit handle.
 * @return database-owned string or NULL if (vid, did) is unknown.
 */
const char *
axl_pci_ids_device_name(
    const AxlPciIds  *ids,
    uint16_t          vid,
    uint16_t          did
);

/**
 * @brief Subsystem lookup against an explicit handle.
 *
 * Subsystem IDs identify the OEM card built around a piece of silicon
 * — a server-vendor rebadged NIC's `(svid, sdid)` decodes to the OEM
 * SKU name even though the underlying device's `(vid, did)` reports
 * the silicon vendor. The (svid, sdid) pair lives at config offsets
 * 0x2C / 0x2E on header-type-0 functions.
 *
 * @return database-owned string or NULL if (svid, sdid) is unknown.
 */
const char *
axl_pci_ids_subsys_name(
    const AxlPciIds  *ids,
    uint16_t          svid,
    uint16_t          sdid
);

/**
 * @name Database iteration callbacks
 * @brief Non-zero return stops the walk; the value propagates.
 * @{
 */
typedef int (*AxlPciIdsVendorFn)(uint16_t vid,
                                 const char *name, void *ctx);
typedef int (*AxlPciIdsDeviceFn)(uint16_t vid, uint16_t did,
                                 const char *name, void *ctx);
typedef int (*AxlPciIdsSubsysFn)(uint16_t svid, uint16_t sdid,
                                 const char *name, void *ctx);
/** @} */

/**
 * @brief Iterate every vendor entry in a database.
 *
 * Useful for debug dumps ("show me everything in this overlay"),
 * validators ("does my private DB shadow these public entries?"),
 * and code that needs to materialize the database into a different
 * representation (sorted list, text export, ...).
 *
 * Iteration order is hash-table-internal — do not rely on it.
 *
 * @return 0 if the walk completed without the callback stopping it,
 *     the callback's first non-zero return if it stopped early, or
 *     -1 if @p ids or @p fn is NULL.
 */
int
axl_pci_ids_foreach_vendor(
    const AxlPciIds   *ids,
    AxlPciIdsVendorFn  fn,
    void              *ctx
);

/// Iterate every (vid, did) device entry. See axl_pci_ids_foreach_vendor.
int
axl_pci_ids_foreach_device(
    const AxlPciIds   *ids,
    AxlPciIdsDeviceFn  fn,
    void              *ctx
);

/// Iterate every (svid, sdid) subsystem entry. See axl_pci_ids_foreach_vendor.
int
axl_pci_ids_foreach_subsys(
    const AxlPciIds   *ids,
    AxlPciIdsSubsysFn  fn,
    void              *ctx
);

// ---------------------------------------------------------------------------
// Process-global database (singleton — thin shim over a single handle)
// ---------------------------------------------------------------------------

/**
 * @brief Load a curated PCI vendor/device/subsystem name database.
 *
 * Two modes selected by @p override_path:
 *
 *   - **Explicit** (`override_path` non-NULL): use exactly that path.
 *     Returns @c AXL_SIDECAR_FILE_MISSING if the file is missing,
 *     @c AXL_SIDECAR_PARSE_ERROR if found but malformed. No
 *     fallback — explicit means explicit, so the error code
 *     reflects what the user asked for.
 *   - **Autodiscover** (`override_path` NULL): try `pci-ids.json5`
 *     next to the running .efi (companion path), then in the
 *     current working directory. Returns @c AXL_SIDECAR_FILE_MISSING
 *     if neither candidate exists, @c AXL_SIDECAR_PARSE_ERROR if a
 *     candidate was found but failed to parse.
 *
 * The file format is the JSON5 schema axl-sdk ships in
 * `share/pci-ids.json5` — vendor entries `{ id, name }`, device
 * entries `{ vid, did, name }`, optional subsystem entries
 * `{ svid, sdid, name }`. Only IDs explicitly listed are decoded;
 * for the long tail use @c scripts/pci-ids-to-json5.py to generate
 * a custom database from the canonical pci.ids text file.
 *
 * Idempotent: a successful load is a no-op on subsequent calls.
 *
 * On a successful first load, the singleton registers an
 * axl_atexit cleanup so the parsed hash tables are freed at
 * runtime cleanup automatically. Calling axl_pci_ids_free
 * explicitly is still fine (it unregisters the trampoline) and
 * worth doing for consumers that want to drop the database before
 * exit, but it's no longer required for leak-free shutdown.
 */
AxlSidecarStatus
axl_pci_ids_load(
    const char  *override_path  ///< explicit path, or NULL to auto-discover
);

/**
 * @brief Free the loaded vendor/device database.
 *
 * Safe to call when no database is loaded. After calling, the
 * pointers previously returned from axl_pci_vendor_name and
 * axl_pci_device_name are no longer valid.
 *
 * Optional — axl_pci_ids_load registers an atexit cleanup
 * automatically. Call this only when you want to drop the database
 * before runtime cleanup runs (e.g. memory-pressure reclaim).
 */
void
axl_pci_ids_free(
    void
);

/**
 * @brief Look up a vendor name by 16-bit vendor ID.
 *
 * @return pointer to the vendor name (database-owned, valid until
 *     axl_pci_ids_free), or NULL if no database is loaded or
 *     @p vid is not present in the loaded set.
 */
const char *
axl_pci_vendor_name(
    uint16_t  vid     ///< 16-bit vendor ID
);

/**
 * @brief Look up a device name by (vid, did) pair.
 *
 * Does not fall back to the vendor name when the device is unknown
 * — callers compose their own "vendor name + numeric device ID"
 * fallback (or use axl_pci_format_name).
 *
 * @return pointer to the device name (database-owned), or NULL if
 *     no database is loaded or the pair isn't in the loaded set.
 */
const char *
axl_pci_device_name(
    uint16_t  vid,    ///< 16-bit vendor ID
    uint16_t  did     ///< 16-bit device ID
);

/**
 * @brief Look up a subsystem (OEM card) name by (svid, sdid) pair.
 *
 * See axl_pci_ids_subsys_name for the rationale (OEM-rebadged
 * silicon needs OEM SKU decoding). Same fallback semantics as the
 * other singleton helpers — NULL when no database is loaded or the
 * pair is unknown.
 */
const char *
axl_pci_subsys_name(
    uint16_t  svid,   ///< 16-bit subsystem vendor ID
    uint16_t  sdid    ///< 16-bit subsystem device ID
);

/**
 * @brief Compose a "vendor + device" display string against a handle.
 *
 * Centralizes the rendering convention every consumer would
 * otherwise reinvent — the goal is that every tool prints the same
 * string for the same (vid, did) pair. Output:
 *
 *   - vendor known + device known   → `"<vendor> <device>"`
 *   - vendor known + device unknown → `"<vendor> Device <DID hex>"`
 *   - vendor unknown                → `"<VID>:<DID>"`
 *
 * Hex literals in the output are lowercase, 4-wide, zero-padded
 * (matching Linux lspci convention).
 *
 * Vendor-unknown short-circuits regardless of device-name presence:
 * without a verified vendor a device-name hit is ambiguous
 * provenance, so the fallback is always all-numeric.
 *
 * Output never exceeds AXL_PCI_NAME_COMPOSED_MAX bytes — pin
 * `char buf[AXL_PCI_NAME_COMPOSED_MAX]` and the formatter is
 * truncation-safe.
 *
 * @return number of bytes written excluding NUL (snprintf-shape),
 *     or -1 on bad arguments.
 */
int
axl_pci_ids_format_name(
    const AxlPciIds  *ids,
    uint16_t          vid,
    uint16_t          did,
    char             *buf,
    size_t            buflen
);

/**
 * @brief Singleton-backed convenience wrapper for axl_pci_ids_format_name.
 *
 * Equivalent to `axl_pci_ids_format_name(<process-global handle>, ...)`.
 * Layered-DB consumers should call the handle form directly with
 * their own priority chain.
 */
int
axl_pci_format_name(
    uint16_t  vid,
    uint16_t  did,
    char     *buf,
    size_t    buflen
);

// ---------------------------------------------------------------------------
// Class-name database (optional sidecar JSON5 overlay)
// ---------------------------------------------------------------------------

/**
 * @brief Opaque handle to a loaded PCI class-code name overlay.
 *
 * Parallel to AxlPciIds but for class triplet decoding. The
 * compiled-in tables in axl-pci.c stay as the bootstrap default;
 * a loaded overlay is consulted first per-tier (base, sub, prog),
 * with the compiled-in table as the fallback. New class triplets
 * (CXL Memory Expanders, future PCIe class assignments, ...) can
 * land via a `git pull` of the JSON5 sidecar without rebuilding
 * every consumer.
 *
 * The overlay lives in the `classes[]` section of `share/pci-ids.json5`. Schema 2 nests subclasses under bases and progs under subclasses; schema 1 (legacy flat) lets each entry pin
 * any subset of (base, sub, prog) — base only for "all subclasses
 * of this base", base+sub for a subclass, base+sub+prog for a
 * specific prog_if.
 */
typedef struct AxlPciClassDb AxlPciClassDb;

/**
 * @brief Open a class-overlay handle from a JSON5 file.
 * @return @c AXL_SIDECAR_OK / @c AXL_SIDECAR_FILE_MISSING /
 *     @c AXL_SIDECAR_PARSE_ERROR.
 */
AxlSidecarStatus
axl_pci_class_open(
    const char       *path,
    AxlPciClassDb   **out
);

/**
 * @brief Open a class-overlay handle from an in-memory buffer.
 * @return @c AXL_SIDECAR_OK / @c AXL_SIDECAR_PARSE_ERROR.
 */
AxlSidecarStatus
axl_pci_class_open_from_buffer(
    const char       *json5,
    size_t            len,
    AxlPciClassDb   **out
);

/**
 * @brief Free a class-overlay handle. NULL-safe.
 */
void
axl_pci_class_close(
    AxlPciClassDb  *db
);

/**
 * @brief Per-tier overlay lookups against an explicit handle.
 *
 * Only the overlay is consulted — these do NOT fall back to the
 * compiled-in tables. Consumers that want "overlay first, then
 * compiled-in" should use axl_pci_class_string_fmt, which
 * internally walks the singleton overlay then the built-in tables.
 *
 * @return database-owned string or NULL if @p db is NULL or the
 *     tier has no override entry for this code.
 */
const char *
axl_pci_class_db_base_name(
    const AxlPciClassDb  *db,
    uint8_t               base
);
const char *
axl_pci_class_db_sub_name(
    const AxlPciClassDb  *db,
    uint8_t               base,
    uint8_t               sub
);
const char *
axl_pci_class_db_prog_name(
    const AxlPciClassDb  *db,
    uint8_t               base,
    uint8_t               sub,
    uint8_t               prog
);

/**
 * @brief Load the process-global class-name overlay.
 *
 * Same lookup semantics as axl_pci_ids_load — explicit
 * @p override_path is authoritative; NULL autodiscovers via
 * `pci-ids.json5` next to the running .efi, then in cwd. Loader reads only the `classes[]` section, ignoring `vendors[]`.
 *
 * Once loaded, every axl_pci_class_string and
 * axl_pci_class_string_fmt call consults the overlay before
 * the compiled-in tables. The compiled-in tables stay as the
 * bootstrap so axl-sdk works without a sidecar at all.
 *
 * Like axl_pci_ids_load, this registers an atexit cleanup on
 * a successful first load so the overlay is freed at runtime
 * cleanup automatically. Calling axl_pci_class_free
 * explicitly is optional (used to drop the overlay early).
 *
 * @return @c AXL_SIDECAR_OK on success (idempotent on second call),
 *     @c AXL_SIDECAR_FILE_MISSING if missing,
 *     @c AXL_SIDECAR_PARSE_ERROR on parse error.
 */
AxlSidecarStatus
axl_pci_class_load(
    const char  *override_path
);

/**
 * @brief Free the process-global class-name overlay.
 *
 * After calling, lookups fall back exclusively to the compiled-in
 * tables.
 */
void
axl_pci_class_free(
    void
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PCI_H */
