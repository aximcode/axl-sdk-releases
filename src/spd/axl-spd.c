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

static AxlSmbus  *g_session           = NULL;
static bool       g_atexit_registered = false;

/* Iteration cursor (used by axl_spd_next). Storage is reused across
   calls; consumers pass &g_cursor to walk the slots in address
   order. */
static uint8_t    g_cursor       = 0;
static bool       g_cursor_valid = false;

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

/*
 * Probe predicate for axl_smbus_new_with_probe: claim a candidate
 * session iff some address in the JEDEC SPD range (0x50..0x57)
 * returns a *plausible SPD memory-type byte* (DDR4 = 0x0C, DDR5 =
 * 0x12; also accept 0x0B/0x0E/0x0F/0x10/0x11 for DDR3/LPDDR variants
 * seen in the wild). Bare "read succeeded" isn't enough — some I2C
 * masters terminate transactions with 0x00 even when no slave is
 * present, and some DDR5 hubs ack with 0xFF for non-existent
 * registers, both of which would cause us to claim the wrong bus.
 *
 * DDR4 vs DDR5 layout:
 *   DDR4 (ee1004 hub): SPD content is mapped directly. Byte 0 is
 *                      the memory-type field. Read 1 byte at reg 0.
 *   DDR5 (SPD5 hub):   Hub MR registers at 0x00..0x7F (byte 0 is
 *                      a vendor MR, often 0xFF on factory part);
 *                      page-selected SPD content at 0x80..0xFF.
 *                      First write MR11=0 (page 0), then read at
 *                      0x80 to get the memory-type field.
 *
 * We try DDR4 first (cheaper, no write); fall back to DDR5 if
 * the byte 0 read either fails or returns implausible-for-DDR4.
 */
static bool
spd_bus_probe(
    AxlSmbus  *cand,
    void      *user
    )
{
    (void)user;
    for (uint8_t addr = AXL_SPD_ADDR_FIRST; addr <= AXL_SPD_ADDR_LAST; addr++) {
        /* DDR4 path: byte 0 = memory-type directly. */
        uint8_t byte0;
        if (axl_smbus_read_byte(cand, addr, 0x00, &byte0) != AXL_OK) {
            /* Slave didn't ack — don't attempt the DDR5 write to this
             * address. Otherwise we'd be blind-writing register 0x0B = 0
             * to whatever non-SPD device might respond on a different
             * bus (PMBus PSU, board sensor, etc.). */
            continue;
        }
        if (byte0 >= 0x09 && byte0 <= 0x12) {
            return true;
        }

        /* DDR5 path — slave acked but byte 0 wasn't a DDR4 mem-type.
         * For SPD5118 hubs, MR0 (0x00) MUST equal 0x18 and MR1 (0x01)
         * MUST equal 0x51 (Linux's spd5118.c uses i2c_smbus_read_word_
         * swapped at MR0 expecting 0x5118). If MR0 doesn't match, this
         * either isn't a DDR5 hub or it's stuck on a non-zero page (a
         * Renesas/ITD-style strict-masking variant — see spd5118.c
         * lines 645-676 for the recovery dance). */
        if (byte0 != AXL_SPD_DDR5_DEVTYPE_LSB) {
            /* Stuck-page recovery — match Linux's spd5118_i2c_init
             * (spd5118.c:645-676): the strict-masking variants
             * (Renesas/ITD) zero out MR space when the hub is left
             * paged on a non-zero page. To recognize this state we
             * require ALL of: MR0=MR1=MR3=MR4=0 AND MR11 page-bits
             * non-zero. Skipping any check risks rewriting MR11 on
             * a non-SPD5118 slave that happens to ack at this
             * address. */
            uint8_t mr1  = 0xFF;
            uint8_t mr3  = 0xFF;
            uint8_t mr4  = 0xFF;
            uint8_t mr11 = 0;
            if (byte0 != 0x00 ||
                axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR1, &mr1)  != AXL_OK ||
                mr1 != 0x00 ||
                axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR3, &mr3)  != AXL_OK ||
                mr3 != 0x00 ||
                axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR4, &mr4)  != AXL_OK ||
                mr4 != 0x00 ||
                axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR11, &mr11) != AXL_OK ||
                (mr11 & AXL_SPD_DDR5_MR11_PAGE_MASK) == 0)
            {
                continue;   /* Not a stuck-page SPD5118; not a hub. */
            }
            /* Write MR11 = (mr11 & ADDR_BIT) to clear the page bits
             * while preserving the addr-mode configuration the BIOS
             * chose. */
            if (axl_smbus_write_byte(cand, addr, AXL_SPD_DDR5_MR11,
                                     (uint8_t)(mr11 & AXL_SPD_DDR5_MR11_ADDR_BIT))
                != AXL_OK)
            {
                continue;
            }
            if (axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR0, &byte0) != AXL_OK
                || byte0 != AXL_SPD_DDR5_DEVTYPE_LSB)
            {
                /* Recovery failed — restore original MR11 best-effort
                 * and move on. */
                (void)axl_smbus_write_byte(cand, addr, AXL_SPD_DDR5_MR11, mr11);
                continue;
            }
            /* Recovery succeeded — fall through to MR1 verification. */
        }

        /* MR0 == 0x18; verify MR1 == 0x51 to complete the 0x5118 ID. */
        uint8_t mr1 = 0;
        if (axl_smbus_read_byte(cand, addr, AXL_SPD_DDR5_MR1, &mr1) != AXL_OK
            || mr1 != AXL_SPD_DDR5_DEVTYPE_MSB)
        {
            continue;
        }
        return true;
    }
    return false;
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
       SmbusHcShim (or similar) after our first probe attempt. */
    g_session = axl_smbus_new_with_probe(spd_bus_probe, NULL);
    if (g_session == NULL) {
        /* Fallback for hosts where SPDs aren't reachable via a probe
         * (e.g., DIMMs not yet enumerated, or BMC owns the bus during
         * BDS): take whatever SMBus controller LocateProtocol returns
         * so caller-driven addressing still works. */
        g_session = axl_smbus_new();
    }
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

static bool
slot_present(
    uint8_t  addr
    )
{
    uint8_t byte0 = 0;
    return axl_smbus_read_byte(g_session, addr, 0x00, &byte0) == AXL_OK;
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
        return AXL_ERR;
    }
    if (ensure_session() != 0) {
        return AXL_ERR;
    }
    /* Codec selection — DDR5 SPD5118 hubs vs flat DDR4 EE1004 EEPROMs
     * have different register-vs-content mappings. The legacy heuristic
     * "read byte at offset 2" works on DDR4 (memory-type byte sits at
     * content offset 2 = register 2) but fails on DDR5 (register 2
     * is MR2 = revision; memory-type byte lives at register 0x82
     * after page-select). So check the SPD5118 device-ID at MR0:MR1
     * first; fall through to byte-2 sampling only when it doesn't
     * match. */
    uint8_t id_lo = 0;
    uint8_t id_hi = 0;
    if (axl_smbus_read_byte(g_session, addr, AXL_SPD_DDR5_MR0, &id_lo) == AXL_OK
        && id_lo == AXL_SPD_DDR5_DEVTYPE_LSB
        && axl_smbus_read_byte(g_session, addr, AXL_SPD_DDR5_MR1, &id_hi) == AXL_OK
        && id_hi == AXL_SPD_DDR5_DEVTYPE_MSB)
    {
        return axl_spd_ddr5_read(g_session, addr, buf, cap, len);
    }
    /* Not a DDR5 hub (or stuck on a non-zero page — spd_bus_probe
     * handles recovery during probe; if we got here without going
     * through probe, accept the platform as DDR4-or-unknown). */
    uint8_t mem_type = 0;
    if (axl_smbus_read_byte(g_session, addr, 0x02, &mem_type) != AXL_OK) {
        return AXL_ERR;
    }
    switch (mem_type) {
        case AXL_SPD_TYPE_DDR4:
            return axl_spd_ddr4_read(g_session, addr, buf, cap, len);
        case AXL_SPD_TYPE_DDR5:
            /* Reached only on systems where MR0:MR1 doesn't return
             * 0x18:0x51 but byte 2 reads as 0x12 — unusual but match
             * the legacy heuristic for compatibility. */
            return axl_spd_ddr5_read(g_session, addr, buf, cap, len);
        default:
            /* Unknown / unprogrammed — read the lower 256 bytes anyway
               so the caller can inspect what's there. */
            {
                size_t to_read = cap < 256 ? cap : 256;
                for (size_t i = 0; i < to_read; i++) {
                    if (axl_smbus_read_byte(g_session, addr,
                                            (uint8_t)i, &buf[i]) != AXL_OK) {
                        if (i == 0) {
                            return AXL_ERR;
                        }
                        *len = i;
                        return AXL_OK;
                    }
                }
                *len = to_read;
                return AXL_OK;
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
        return AXL_ERR;
    }
    uint8_t buf[AXL_SPD_RAW_MAX];
    size_t  len = 0;
    if (axl_spd_dump_raw(addr, buf, sizeof(buf), &len) != AXL_OK) {
        return AXL_ERR;
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
        return AXL_ERR;
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
            return AXL_OK;
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
