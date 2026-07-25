/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mem-region.h
    Physical-memory region map + fault-safe range access.

    `<axl/axl-mem-phys.h>` provides the *raw* physical access primitives
    (map / one-shot read & write). They are fast and unguarded: reading an
    unmapped or misaligned address can raise a synchronous fault and
    terminate the image. This header is the layer that makes arbitrary-
    address access **safe and navigable** — what a memory / hex editor needs
    when a user can type any address:

      - **Classify** the physical address space into typed regions
        (`AxlMemRegion`: RAM / reserved / ACPI / MMIO / unmapped), sourced
        from the UEFI memory map merged with the PI **GCD** memory-space map
        (so MMIO ranges the EFI map omits — e.g. PCI BARs — are classified).
      - **Gate** every access behind `axl_mem_phys_is_accessible`, which
        refuses anything not entirely backed by a mapped region. This is a
        best-effort guard, not a true fault handler (see the caveat on that
        function) — but it catches the common "typed an address in a huge
        unmapped gap" case that would otherwise fault.
      - **Bulk** width- and alignment-aware `read_range` / `write_range`,
        centralizing the AArch64 natural-alignment rule so the consumer
        never hand-rolls a faulting access.

    Region data is cached on first query and re-walked by
    `axl_mem_phys_region_refresh`. UEFI is single-process / single-threaded
    (BSP), so the cache and the access policy are plain process-global state.
**/

#ifndef AXL_MEM_REGION_H
#define AXL_MEM_REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Region classification
// ---------------------------------------------------------------------------

/// How a physical region is classified. The bit `(1u << type)` is used in the
/// access-policy masks (see AxlMemAccessPolicy).
typedef enum {
    AXL_MEM_REGION_RAM      = 0, ///< Usable system RAM (EfiConventionalMemory /
                                 ///< Loader* / BootServices*; GCD SystemMemory).
    AXL_MEM_REGION_RESERVED = 1, ///< Reserved / runtime / GCD Reserved /
                                 ///< Persistent — mapped but not general-use RAM.
    AXL_MEM_REGION_ACPI     = 2, ///< ACPI reclaim memory or ACPI NVS.
    AXL_MEM_REGION_MMIO     = 3, ///< Memory-mapped I/O (EfiMemoryMappedIO[PortSpace]
                                 ///< + GCD MemoryMappedIo). Access may have
                                 ///< side effects; byte width is often wrong.
    AXL_MEM_REGION_UNMAPPED  = 4, ///< Not described by any map (GCD NonExistent /
                                 ///< a gap). Touching it would fault — never
                                 ///< accessible regardless of policy.
    AXL_MEM_REGION_TYPE_COUNT = 5 ///< Number of region types (for mask sizing).
} AxlMemRegionType;

/// A classified, contiguous physical region.
typedef struct {
    uintptr_t        base;  ///< Region start (physical address).
    uint64_t         len;   ///< Region length in bytes (> 0).
    AxlMemRegionType type;  ///< Classification.
    uint64_t         attr;  ///< Firmware-reported EFI_MEMORY_* attribute bits
                            ///< (UC/WC/WT/WB/RO/…); 0 when not applicable
                            ///< (e.g. UNMAPPED) or unknown. Informational.
} AxlMemRegion;

/**
 * @brief Classify the region containing @p phys.
 *
 * Fills @p out with the contiguous region that contains @p phys. An address
 * in a gap not described by any map is reported as @ref AXL_MEM_REGION_UNMAPPED
 * with best-effort bounds (the gap between the surrounding described regions),
 * so a caller can always render "you are at 0x… in an UNMAPPED region" rather
 * than getting an error. The region map is built on first use and cached;
 * call axl_mem_phys_region_refresh to rebuild it.
 *
 * @return AXL_OK with @p out populated; AXL_ERR only on a NULL @p out or if the
 *     region map could not be built (firmware query failed).
 */
int
axl_mem_phys_region_at(
    uintptr_t      phys,   ///< physical address to classify
    AxlMemRegion  *out     ///< [out] receives the containing region (non-NULL)
);

/**
 * @brief Number of regions in the (cached) physical region map.
 *
 * Regions are ordered by ascending base, non-overlapping, and adjacent
 * same-type regions are coalesced. Pair with axl_mem_phys_region_get to
 * enumerate (a region-picker UI / "jump to next region"). Built on first use.
 *
 * @return AXL_OK (and @p out_count set); AXL_ERR on NULL @p out_count or a
 *     firmware-query failure.
 */
int
axl_mem_phys_region_count(
    size_t  *out_count   ///< [out] number of regions (non-NULL)
);

/**
 * @brief Fetch the region at @p index in the cached map.
 *
 * @p index is in `[0, count)` per axl_mem_phys_region_count.
 *
 * @return AXL_OK with @p out populated; AXL_ERR on NULL @p out, @p index out of
 *     range, or a firmware-query failure.
 */
int
axl_mem_phys_region_get(
    size_t         index,  ///< region index in [0, count)
    AxlMemRegion  *out     ///< [out] receives the region (non-NULL)
);

/**
 * @brief Rebuild the cached physical region map from current firmware state.
 *
 * The map is otherwise built once on first query and cached. Call this to pick
 * up changes (e.g. after driver enumeration alters the GCD MMIO map). Cheap to
 * call; the editor may refresh on demand.
 *
 * @return AXL_OK on success, AXL_ERR if the firmware query failed (the cache is
 *     left marked stale and is rebuilt on the next query).
 */
int
axl_mem_phys_region_refresh(void);

// ---------------------------------------------------------------------------
// Access policy (the opt-in guard)
// ---------------------------------------------------------------------------

/// Which region types may be read / written, as bitmasks of `(1u << type)`.
/// The default policy (see axl_mem_phys_get_policy) permits every *mapped*
/// type (RAM | RESERVED | ACPI | MMIO) for both read and write — least
/// limiting. Tighten it (e.g. RAM-only, or read-only) with
/// axl_mem_phys_set_policy. @ref AXL_MEM_REGION_UNMAPPED is never accessible
/// regardless of the masks (it would fault); the policy only gates among
/// mapped types.
typedef struct {
    uint32_t readable_types;  ///< bits `(1u << AxlMemRegionType)` allowed for read
    uint32_t writable_types;  ///< bits allowed for write
} AxlMemAccessPolicy;

/// All mapped region types (everything except UNMAPPED) — the default mask.
#define AXL_MEM_ACCESS_ALL_MAPPED                                          \
    ((1u << AXL_MEM_REGION_RAM) | (1u << AXL_MEM_REGION_RESERVED) |        \
     (1u << AXL_MEM_REGION_ACPI) | (1u << AXL_MEM_REGION_MMIO))

/**
 * @brief Read the current access policy.
 *
 * @return AXL_OK (and @p out filled); AXL_ERR on NULL @p out.
 */
int
axl_mem_phys_get_policy(
    AxlMemAccessPolicy  *out   ///< [out] current policy (non-NULL)
);

/**
 * @brief Replace the access policy used by is_accessible / read_range /
 *        write_range.
 *
 * Use this to restrict access — e.g. set both masks to `1u << AXL_MEM_REGION_RAM`
 * for a "RAM only" safe mode, or clear `writable_types` for read-only. Passing
 * NULL restores the permissive default (@ref AXL_MEM_ACCESS_ALL_MAPPED for both).
 *
 * @return AXL_OK.
 */
int
axl_mem_phys_set_policy(
    const AxlMemAccessPolicy  *policy   ///< new policy; NULL = restore default
);

// ---------------------------------------------------------------------------
// Fault-safety gate
// ---------------------------------------------------------------------------

/**
 * @brief Whether `[phys, phys+len)` is safe to access under the current policy.
 *
 * Returns true iff the entire span is covered by mapped regions whose type is
 * permitted by the current policy (writable_types when @p want_write, else
 * readable_types). A span that touches an @ref AXL_MEM_REGION_UNMAPPED region,
 * crosses out of the permitted set, has @p len == 0, or overflows the address
 * space returns false. Call this before every read / write so an arbitrary
 * user-typed address cannot terminate the image.
 *
 * **Caveat — best-effort, not a fault handler.** Pre-boot there is no
 * recoverable fault handler, so this gates through the region map only. It
 * cannot guarantee no fault: firmware may mark a page within a "RAM" region as
 * protected, and reads of @ref AXL_MEM_REGION_MMIO ranges may have **side
 * effects** on the device. It reduces, but does not eliminate, the risk.
 *
 * @return true if the span is accessible under the current policy, false
 *     otherwise.
 */
bool
axl_mem_phys_is_accessible(
    uintptr_t  phys,       ///< start physical address
    size_t     len,        ///< span length in bytes
    bool       want_write  ///< true to check write access, false for read
);

// ---------------------------------------------------------------------------
// Bulk width-aware range read / write
// ---------------------------------------------------------------------------

/**
 * @brief Read @p len bytes from @p phys into @p buf using @p access_width-byte
 *        accesses.
 *
 * Issues `len / access_width` natural-width accesses (each
 * `axl_mem_phys_read{8,16,32,64}`), copying into @p buf in ascending order.
 * `access_width == 1` is the plain byte read for RAM; 4 is the common width
 * for MMIO registers (where byte access is often wrong or has side effects).
 * Centralizes the AArch64 alignment rule so the consumer never issues a
 * faulting misaligned access.
 *
 * Refuses (AXL_ERR, no access performed) if: @p access_width is not one of
 * 1/2/4/8; @p phys is not a multiple of @p access_width (AArch64 would fault);
 * @p len is not a multiple of @p access_width; @p len == 0; @p buf is NULL; or
 * the span is not accessible for read per axl_mem_phys_is_accessible.
 *
 * @return AXL_OK on success (@p buf filled), AXL_ERR otherwise.
 */
int
axl_mem_phys_read_range(
    uintptr_t  phys,          ///< source physical address (multiple of @p access_width)
    size_t     len,           ///< bytes to read (multiple of @p access_width, > 0)
    uint32_t   access_width,  ///< per-access width in bytes: 1, 2, 4, or 8
    void      *buf            ///< [out] destination buffer (>= @p len bytes)
);

/**
 * @brief Write @p len bytes from @p buf to @p phys using @p access_width-byte
 *        accesses.
 *
 * Write counterpart to axl_mem_phys_read_range, with the same width /
 * alignment / length rules, gated on write accessibility
 * (axl_mem_phys_is_accessible with want_write = true). Issues
 * `axl_mem_phys_write{8,16,32,64}` in ascending order.
 *
 * @return AXL_OK on success, AXL_ERR on a bad argument or an inaccessible span.
 */
int
axl_mem_phys_write_range(
    uintptr_t    phys,          ///< destination physical address (multiple of @p access_width)
    size_t       len,           ///< bytes to write (multiple of @p access_width, > 0)
    const void  *buf,           ///< source buffer (>= @p len bytes)
    uint32_t     access_width   ///< per-access width in bytes: 1, 2, 4, or 8
);

// ===========================================================================
// I/O port space — region map + access (the port-space sibling of the above)
// ===========================================================================
//
// The x86 I/O port address space (0x0000–0xFFFF) is a SEPARATE address space
// from physical memory, accessed with IN/OUT instructions. The region map
// classifies it from the PI GCD I/O-space map; the access helpers wrap
// `axl-io-port.h`'s `axl_io_port_read/write`.
//
// **x86-only access.** Classification (region_at/count/get) works on any arch
// (it just reflects what the GCD reports — typically empty on AArch64, which
// has no port-I/O instruction space), but `axl_io_read_range` /
// `axl_io_write_range` return AXL_ERR on non-x86. **Reading an I/O port can
// have side effects on the device** — there is no side-effect-free probe.

/// Classification of an I/O port range.
typedef enum {
    AXL_IO_REGION_IO        = 0, ///< usable I/O port range (GCD Io).
    AXL_IO_REGION_RESERVED  = 1, ///< reserved I/O range (GCD Reserved).
    AXL_IO_REGION_UNMAPPED  = 2, ///< non-existent / not described (GCD NonExistent).
    AXL_IO_REGION_TYPE_COUNT = 3
} AxlIoRegionType;

/// A classified, contiguous I/O port range.
typedef struct {
    uintptr_t       base;  ///< range start (I/O port address).
    uint64_t        len;   ///< length in ports (> 0).
    AxlIoRegionType type;  ///< classification.
} AxlIoRegion;

/**
 * @brief Classify the I/O range containing port @p port.
 *
 * I/O-space analogue of axl_mem_phys_region_at — a port not described by the
 * GCD I/O map is reported as @ref AXL_IO_REGION_UNMAPPED with best-effort
 * bounds. Built on first use and cached; axl_io_region_refresh rebuilds it.
 *
 * @return AXL_OK with @p out populated; AXL_ERR on NULL @p out or a
 *     firmware-query failure.
 */
int
axl_io_region_at(
    uintptr_t     port,  ///< I/O port address to classify
    AxlIoRegion  *out    ///< [out] receives the containing range (non-NULL)
);

/**
 * @brief Number of ranges in the (cached) I/O region map.
 *
 * @return AXL_OK (and @p out_count set); AXL_ERR on NULL @p out_count or a
 *     firmware-query failure.
 */
int
axl_io_region_count(
    size_t  *out_count   ///< [out] number of ranges (non-NULL)
);

/**
 * @brief Fetch the I/O range at @p index in the cached map (`[0, count)`).
 *
 * @return AXL_OK with @p out populated; AXL_ERR on NULL @p out, @p index out
 *     of range, or a firmware-query failure.
 */
int
axl_io_region_get(
    size_t        index,  ///< range index in [0, count)
    AxlIoRegion  *out     ///< [out] receives the range (non-NULL)
);

/**
 * @brief Rebuild the cached I/O region map from current firmware state.
 *
 * @return AXL_OK on success, AXL_ERR if the firmware query failed (the cache
 *     is left marked stale and rebuilt on the next query).
 */
int
axl_io_region_refresh(void);

/**
 * @brief Whether the I/O range `[port, port+len)` is safe to access.
 *
 * True iff the whole range is classified @ref AXL_IO_REGION_IO. @p want_write
 * is accepted for symmetry with the memory gate; the I/O policy is the same
 * for read and write. Note an I/O read may still have device side effects.
 *
 * @return true if the range is entirely usable I/O space, false otherwise.
 */
bool
axl_io_is_accessible(
    uintptr_t  port,       ///< start I/O port
    size_t     len,        ///< number of ports
    bool       want_write  ///< accepted for symmetry; same policy as read
);

/**
 * @brief Read @p len ports from @p port into @p buf using @p access_width-byte
 *        port reads (1/2/4).
 *
 * Issues `len / access_width` `axl_io_port_read{8,16,32}` reads. Refuses
 * (AXL_ERR) a bad width (not 1/2/4), a @p len not a multiple of it, a NULL
 * @p buf, a range not accessible per axl_io_is_accessible, a span beyond the
 * 64 KiB port space, or any access on a **non-x86** build (no port I/O).
 * Unlike memory, I/O ports have no alignment requirement.
 *
 * @return AXL_OK on success, AXL_ERR otherwise.
 */
int
axl_io_read_range(
    uintptr_t  port,          ///< source I/O port
    size_t     len,           ///< ports to read (multiple of @p access_width, > 0)
    uint32_t   access_width,  ///< per-access width: 1, 2, or 4
    void      *buf            ///< [out] destination buffer (>= @p len bytes)
);

/**
 * @brief Write @p len ports from @p buf to @p port (1/2/4-byte port writes).
 *
 * Write counterpart to axl_io_read_range, same width / length / range /
 * arch rules, issuing `axl_io_port_write{8,16,32}`.
 *
 * @return AXL_OK on success, AXL_ERR on a bad argument, inaccessible range,
 *     or a non-x86 build.
 */
int
axl_io_write_range(
    uintptr_t    port,          ///< destination I/O port
    size_t       len,           ///< ports to write (multiple of @p access_width, > 0)
    const void  *buf,           ///< source buffer (>= @p len bytes)
    uint32_t     access_width   ///< per-access width: 1, 2, or 4
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_MEM_REGION_H */
