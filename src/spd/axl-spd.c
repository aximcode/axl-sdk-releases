/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd.c
    AxlSpd — JEDEC SPD reader for DDR4/DDR5 DIMMs.

    Holds a single AxlSmbus session, opened lazily on first call.
    Iteration walks 0x50..0x57 and probes each via a byte read at
    offset 0; a successful read marks the slot as populated. The
    memory-type byte at offset 2 selects the codec (DDR4 / DDR5).

    The decoded view is intentionally small — manufacturer codes
    are exposed raw (16-bit JEP-106 packed bank+id) so the policy
    of mapping codes to vendor names lives in the tool layer (see
    tools/memspd.c for the JSON-sidecar pattern).
**/

#include "axl-spd-internal.h"

#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-atexit.h>

AXL_LOG_DOMAIN("spd");

// ---------------------------------------------------------------------------
// Lazy session
// ---------------------------------------------------------------------------

static AxlSmbus  *g_session         = NULL;
static bool       g_atexit_registered = false;

static void
spd_cleanup(
    void  *cookie
    )
{
    (void)cookie;
    if (g_session != NULL) {
        axl_smbus_free(g_session);
        g_session = NULL;
    }
}

static int
ensure_session(
    void
    )
{
    if (g_session != NULL) {
        return 0;
    }
    /* Don't cache a sticky failure — a UEFI shell session may load
       SmbusHcShim (or similar) after our first probe attempt. We re-
       try axl_smbus_new() on each call; the cost is one LocateProtocol
       round-trip, negligible compared to the SMBus reads that follow. */
    g_session = axl_smbus_new();
    if (g_session == NULL) {
        axl_debug("SMBus controller unavailable; SPD access disabled");
        return -1;
    }
    if (!g_atexit_registered) {
        axl_atexit(spd_cleanup, NULL);
        g_atexit_registered = true;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

static uint8_t  g_cursor       = 0;
static bool     g_cursor_valid = false;

static bool
slot_present(
    uint8_t  addr
    )
{
    uint8_t byte0 = 0;
    return axl_smbus_read_byte(g_session, addr, 0x00, &byte0) == 0;
}

uint8_t *
axl_spd_next(
    uint8_t  *prev
    )
{
    if (ensure_session() != 0) {
        return NULL;
    }

    /* Cursor convention mirrors axl_pci_next: prev is a one-bit
       continue/restart signal — only &g_cursor (the previous return
       value) advances the walk; anything else restarts it. */
    uint8_t start;
    if (prev == NULL || prev != &g_cursor || !g_cursor_valid) {
        start = AXL_SPD_ADDR_FIRST;
    } else {
        start = g_cursor + 1;
    }

    for (uint8_t a = start; a <= AXL_SPD_ADDR_LAST; a++) {
        if (slot_present(a)) {
            g_cursor       = a;
            g_cursor_valid = true;
            return &g_cursor;
        }
    }
    g_cursor_valid = false;
    return NULL;
}

// ---------------------------------------------------------------------------
// Read + decode
// ---------------------------------------------------------------------------

int
axl_spd_dump_raw(
    uint8_t   addr,
    uint8_t  *buf,
    size_t    cap,
    size_t   *len
    )
{
    if (buf == NULL || len == NULL || cap == 0) {
        return -1;
    }
    if (ensure_session() != 0) {
        return -1;
    }
    /* Probe + check memory-type byte to choose codec. */
    uint8_t mem_type = 0;
    if (axl_smbus_read_byte(g_session, addr, 0x02, &mem_type) != 0) {
        return -1;
    }
    switch (mem_type) {
        case AXL_SPD_TYPE_DDR4:
            return axl_spd_ddr4_read(g_session, addr, buf, cap, len);
        case AXL_SPD_TYPE_DDR5:
            return axl_spd_ddr5_read(g_session, addr, buf, cap, len);
        default:
            /* Unknown / unprogrammed — read the lower 256 bytes anyway
               so the caller can inspect what's there. */
            {
                size_t to_read = cap < 256 ? cap : 256;
                for (size_t i = 0; i < to_read; i++) {
                    if (axl_smbus_read_byte(g_session, addr,
                                            (uint8_t)i, &buf[i]) != 0) {
                        if (i == 0) {
                            return -1;
                        }
                        *len = i;
                        return 0;
                    }
                }
                *len = to_read;
                return 0;
            }
    }
}

int
axl_spd_read(
    uint8_t      addr,
    AxlSpdInfo  *out
    )
{
    if (out == NULL) {
        return -1;
    }
    uint8_t buf[AXL_SPD_RAW_MAX];
    size_t  len = 0;
    if (axl_spd_dump_raw(addr, buf, sizeof(buf), &len) != 0) {
        return -1;
    }
    return axl_spd_decode(buf, len, out);
}

int
axl_spd_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
    )
{
    if (buf == NULL || out == NULL || len < 3) {
        return -1;
    }
    axl_memset(out, 0, sizeof(*out));

    switch (buf[2]) {
        case AXL_SPD_TYPE_DDR4:
            return axl_spd_ddr4_decode(buf, len, out);
        case AXL_SPD_TYPE_DDR5:
            return axl_spd_ddr5_decode(buf, len, out);
        default:
            /* Memory-type byte is unrecognised. Leave ddr_generation = 0
               and report success so callers can still see the empty
               struct (matches QEMU's default zero-init EEPROMs). */
            out->ddr_generation = 0;
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Shared codec helpers
// ---------------------------------------------------------------------------

uint16_t
axl_spd_pack_mfg_code(
    uint8_t  bank,
    uint8_t  id_byte
    )
{
    /* Preserve the raw on-the-wire byte (don't strip JEDEC's odd-parity
       MSB) so the packed code matches what consumers see when they
       hand-read SPD bytes — e.g., Samsung is 0x00CE, not 0x004E. The
       parity bit is informational only; lookup tables key by the raw
       byte. 0x00 (unprogrammed) and 0xFF (erased) both report as
       "unset" since neither is a valid JEP-106 position. */
    if (id_byte == 0x00 || id_byte == 0xFF) {
        return 0;
    }
    return ((uint16_t)bank << 8) | id_byte;
}

void
axl_spd_copy_part_number(
    char           *dst,
    size_t          dst_cap,
    const uint8_t  *src,
    size_t          src_len
    )
{
    if (dst == NULL || dst_cap == 0) {
        return;
    }
    if (src_len >= dst_cap) {
        src_len = dst_cap - 1;
    }
    /* Copy printable ASCII; replace 0xFF (unprogrammed) with NUL stop. */
    size_t out_len = 0;
    for (size_t i = 0; i < src_len; i++) {
        uint8_t c = src[i];
        if (c == 0x00 || c == 0xFF) {
            break;
        }
        dst[out_len++] = (char)c;
    }
    /* Trim trailing spaces. */
    while (out_len > 0 && dst[out_len - 1] == ' ') {
        out_len--;
    }
    dst[out_len] = '\0';
}

uint8_t
axl_spd_bcd_to_u8(
    uint8_t  bcd
    )
{
    uint8_t hi = (bcd >> 4) & 0x0F;
    uint8_t lo = bcd & 0x0F;
    if (hi > 9 || lo > 9) {
        return 0xFF;
    }
    return (uint8_t)(hi * 10 + lo);
}
