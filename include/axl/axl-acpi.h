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

    Scope is discovery, typed readers, and a **non-evaluating** AML
    namespace walker.

    **AML execution is out of scope, permanently** — that means a
    bytecode interpreter with `OperationRegion` access, which is
    ACPICA-sized and not what diagnostic tools need. Parsing static
    `Name()` declarations out of the byte stream is a different
    thing: a structural walk with no evaluation, no side effects and
    no hardware access, and that is what axl_aml_walk_begin() does.
    A `Method` body is skipped by length and never entered, so
    anything whose value is not a literal in the table is reported
    as present-but-unreadable rather than guessed at.
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
 * The walk is the complete table set, like `acpidump` or
 * EFI_ACPI_SDT_PROTOCOL.GetAcpiTable: it includes the FADT's DSDT and
 * FACS children, which are not RSDT/XSDT entries (they hang off the
 * FADT) and are recovered from it. They are yielded FIRST — FACS then
 * DSDT (when present), ahead of the RSDT/XSDT tables — matching the
 * order GetAcpiTable / `acpidump` / `/sys/firmware/acpi/tables` use, so
 * a consumer diffing against that oracle needs no reordering.
 * `axl_acpi_find` / `_find_next` see them too. Caveat: the FACS is not
 * a standard System Description
 * Table — only its `signature` and `length` are meaningful, and
 * `axl_acpi_checksum_ok` does not apply to it (it has no whole-table
 * checksum). The DSDT is a normal SDT with a valid header.
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

// ---------------------------------------------------------------------------
// AML namespace walker — parsing only, never evaluation
// ---------------------------------------------------------------------------

/// Longest namespace path reported, e.g. "\\_SB_.PC00.RP05.PXSX".
/// Firmware nests deeply but not unboundedly; paths longer than this
/// are truncated and the node still reported.
#define AXL_AML_PATH_MAX  128

/// Nesting depth cap. The table is untrusted firmware input, so the
/// walk must be bounded rather than trusting the byte stream.
#define AXL_AML_DEPTH_MAX  32

/// Distinct method names remembered for resolving method invocations.
/// `MethodInvocation := NameString TermArgList` carries no argument
/// count, so AML cannot be parsed without knowing each method's
/// declared arity -- see axl_aml_walk_begin(). The two measured
/// firmware images declare 391 and 73 distinct method names.
#define AXL_AML_METHOD_MAX  512

/**
 * @brief How a named object's value was obtained — or why it wasn't.
 *
 * This distinction is the walker's reason for existing. A static
 * walker CAN read `Name(_ADR, 0x001C0004)` and CANNOT read
 * `Method(_ADR){...}`, and a consumer must never confuse the second
 * with "the firmware didn't publish one".
 *
 * #AXL_AML_VALUE_NON_INTEGER is the fourth case and it is real, not
 * theoretical: `_UID` is commonly a string (`Name (_UID, "IPMI
 * Device")`), and reporting that as a Method would claim the walker
 * could not read something it read perfectly well.
 *
 * How much falls in each bucket varies enormously between machines,
 * so treat it as data rather than assuming a distribution: one
 * measured client had 34% of its `_ADR` objects behind Methods and
 * published no `_SUN` at all, while a measured server had every
 * `_ADR` static and 26 `_SUN`, but every `_SEG` and `_BBN` behind a
 * Method.
 */
typedef enum {
    AXL_AML_VALUE_STATIC = 0,   ///< a literal in the byte stream; the value is valid
    AXL_AML_VALUE_METHOD,       ///< declared as a Method — present, but unreadable without executing AML
    AXL_AML_VALUE_NON_INTEGER,  ///< declared as a String, Buffer or Package — present and static, but not a number
    AXL_AML_VALUE_ABSENT        ///< not declared on this device at all
} AxlAmlValueKind;

/**
 * @brief One named integer object belonging to a device.
 *
 * @a value is meaningful only when @a kind is #AXL_AML_VALUE_STATIC.
 * It is zeroed otherwise, but a caller must branch on @a kind rather
 * than testing for zero — zero is a perfectly ordinary `_ADR`, `_UID`
 * or `_SEG`.
 */
typedef struct {
    AxlAmlValueKind  kind;
    uint64_t         value;   ///< valid only when @a kind is AXL_AML_VALUE_STATIC
} AxlAmlValue;

/**
 * @brief One `Device` found in the namespace.
 *
 * The integer objects a device may carry, all reported through
 * #AxlAmlValue so "unreadable" stays distinct from "absent".
 * `_PLD` is deliberately not an #AxlAmlValue: it is a Buffer, not an
 * integer, so only its presence is reported.
 */
typedef struct {
    char             path[AXL_AML_PATH_MAX];  ///< full path, NUL-terminated
    bool             conditional;             ///< declared inside an If/Else — may not exist at runtime
    bool             path_truncated;          ///< the real path exceeded AXL_AML_PATH_MAX
    AxlAmlValue      adr;                     ///< _ADR — device+function, the correlation's join key
    AxlAmlValue      sun;                     ///< _SUN — slot user number
    AxlAmlValue      uid;                     ///< _UID — unique ID
    AxlAmlValue      seg;                     ///< _SEG — PCI segment group
    AxlAmlValue      bbn;                     ///< _BBN — bus number. Very often a Method; see the note on AxlAmlValueKind
    AxlAmlValueKind  pld_kind;                ///< _PLD is a Buffer; presence only, never a value
} AxlAmlNode;

/**
 * @brief Walk state. Allocate on the stack; the walk owns no memory.
 *
 * Members are private and prefixed accordingly — read them through
 * the accessor functions, not directly.
 */
typedef struct {
    const uint8_t  *_aml;             ///< definition block start
    size_t          _len;             ///< definition block length
    size_t          _pos;             ///< current offset into _aml
    bool            _truncated;       ///< the walk stopped on malformed AML
    bool            _skipped;         ///< a package was stepped over unparsed
    unsigned        _depth;           ///< current nesting depth
    /* Per-level scope bookkeeping: the name segment opened at each
       level, the offset its package ends at, and whether that level
       was entered through an If/Else. */
    char            _seg[AXL_AML_DEPTH_MAX][5];
    size_t          _end[AXL_AML_DEPTH_MAX];
    bool            _cond[AXL_AML_DEPTH_MAX];
    /* Method arity table, filled by axl_aml_walk_begin. */
    char            _mseg[AXL_AML_METHOD_MAX][5];
    uint8_t         _margc[AXL_AML_METHOD_MAX];
    unsigned        _mcount;
} AxlAmlWalk;

/**
 * @brief Begin walking the AML definition block in @p table.
 *
 * @p table is a DSDT or SSDT header. The walk is read-only, allocates
 * nothing, and executes nothing. Call axl_aml_walk_next() until it
 * returns false.
 *
 * Begins with a pre-pass that records every `Method` declaration's
 * name and argument count. That pass is not optional: the AML grammar
 * defines `MethodInvocation := NameString TermArgList` with **no
 * argument count**, so a parser that meets a call cannot know how many
 * arguments to step over without having seen the declaration. ACPICA,
 * `iasl` and the smaller AML interpreters all load the namespace
 * first for the same reason. Skipping it costs real devices: on one
 * measured DSDT, 42 of 396 devices sat behind unresolvable calls.
 *
 * @return AXL_OK on success, AXL_ERR if @p walk or @p table is NULL,
 *     or the table is too short to contain a definition block.
 */
int
axl_aml_walk_begin(
    AxlAmlWalk           *walk,   ///< [out] caller-allocated walk state
    const AxlAcpiHeader  *table   ///< DSDT or SSDT
);

/**
 * @brief Yield the next `Device` found, in declaration order.
 *
 * A device declared inside an `If` or `Else` body is yielded with
 * @c conditional set. The walker cannot know whether the condition
 * holds without executing AML, so it reports the device *and* the
 * doubt rather than choosing between dropping real devices and
 * inventing absent ones. On one measured machine roughly 23% of
 * devices — including a host bridge — were declared this way.
 *
 * @return true when @p out was populated; false at the end of the
 *     table, or when the walk stopped on malformed AML (which
 *     axl_aml_walk_truncated() then reports).
 */
bool
axl_aml_walk_next(
    AxlAmlWalk   *walk,   ///< walk state
    AxlAmlNode   *out     ///< [out] receives the device
);

/**
 * @brief Report whether anything in the table went unseen.
 *
 * True for either of two reasons, because to a consumer they mean the
 * same thing — the device list is incomplete:
 *
 * - the walk **stopped** on malformed AML or at #AXL_AML_DEPTH_MAX;
 * - the walk **skipped** a package whose contents it could not parse,
 *   and carried on with that package's siblings.
 *
 * The second is the common one and is not an error: firmware contains
 * constructs this walker deliberately does not model, and stepping
 * over them beats abandoning the table. What matters is that a
 * consumer rendering a device list can say "there may be more" rather
 * than implying it saw everything.
 *
 * @return true if any part of the definition block went unwalked;
 *     false if the whole of it was traversed.
 */
bool
axl_aml_walk_truncated(
    const AxlAmlWalk  *walk   ///< walk state
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ACPI_H */
