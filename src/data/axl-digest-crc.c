/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest-crc.c
    Rolling 32-bit checksums: CRC-32 (gzip/zlib/PNG) and Adler-32
    (RFC 1950 / zlib). Both follow zlib's update-and-fold contract so
    they compose across streamed chunks. Standalone — no dependency on
    the AxlChecksum machinery (those are the cryptographic hashes).

    These back the gzip/zlib framing in AxlCompress but are public in
    their own right (integrity checks, content addressing, etc.).
**/

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-digest.h>

// ---------------------------------------------------------------------------
// CRC-32 — reflected polynomial 0xEDB88320 (gzip / zlib / PNG)
// ---------------------------------------------------------------------------

/* Lazily-built 256-entry table. UEFI boot services are single-threaded,
   so the unguarded lazy init is safe; the table is idempotent anyway —
   a racing rebuild would write identical values. */
static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void
crc32_build_table(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[n] = c;
    }
    crc32_table_ready = true;
}

uint32_t
axl_crc32(uint32_t crc, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return crc;
    }
    if (!crc32_table_ready) {
        crc32_build_table();
    }

    const uint8_t *p = (const uint8_t *)data;
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Adler-32 — RFC 1950 (zlib). Two 16-bit sums mod 65521.
// ---------------------------------------------------------------------------

/* Largest n such that 255*n*(n+1)/2 + (n+1)*(65521-1) fits in 32 bits,
   i.e. how many bytes we can accumulate before the modulo is required. */
#define ADLER_MOD  65521u
#define ADLER_NMAX 5552u

uint32_t
axl_adler32(uint32_t adler, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return adler;
    }

    const uint8_t *p  = (const uint8_t *)data;
    uint32_t       s1 = adler & 0xFFFFu;
    uint32_t       s2 = (adler >> 16) & 0xFFFFu;

    while (len > 0) {
        size_t block = (len < ADLER_NMAX) ? len : ADLER_NMAX;
        len -= block;
        while (block-- > 0) {
            s1 += *p++;
            s2 += s1;
        }
        s1 %= ADLER_MOD;
        s2 %= ADLER_MOD;
    }
    return (s2 << 16) | s1;
}
