/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-scsi.c
    Unit tests for the AxlScsi pure raw-buffer decoders — standard INQUIRY,
    VPD page 0x80 serial, READ CAPACITY (16), the Informational Exceptions
    log page (0x2F), and the Temperature log page (0x0D). Pure functions over
    fixed big-endian buffers — no SCSI device, no QEMU. The device-facing path
    (axl_scsi_next / inquiry / read_capacity / health over
    EFI_EXT_SCSI_PASS_THRU) is exercised by test/integration/test-scsi-qemu.sh
    against an emulated scsi-hd.
**/

#include <axl.h>
#include "axl-test.h"

/* Store a big-endian value of @p nbytes at @p off. */
static void
put_be(uint8_t *b, size_t off, uint64_t v, size_t nbytes)
{
    for (size_t i = 0; i < nbytes; i++) {
        b[off + nbytes - 1 - i] = (uint8_t)(v >> (8 * i));
    }
}

// ---------------------------------------------------------------------------
// INQUIRY (standard)
// ---------------------------------------------------------------------------

static void
test_inquiry(void)
{
    uint8_t d[36] = { 0 };
    d[0] = AXL_SCSI_DEVTYPE_DISK;          /* peripheral device type 0 */
    d[1] = 0x80;                           /* RMB: removable */
    axl_memcpy(d + 8,  "AXLVND  ", 8);     /* vendor (trailing spaces) */
    axl_memcpy(d + 16, "AXL SCSI DISK   ", 16);
    axl_memcpy(d + 32, "1.0 ", 4);

    AxlScsiInquiry inq = { 0 };
    test_check(axl_scsi_decode_inquiry(d, sizeof(d), &inq) == AXL_OK,
               "scsi inquiry: decode -> AXL_OK");
    test_check(axl_strcmp(inq.vendor, "AXLVND") == 0,
               "scsi inquiry: vendor trimmed");
    test_check(axl_strcmp(inq.product, "AXL SCSI DISK") == 0,
               "scsi inquiry: product trimmed");
    test_check(axl_strcmp(inq.revision, "1.0") == 0,
               "scsi inquiry: revision trimmed");
    test_check(inq.device_type == AXL_SCSI_DEVTYPE_DISK,
               "scsi inquiry: device type = disk");
    test_check(inq.removable, "scsi inquiry: removable bit set");

    /* Non-removable, CD-ROM. */
    d[0] = AXL_SCSI_DEVTYPE_CDROM;
    d[1] = 0x00;
    test_check(axl_scsi_decode_inquiry(d, sizeof(d), &inq) == AXL_OK
                   && inq.device_type == AXL_SCSI_DEVTYPE_CDROM
                   && !inq.removable,
               "scsi inquiry: cdrom, not removable");

    test_check(axl_scsi_decode_inquiry(d, 35, &inq) == AXL_ERR,
               "scsi inquiry: short buffer -> AXL_ERR");
    test_check(axl_scsi_decode_inquiry(NULL, 36, &inq) == AXL_ERR,
               "scsi inquiry: NULL buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// VPD page 0x80 serial
// ---------------------------------------------------------------------------

static void
test_serial(void)
{
    uint8_t v[12] = { 0 };
    v[0] = 0x00;
    v[1] = 0x80;            /* page code */
    put_be(v, 2, 8, 2);     /* page length = 8 */
    axl_memcpy(v + 4, "SN12345 ", 8);   /* serial, trailing space */

    char serial[21] = { 0 };
    test_check(axl_scsi_decode_serial(v, sizeof(v), serial, sizeof(serial)) == AXL_OK
                   && axl_strcmp(serial, "SN12345") == 0,
               "scsi serial: VPD 0x80 trimmed");

    /* Zero-length serial page -> "". */
    uint8_t empty[4] = { 0x00, 0x80, 0x00, 0x00 };
    serial[0] = 'x';
    test_check(axl_scsi_decode_serial(empty, sizeof(empty), serial, sizeof(serial)) == AXL_OK
                   && serial[0] == '\0',
               "scsi serial: empty page -> \"\"");

    test_check(axl_scsi_decode_serial(v, 3, serial, sizeof(serial)) == AXL_ERR,
               "scsi serial: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// READ CAPACITY (16)
// ---------------------------------------------------------------------------

static void
test_capacity(void)
{
    uint8_t d[32] = { 0 };
    put_be(d, 0, 0x1FFFF, 8);   /* returned LBA = 131071 -> 131072 blocks */
    put_be(d, 8, 512, 4);       /* block length 512 */

    AxlScsiCapacity cap = { 0 };
    test_check(axl_scsi_decode_capacity(d, sizeof(d), &cap) == AXL_OK,
               "scsi capacity: decode -> AXL_OK");
    test_check(cap.size_blocks == 131072,
               "scsi capacity: blocks = last LBA + 1");
    test_check(cap.block_size == 512, "scsi capacity: block size 512");
    test_check(cap.capacity_bytes == (uint64_t)131072 * 512,
               "scsi capacity: bytes = blocks * block size");

    test_check(axl_scsi_decode_capacity(d, 15, &cap) == AXL_ERR,
               "scsi capacity: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Informational Exceptions log page (0x2F) -> health
// ---------------------------------------------------------------------------

/* Build an IE log page: header(4) + param header(4) + {asc, ascq, temp}. */
static size_t
build_ie(uint8_t *b, uint8_t asc, uint8_t ascq, uint8_t temp, bool with_temp)
{
    b[0] = 0x2F;                 /* page code */
    b[1] = 0x00;                 /* subpage */
    uint8_t plen = with_temp ? 3 : 2;
    put_be(b, 2, (uint64_t)(4 + plen), 2);  /* page length: param hdr + data */
    put_be(b, 4, 0x0000, 2);     /* parameter code */
    b[6] = 0x00;                 /* control */
    b[7] = plen;                 /* parameter length */
    b[8] = asc;
    b[9] = ascq;
    if (with_temp) {
        b[10] = temp;
    }
    return (size_t)(8 + plen);
}

static void
test_health(void)
{
    uint8_t b[16] = { 0 };
    size_t n = build_ie(b, 0x00, 0x00, 0x23, true);   /* healthy, 35 C */

    AxlScsiHealth h = { 0 };
    test_check(axl_scsi_decode_health(b, n, &h) == AXL_OK,
               "scsi health: decode -> AXL_OK");
    test_check(h.healthy && h.asc == 0 && h.ascq == 0,
               "scsi health: no exception -> healthy");
    test_check(h.temperature_c == 35,
               "scsi health: IE temperature byte");

    /* Predicted failure: asc 0x5D. */
    n = build_ie(b, 0x5D, 0x10, 0x00, false);
    axl_memset(&h, 0, sizeof(h));
    test_check(axl_scsi_decode_health(b, n, &h) == AXL_OK
                   && !h.healthy && h.asc == 0x5D && h.ascq == 0x10,
               "scsi health: asc 0x5D -> not healthy");
    test_check(h.temperature_c == INT32_MIN,
               "scsi health: no temp byte -> INT32_MIN");

    test_check(axl_scsi_decode_health(b, 9, &h) == AXL_ERR,
               "scsi health: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Temperature log page (0x0D)
// ---------------------------------------------------------------------------

static void
test_temperature(void)
{
    uint8_t b[10] = { 0 };
    b[0] = 0x0D;             /* page code */
    put_be(b, 2, 6, 2);      /* page length */
    put_be(b, 4, 0x0000, 2); /* parameter code (Temperature) */
    b[6] = 0x00;             /* control */
    b[7] = 0x02;             /* param length */
    b[8] = 0x00;             /* reserved */
    b[9] = 0x28;             /* temperature 40 C */

    int32_t t = 0;
    test_check(axl_scsi_decode_temperature(b, sizeof(b), &t) == AXL_OK && t == 40,
               "scsi temperature: 0x0D current temp = 40 C");

    b[9] = 0xFF;             /* not available */
    test_check(axl_scsi_decode_temperature(b, sizeof(b), &t) == AXL_OK
                   && t == INT32_MIN,
               "scsi temperature: 0xFF -> INT32_MIN");

    test_check(axl_scsi_decode_temperature(b, 9, &t) == AXL_ERR,
               "scsi temperature: short buffer -> AXL_ERR");
}

static int
test_scsi_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlScsi");

    test_inquiry();
    test_serial();
    test_capacity();
    test_health();
    test_temperature();

    return test_print_results();
}

AXL_APP(test_scsi_main)
