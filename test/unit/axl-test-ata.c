/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-ata.c
    Unit tests for the AxlAta raw-buffer decoders (IDENTIFY DEVICE, SMART
    READ DATA + THRESHOLDS, self-test execution status). Pure functions
    over fixed buffers — no ATA device, no QEMU. The device-facing path
    (axl_ata_next over EFI_ATA_PASS_THRU) is exercised by
    test/integration/test-ata-qemu.sh against an emulated AHCI disk.
**/

#include <axl.h>
#include "axl-test.h"

static void
put_le16(uint8_t *b, size_t off, uint16_t v)
{
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
}

static void
put_le32(uint8_t *b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[off + (size_t)i] = (uint8_t)(v >> (8 * i));
    }
}

/* Store an ASCII string into an ATA identity field, byte-swapped within
   each 16-bit word (the ATA convention the decoder must undo). Pads with
   spaces to @p nbytes. */
static void
put_ata_string(uint8_t *b, size_t off, size_t nbytes, const char *s)
{
    size_t n = axl_strlen(s);
    for (size_t i = 0; i < nbytes; i += 2) {
        char c0 = (i < n) ? s[i] : ' ';
        char c1 = (i + 1 < n) ? s[i + 1] : ' ';
        b[off + i]     = (uint8_t)c1;   /* swapped */
        b[off + i + 1] = (uint8_t)c0;
    }
}

// ---------------------------------------------------------------------------
// IDENTIFY DEVICE
// ---------------------------------------------------------------------------

static void
test_identify(void)
{
    uint8_t id[512] = { 0 };
    put_ata_string(id, 20, 20, "SER1234567890");        /* words 10..19 */
    put_ata_string(id, 46, 8, "FW2.0");                 /* words 23..26 */
    put_ata_string(id, 54, 40, "AXL TEST SATA SSD");    /* words 27..46 */
    put_le16(id, 164, 0x0001);                          /* word 82: SMART bit0 */
    put_le16(id, 166, (uint16_t)(1u << 10));            /* word 83: 48-bit LBA */
    put_le32(id, 200, 0x08000000);                      /* words 100..101: 48-bit count low */
    /* word 106 (byte 212) bit12 clear -> 512-byte sectors. */

    AxlAtaIdentify d = { 0 };
    test_check(axl_ata_decode_identify(id, sizeof(id), &d) == AXL_OK,
               "ata identify: decode -> AXL_OK");
    test_check(axl_strcmp(d.serial, "SER1234567890") == 0,
               "ata identify: serial de-swapped + trimmed");
    test_check(axl_strcmp(d.firmware, "FW2.0") == 0,
               "ata identify: firmware de-swapped");
    test_check(axl_strcmp(d.model, "AXL TEST SATA SSD") == 0,
               "ata identify: model de-swapped + trimmed");
    test_check(d.smart_supported, "ata identify: SMART supported (word 82 bit0)");
    test_check(d.size_blocks == 0x08000000, "ata identify: 48-bit LBA count");
    test_check(d.block_size == 512, "ata identify: default 512-byte sectors");
    test_check(d.capacity_bytes == (uint64_t)0x08000000 * 512,
               "ata identify: capacity = blocks * block_size");

    test_check(axl_ata_decode_identify(id, 511, &d) == AXL_ERR,
               "ata identify: short buffer -> AXL_ERR");
    test_check(axl_ata_decode_identify(NULL, 512, &d) == AXL_ERR,
               "ata identify: NULL buffer -> AXL_ERR");

    /* 28-bit fallback when word 83 bit10 is clear. */
    uint8_t id28[512] = { 0 };
    put_le16(id28, 164, 0x0001);
    put_le32(id28, 120, 0x00100000);   /* words 60..61: 28-bit count */
    test_check(axl_ata_decode_identify(id28, sizeof(id28), &d) == AXL_OK
                   && d.size_blocks == 0x00100000,
               "ata identify: 28-bit LBA count when 48-bit unsupported");
}

// ---------------------------------------------------------------------------
// SMART READ DATA + THRESHOLDS -> health
// ---------------------------------------------------------------------------

/* Write one attribute entry (12 bytes) at attribute slot @p i of a SMART
   READ DATA buffer: id, flags (bit0 = pre-fail), normalized value, raw. */
static void
put_attr(uint8_t *data, int i, uint8_t id, uint16_t flags, uint8_t value,
         uint64_t raw)
{
    size_t off = 2 + (size_t)i * 12;
    data[off] = id;
    put_le16(data, off + 1, flags);
    data[off + 3] = value;       /* current normalized */
    data[off + 4] = value;       /* worst */
    for (int k = 0; k < 6; k++) {
        data[off + 5 + (size_t)k] = (uint8_t)(raw >> (8 * k));
    }
}

/* Write a threshold entry (12 bytes) at slot @p i: id + threshold. */
static void
put_thresh(uint8_t *thr, int i, uint8_t id, uint8_t threshold)
{
    size_t off = 2 + (size_t)i * 12;
    thr[off] = id;
    thr[off + 1] = threshold;
}

#define ATTR_PREFAIL  0x0001

static void
test_smart(void)
{
    uint8_t data[512] = { 0 };
    uint8_t thr[512]  = { 0 };
    AxlAtaSmart s = { 0 };

    /* Universal attributes: 5 reallocated (pre-fail, value 100, thr 10),
       9 power-on-hours, 12 power-cycles, 194 temperature. */
    put_attr(data, 0, 5,   ATTR_PREFAIL, 100, 0);       /* reallocated raw 0 */
    put_attr(data, 1, 9,   0,            99,  8760);     /* POH 8760 */
    put_attr(data, 2, 12,  0,            99,  42);       /* power cycles 42 */
    put_attr(data, 3, 194, 0,            60,  40);       /* temp raw[0]=40 C */
    put_thresh(thr, 0, 5, 10);

    test_check(axl_ata_decode_smart(data, thr, 512, &s) == AXL_OK,
               "ata smart: decode -> AXL_OK");
    test_check(s.healthy, "ata smart: healthy (reallocated 100 > threshold 10)");
    test_check(s.temperature_c == 40, "ata smart: temperature from attr 194");
    test_check(s.power_on_hours == 8760, "ata smart: power-on hours (attr 9)");
    test_check(s.power_cycles == 42, "ata smart: power cycles (attr 12)");
    test_check(s.reallocated_sectors == 0, "ata smart: reallocated raw (attr 5)");
    test_check(s.percent_used == 0xFF, "ata smart: percent_used always unknown");

    /* Drop the pre-fail attribute to its threshold -> failing. */
    put_attr(data, 0, 5, ATTR_PREFAIL, 10, 1200);   /* value == threshold 10 */
    test_check(axl_ata_decode_smart(data, thr, 512, &s) == AXL_OK
                   && !s.healthy && s.reallocated_sectors == 1200,
               "ata smart: pre-fail attr at threshold -> FAILING");

    /* An OLD-AGE attribute at/below threshold must NOT fail health. */
    uint8_t d2[512] = { 0 };
    uint8_t t2[512] = { 0 };
    put_attr(d2, 0, 190, 0 /* not pre-fail */, 5, 45);  /* old-age temp */
    put_thresh(t2, 0, 190, 40);
    test_check(axl_ata_decode_smart(d2, t2, 512, &s) == AXL_OK && s.healthy,
               "ata smart: old-age attr below threshold stays healthy");

    /* No temperature attribute -> sentinel. */
    uint8_t d3[512] = { 0 };
    uint8_t t3[512] = { 0 };
    put_attr(d3, 0, 9, 0, 99, 100);
    test_check(axl_ata_decode_smart(d3, t3, 512, &s) == AXL_OK
                   && s.temperature_c == INT32_MIN
                   && s.power_cycles == UINT64_MAX
                   && s.reallocated_sectors == UINT64_MAX,
               "ata smart: absent attrs use sentinels");

    test_check(axl_ata_decode_smart(data, thr, 100, &s) == AXL_ERR,
               "ata smart: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Self-test execution status (SMART READ DATA byte 363)
// ---------------------------------------------------------------------------

static void
test_self_test(void)
{
    uint8_t data[512] = { 0 };
    AxlAtaSelfTestResult r = { 0 };

    /* Completed without error (status 0). */
    data[363] = 0x00;
    test_check(axl_ata_decode_self_test(data, sizeof(data), &r) == AXL_OK
                   && !r.in_progress && r.passed && r.result_code == 0
                   && r.percent_complete == 0,
               "ata self-test: completed-without-error");

    /* In progress (status 0xF), 30% remaining -> 70% complete. */
    data[363] = 0xF3;
    test_check(axl_ata_decode_self_test(data, sizeof(data), &r) == AXL_OK
                   && r.in_progress && r.percent_complete == 70
                   && !r.passed && r.result_code == 0xF,
               "ata self-test: in progress 70% complete");

    /* Completed with a read failure (status 5). */
    data[363] = 0x50;
    test_check(axl_ata_decode_self_test(data, sizeof(data), &r) == AXL_OK
                   && !r.in_progress && !r.passed && r.result_code == 5
                   && r.percent_complete == 0,
               "ata self-test: completed failure -> result 5");

    test_check(axl_ata_decode_self_test(data, 200, &r) == AXL_ERR,
               "ata self-test: short buffer -> AXL_ERR");
}

static int
test_ata_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlAta");

    test_identify();
    test_smart();
    test_self_test();

    return test_print_results();
}

AXL_APP(test_ata_main)
