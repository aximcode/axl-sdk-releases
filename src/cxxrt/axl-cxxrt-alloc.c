/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-alloc.c
    Allocator bridge for the EXCEPTIONS build: newlib's allocator entry points
    routed onto AXL's.

    SEPARATE OBJECT from the frame-table lifecycle on purpose. Archive members
    are all-or-nothing, and every C++ link pulls this one -- the toolchain's
    `operator new` calls `malloc`. Keeping the `__eh_frame_start` reference in
    a different member means a link can take the allocator bridge WITHOUT
    opting into the exceptions linker script. Merged, `--no-undefined` fires on
    `__eh_frame_start` before `--gc-sections` can collect the unreferenced
    function holding it, and nothing can link the archive at all. Same reason
    `src/runtime/axl-cxxabi.c` is its own object.

    Why bridge at all: the toolchain's `operator new` calls `malloc`, so this
    is what keeps every C++ allocation inside AXL's tracker. Without it the
    leak gate goes blind on exactly the code most likely to leak. (Tracking is
    `AXL_MEM_DEBUG`-gated; a RELEASE build tracks nothing either way.)

    Overriding a libc symbol works here only because a definition in an OBJECT
    prevents the linker pulling the archive member that would also define it.
    That is why `_impure_ptr` is deliberately NOT bridged: newlib's impure.o is
    pulled for other symbols regardless, so defining it is a
    multiple-definition error rather than an override.
**/

#include <axl.h>

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Allocator bridge
// ---------------------------------------------------------------------------
//
// axl_malloc_impl directly, not the axl_malloc macro: the macro records
// __FILE__/__LINE__, which for a bridge means every C++ allocation in the
// program is attributed to one line of this file. A fixed label at least says
// WHICH subsystem, and the leak gate counts either way.

#define CXXRT_ORIGIN   "<c++ runtime>"

void *
malloc(
    size_t size
    )
{
    return axl_malloc_impl(size, CXXRT_ORIGIN, 0);
}

void
free(
    void *ptr
    )
{
    axl_free(ptr);
}

void *
realloc(
    void  *ptr,
    size_t size
    )
{
    return axl_realloc(ptr, size);
}

void *
calloc(
    size_t count,
    size_t size
    )
{
    return axl_calloc(count, size);
}

/**
 * @brief Aligned allocation, for over-aligned `operator new`.
 *
 * `memalign`, NOT `posix_memalign`: measured with `nm -u` on both toolchains,
 * libsupc++ references `memalign` (from new_opa.o, i.e.
 * `operator new(size_t, align_val_t)`) and references `posix_memalign`
 * nowhere. Bridging the wrong one leaves over-aligned new resolving to
 * newlib's own allocator, which reaches `_sbrk` -> -1 -> NULL -> `bad_alloc`
 * for any `alignas(32)` type.
 *
 * LIMITATION, deliberately fail-closed: axl_malloc's blocks are 8/16-byte
 * aligned, so an alignment above that cannot be honoured and this returns
 * NULL rather than a misaligned block. `operator new` turns that into
 * `std::bad_alloc` at the allocation site; a misaligned block would fault
 * later and elsewhere. Supporting larger alignments needs a real aligned path
 * in AxlMem, not a wider guard here.
 */
void *
memalign(
    size_t alignment,
    size_t size
    )
{
    if (alignment > 16u) {
        return NULL;
    }
    return axl_malloc_impl(size, CXXRT_ORIGIN, 0);
}

// ---------------------------------------------------------------------------
// The heap boundary
// ---------------------------------------------------------------------------

/**
 * @brief Refuse to grow a break: AXL owns the heap.
 *
 * BOTH spellings, because the toolchains disagree and each needs its own --
 * measured, not assumed:
 *
 *     x64  libnosys.a sbrk.o defines `sbrk`
 *     aa64 libnosys.a sbrk.o defines `_sbrk`
 *
 * and BOTH reference the linker symbol `end`, which AXL's linker scripts do
 * not define. Defining a spelling keeps the matching libnosys object out of
 * the link entirely; getting it wrong per-arch fails on `end` with no hint
 * that spelling is the issue. Defining only one covered exactly one arch, in
 * each direction.
 *
 * INVARIANT, not merely honesty: returning -1 is what keeps newlib's heap
 * from ever obtaining a byte, and that is load-bearing for MEMORY SAFETY.
 * This file bridges the plain names but NOT the reentrant `_malloc_r` /
 * `_free_r` / `_calloc_r` / `_realloc_r` family, which ~45 newlib objects use
 * (stdio, mprec's float formatting, strdup_r). Those reach newlib's own
 * dlmalloc. Two allocators coexisting is fine only while the second can never
 * obtain memory: the moment `_sbrk` succeeds, a pointer from `strdup()` can
 * reach `axl_free()`, which reads a bookkeeping header that was never
 * written. Anything that makes newlib's heap work must bridge the `_r` family
 * FIRST.
 */
void *
sbrk(
    intptr_t increment
    )
{
    (void)increment;
    return (void *)-1;
}

void *
_sbrk(
    intptr_t increment
    )
{
    (void)increment;
    return (void *)-1;
}
