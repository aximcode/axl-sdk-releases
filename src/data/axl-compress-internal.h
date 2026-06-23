/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-compress-internal.h
    Internal definitions shared between axl-compress.c and
    axl-compress-lzma.c. Not part of the public API. */

#ifndef AXL_COMPRESS_INTERNAL_H
#define AXL_COMPRESS_INTERNAL_H

#include <stddef.h>

/* Upper bound on a single decompressed buffer. Caps both the gzip
   ISIZE-driven allocation (a forged ISIZE can't trigger a multi-GB
   alloc) and the unknown-size grow loop for zlib/raw (a decompression
   bomb can't expand without limit). 512 MiB is far beyond any fixture,
   firmware section, or tar.gz this library handles in a UEFI context. */
#define AXL_COMPRESS_MAX_OUTPUT  (512u * 1024u * 1024u)

/* Declarations for the LZMA codec implemented in axl-compress-lzma.c. */
int axl_lzma_decompress(const void *in, size_t in_len,
                        void **out, size_t *out_len);
int axl_lzma_compress(const void *in, size_t in_len,
                      void **out, size_t *out_len, int level);

#endif /* AXL_COMPRESS_INTERNAL_H */
