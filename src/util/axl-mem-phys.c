/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mem-phys.c
    Physical-memory access on UEFI.

    UEFI runs identity-mapped, so map/unmap are no-ops and
    one-shot reads dereference the physical address directly. The
    map/unmap shape is preserved for portability — a Linux backend
    would `mmap("/dev/mem")` here.
**/

#include <axl/axl-mem-phys.h>
#include <axl/axl-str.h>   /* axl_memmem */

int
axl_mem_phys_map(
    uintptr_t   phys,
    size_t      len,
    void      **out_va
    )
{
    if (out_va == NULL || len == 0) {
        return -1;
    }
    *out_va = (void *)phys;
    return 0;
}

void
axl_mem_phys_unmap(
    void   *va,
    size_t  len
    )
{
    (void)va;
    (void)len;
    /* No-op on UEFI (identity-mapped). */
}

// ---------------------------------------------------------------------------
// One-shot helpers
// ---------------------------------------------------------------------------

int
axl_mem_phys_read8(
    uintptr_t   phys,
    uint8_t    *out
    )
{
    if (out == NULL) {
        return -1;
    }
    *out = *(volatile const uint8_t *)phys;
    return 0;
}

int
axl_mem_phys_read16(
    uintptr_t   phys,
    uint16_t   *out
    )
{
    if (out == NULL) {
        return -1;
    }
    *out = *(volatile const uint16_t *)phys;
    return 0;
}

int
axl_mem_phys_read32(
    uintptr_t   phys,
    uint32_t   *out
    )
{
    if (out == NULL) {
        return -1;
    }
    *out = *(volatile const uint32_t *)phys;
    return 0;
}

int
axl_mem_phys_read64(
    uintptr_t   phys,
    uint64_t   *out
    )
{
    if (out == NULL) {
        return -1;
    }
    *out = *(volatile const uint64_t *)phys;
    return 0;
}

int
axl_mem_phys_write8(
    uintptr_t  phys,
    uint8_t    value
    )
{
    *(volatile uint8_t *)phys = value;
    return 0;
}

int
axl_mem_phys_write16(
    uintptr_t  phys,
    uint16_t   value
    )
{
    *(volatile uint16_t *)phys = value;
    return 0;
}

int
axl_mem_phys_write32(
    uintptr_t  phys,
    uint32_t   value
    )
{
    *(volatile uint32_t *)phys = value;
    return 0;
}

int
axl_mem_phys_write64(
    uintptr_t  phys,
    uint64_t   value
    )
{
    *(volatile uint64_t *)phys = value;
    return 0;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

int
axl_mem_phys_search(
    const void   *va,
    size_t        len,
    const void   *needle,
    size_t        needle_len,
    const void  **out_match
    )
{
    if (out_match == NULL) {
        return -1;
    }
    /* Clear the out parameter unconditionally so a -1 return never
       leaves a stale pointer from a prior successful call lying
       around (defensive against callers that forget to check rc). */
    *out_match = NULL;
    void *hit = axl_memmem(va, len, needle, needle_len);
    if (hit == NULL) {
        return -1;
    }
    *out_match = hit;
    return 0;
}
