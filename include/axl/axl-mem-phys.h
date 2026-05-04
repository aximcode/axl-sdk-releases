/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mem-phys.h
    Physical-memory access — map / unmap and one-shot read / write.

    UEFI runs with the physical memory map identity-mapped, so the
    map step is effectively a no-op there: the returned VA equals
    the physical address. The map / unmap shape exists for
    portability — a future Linux backend would `mmap("/dev/mem")`
    on the way in and `munmap` on the way out, and consumer code
    that holds a mapping over multiple accesses keeps working.

    For the common case where a tool reads (or writes) a single
    address one time, the one-shot helpers do the map / access /
    unmap trio in one call. They keep the typical
    `*(volatile uint32_t *)0xFEE00000` style readable without
    forcing every call site to manage a mapping.

    @code
    // Held mapping over multiple accesses.
    void *va;
    if (axl_mem_phys_map(0xFED00000, 4096, &va) == AXL_OK) {
        for (size_t i = 0; i < 4096; i += 4) {
            uint32_t w = *(volatile uint32_t *)((uint8_t *)va + i);
            // ...
        }
        axl_mem_phys_unmap(va, 4096);
    }

    // One-shot.
    uint32_t signature;
    axl_mem_phys_read32(0xE0000, &signature);
    @endcode
**/

#ifndef AXL_MEM_PHYS_H
#define AXL_MEM_PHYS_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Map / unmap
// ---------------------------------------------------------------------------

/**
 * @brief Map @p len bytes of physical memory starting at @p phys.
 *
 * Returns a virtual address through @p out_va. On UEFI this is the
 * same as @p phys (identity-mapped); on a future Linux backend it
 * is a `mmap` return value. Either way the caller dereferences the
 * VA directly with `volatile uint{N}_t *` casts to read or write,
 * and pairs every successful map with an `axl_mem_phys_unmap`
 * using the same @p len.
 *
 * @return AXL_OK on success, AXL_ERR if the mapping cannot be established.
 */
int
axl_mem_phys_map(
    uintptr_t   phys,    ///< physical address (alignment is consumer's responsibility)
    size_t      len,     ///< number of bytes to map
    void      **out_va   ///< [out] receives the mapped VA
);

/**
 * @brief Release a mapping established by axl_mem_phys_map.
 *
 * On UEFI this is a no-op; on a future Linux backend it is
 * `munmap`. NULL @p va is tolerated (no-op).
 */
void
axl_mem_phys_unmap(
    void   *va,    ///< VA returned by axl_mem_phys_map
    size_t  len    ///< same length passed to map
);

// ---------------------------------------------------------------------------
// One-shot read / write helpers
// ---------------------------------------------------------------------------

/**
 * @brief Read a byte from physical memory.
 *
 * Maps the smallest aligned region covering @p phys, dereferences,
 * and unmaps. On UEFI all three steps are direct; on backends that
 * actually need to map, this is one mmap + read + munmap per call,
 * so the held-mapping API above is the better choice for hot loops.
 *
 * @return AXL_OK on success, AXL_ERR if the mapping cannot be established.
 */
int
axl_mem_phys_read8(
    uintptr_t   phys,
    uint8_t    *out
);

/// 16-bit variant. @p phys must be 16-bit aligned on AArch64
/// (a misaligned access raises a synchronous Data Abort and
/// terminates the image); x86 tolerates misaligned reads at a
/// performance penalty.
int
axl_mem_phys_read16(uintptr_t phys, uint16_t *out);

/// 32-bit variant. Same alignment caveat as
/// axl_mem_phys_read16 — required on AArch64, advisory on x86.
int
axl_mem_phys_read32(uintptr_t phys, uint32_t *out);

/// 64-bit variant. Same alignment caveat as
/// axl_mem_phys_read16 — required on AArch64, advisory on x86.
int
axl_mem_phys_read64(uintptr_t phys, uint64_t *out);

/// One-shot byte write.
int
axl_mem_phys_write8 (uintptr_t phys, uint8_t  value);

/// One-shot 16-bit write.
int
axl_mem_phys_write16(uintptr_t phys, uint16_t value);

/// One-shot 32-bit write.
int
axl_mem_phys_write32(uintptr_t phys, uint32_t value);

/// One-shot 64-bit write.
int
axl_mem_phys_write64(uintptr_t phys, uint64_t value);

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

/**
 * @brief Find the first occurrence of @p needle in a mapped region.
 *
 * Operates on a VA (typically returned by axl_mem_phys_map). Useful
 * for scanning ROMs, firmware tables, or signature blocks. Linear
 * byte-by-byte scan; use sparingly on multi-megabyte regions.
 *
 * @return AXL_OK on hit (and @c *out_match is set to a pointer inside
 *     @p va), AXL_ERR if @p needle is not present.
 */
int
axl_mem_phys_search(
    const void   *va,              ///< mapped region base
    size_t        len,              ///< region length in bytes
    const void   *needle,           ///< pattern to find
    size_t        needle_len,       ///< pattern length in bytes
    const void  **out_match         ///< [out] pointer inside @p va
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_MEM_PHYS_H */
