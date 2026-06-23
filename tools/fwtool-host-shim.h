/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file fwtool-host-shim.h
    Host-build shim for the backend-free fwtool unity build.

    The firmware parser (`src/fw/axl-fw.c`) and the LZMA codec
    (`src/data/axl-compress*.c`) are "backend-free" — they call AXL leaf
    primitives (allocation, memory ops) but no UEFI protocol. To run the
    SAME sources natively under the host toolchain (gcc, not the cross
    EFI compiler) we map those leaf primitives onto libc here and prevent
    the real allocator header (`axl-mem.h`, which expands `axl_malloc` to a
    debug-tracking `_impl` call needing the UEFI/native backend) from being
    processed.

    This header is force-included via `-include` so it lands at the top of
    every translation unit, BEFORE any AXL header. We:

      1. Pre-define the include guards of `axl-mem.h` / `axl-mem-impl.h` so
         their macro-based allocator (which would shadow ours and pull in
         `axl_malloc_impl`) is skipped, then supply libc-backed allocator
         macros ourselves.
      2. `#define` the memory-op leaf functions (`axl_memcpy`, …) and the
         gzip/zlib checksum leaves (`axl_crc32`, `axl_adler32`) onto libc /
         tiny stubs. The redeclarations that `axl-str.h` / `axl-digest.h`
         still emit are signature-compatible with the libc targets, so they
         parse cleanly; the checksum stubs are never reached (fwtool only
         drives `axl_decompress(AXL_COMPRESS_LZMA, …)`, and the gzip/zlib
         paths that reference them are dropped by `-Wl,--gc-sections`).

    The higher-level entry points (`axl_decompress`, `axl_lzma_decompress`,
    the `axl_fw_*` tree API) are real definitions compiled from the unity
    source set, so they are NOT shimmed here.
*/

#ifndef FWTOOL_HOST_SHIM_H
#define FWTOOL_HOST_SHIM_H

#include <stdlib.h>   /* malloc, calloc, realloc, free, strtoull */
#include <string.h>   /* memcpy, memset, memmove, memcmp, strlen */

/* ---------------------------------------------------------------------------
 * Allocation — suppress axl-mem.h's debug-tracking macros and map to libc.
 * ---------------------------------------------------------------------------
 * axl-mem.h would otherwise #define axl_malloc(size) -> axl_malloc_impl(...),
 * dragging in the native allocator backend. Defining its include guards here
 * (before any AXL header is seen) skips it entirely; we then provide libc
 * equivalents. axl_realloc is needed by the FFS tree grow path; the rest by
 * both the parser and the LZMA codec.
 */
#define AXL_MEM_H
#define AXL_MEM_IMPL_H

#define axl_malloc(size)         malloc((size))
#define axl_calloc(count, size)  calloc((count), (size))
#define axl_realloc(ptr, size)   realloc((ptr), (size))
#define axl_free(ptr)            free((ptr))

/* ---------------------------------------------------------------------------
 * Memory ops — map AXL's leaf wrappers onto libc.
 * ---------------------------------------------------------------------------
 * axl-str.h still declares these as functions; with these macros in scope
 * those declarations become signature-compatible redeclarations of the libc
 * functions (e.g. `void *memcpy(void *, const void *, size_t)`), which parse
 * cleanly. NOTE: AXL's axl_memcpy/axl_memset are documented NULL-safe; the
 * parser/codec never pass NULL to them, so plain libc semantics are fine.
 */
#define axl_memcpy   memcpy
#define axl_memset   memset
#define axl_memmove  memmove
#define axl_memcmp   memcmp

/* ---------------------------------------------------------------------------
 * Checksum leaves — referenced only by the gzip/zlib paths in
 * axl-compress.c, which --gc-sections drops (fwtool uses LZMA only). Map to
 * tiny no-op stubs so any stray reference still links; they are never run.
 * ---------------------------------------------------------------------------
 */
#define axl_crc32    fwtool_host_crc32_stub
#define axl_adler32  fwtool_host_adler32_stub

#include <stdint.h>  /* uint32_t for the checksum-stub signatures */

static inline uint32_t
fwtool_host_crc32_stub(uint32_t crc, const void *data, size_t len)
{
    (void)data;
    (void)len;
    return crc;
}

static inline uint32_t
fwtool_host_adler32_stub(uint32_t adler, const void *data, size_t len)
{
    (void)data;
    (void)len;
    return adler;
}

#endif /* FWTOOL_HOST_SHIM_H */
