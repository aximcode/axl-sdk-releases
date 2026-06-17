/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-nvme.c
    Unit tests for the AxlNvme raw-buffer decoders.

    The decoders (axl_nvme_decode_*) are pure functions over the
    little-endian NVMe data structures, so they are tested here against
    spec-faithful buffers built in-test — no NVMe device and no QEMU
    needed. The device-facing readers (axl_nvme_smart over the pass-thru
    protocol) are exercised by test/integration/test-nvme-qemu.sh against
    an emulated `-device nvme`.
**/

#include <axl.h>
#include "axl-test.h"

// ---------------------------------------------------------------------------
// Little-endian writers for building spec-faithful buffers.
// ---------------------------------------------------------------------------

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

static void
put_le64(uint8_t *b, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        b[off + (size_t)i] = (uint8_t)(v >> (8 * i));
    }
}

/* Copy an ASCII field, space-padded to @p len (the NVMe identity-field
   convention: trailing spaces, not NUL). */
static void
put_ascii_padded(uint8_t *b, size_t off, size_t len, const char *s)
{
    size_t n = axl_strlen(s);
    for (size_t i = 0; i < len; i++) {
        b[off + i] = (uint8_t)(i < n ? s[i] : ' ');
    }
}

// ---------------------------------------------------------------------------
// Identify Controller
// ---------------------------------------------------------------------------

static void
test_identify_controller(void)
{
    uint8_t id[4096] = { 0 };
    put_le16(id, 0, 0x144D);                          /* VID */
    put_le16(id, 2, 0x144D);                          /* SSVID */
    put_ascii_padded(id, 4, 20, "SN0123456789");      /* SN */
    put_ascii_padded(id, 24, 40, "AXL Test NVMe 1TB"); /* MN */
    put_ascii_padded(id, 64, 8, "FW1.0");             /* FR */
    put_le32(id, 80, 0x00010400);                     /* VER 1.4.0 */
    put_le32(id, 516, 1);                             /* NN */

    AxlNvmeController c = { 0 };
    test_check(axl_nvme_decode_identify_controller(id, sizeof(id), &c) == AXL_OK,
               "nvme id-ctrl: decode -> AXL_OK");
    test_check(c.pci_vid == 0x144D && c.pci_ssvid == 0x144D,
               "nvme id-ctrl: VID/SSVID");
    test_check(axl_strcmp(c.serial, "SN0123456789") == 0,
               "nvme id-ctrl: serial trimmed");
    test_check(axl_strcmp(c.model, "AXL Test NVMe 1TB") == 0,
               "nvme id-ctrl: model trimmed");
    test_check(axl_strcmp(c.firmware, "FW1.0") == 0,
               "nvme id-ctrl: firmware trimmed");
    test_check(c.nvme_version == 0x00010400, "nvme id-ctrl: version BCD");
    test_check(c.namespace_count == 1, "nvme id-ctrl: namespace count");

    /* A model that fills the whole 40-byte field (no trailing spaces)
       must NUL-terminate at the buffer cap without truncating content or
       overrunning model[41]. */
    put_ascii_padded(id, 24, 40, "0123456789012345678901234567890123456789");
    test_check(axl_nvme_decode_identify_controller(id, sizeof(id), &c) == AXL_OK
                   && axl_strlen(c.model) == 40
                   && axl_strcmp(c.model,
                          "0123456789012345678901234567890123456789") == 0,
               "nvme id-ctrl: full-width 40-char model trims/terminates");

    /* Short buffer + NULL args fail closed. */
    test_check(axl_nvme_decode_identify_controller(id, 4095, &c) == AXL_ERR,
               "nvme id-ctrl: short buffer -> AXL_ERR");
    test_check(axl_nvme_decode_identify_controller(NULL, 4096, &c) == AXL_ERR,
               "nvme id-ctrl: NULL buffer -> AXL_ERR");
    test_check(axl_nvme_decode_identify_controller(id, sizeof(id), NULL) == AXL_ERR,
               "nvme id-ctrl: NULL out -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Identify Namespace
// ---------------------------------------------------------------------------

static void
test_identify_namespace(void)
{
    uint8_t id[4096] = { 0 };
    put_le64(id, 0, 0x1000000);   /* NSZE = 16,777,216 logical blocks */
    id[25] = 0;                   /* NLBAF (0-based: one format) */
    id[26] = 0;                   /* FLBAS: active LBA format index 0 */
    /* LBA Format 0 at offset 128; LBADS (log2 block size) at entry+2. */
    id[128 + 0 * 4 + 2] = 12;     /* LBADS 12 -> 4096-byte blocks */

    AxlNvmeNamespace ns = { 0 };
    test_check(axl_nvme_decode_identify_namespace(id, sizeof(id), 1, &ns) == AXL_OK,
               "nvme id-ns: decode -> AXL_OK");
    test_check(ns.nsid == 1, "nvme id-ns: nsid carried through");
    test_check(ns.size_blocks == 0x1000000, "nvme id-ns: NSZE");
    test_check(ns.block_size == 4096, "nvme id-ns: 4K block from LBADS 12");
    test_check(ns.capacity_bytes == (uint64_t)0x1000000 * 4096,
               "nvme id-ns: capacity = blocks * block_size");

    /* A 512-byte-format namespace via FLBAS selecting a second format. */
    id[26] = 1;                   /* active LBA format index 1 */
    id[128 + 1 * 4 + 2] = 9;      /* LBADS 9 -> 512-byte blocks */
    test_check(axl_nvme_decode_identify_namespace(id, sizeof(id), 7, &ns) == AXL_OK
                   && ns.block_size == 512 && ns.nsid == 7,
               "nvme id-ns: FLBAS selects 512-byte format");

    /* An out-of-range LBADS (the inactive/garbage case) yields block_size 0
       — the signal the tool / mkfixture use to skip inactive namespaces. */
    id[26] = 2;                   /* active LBA format index 2 */
    id[128 + 2 * 4 + 2] = 0;      /* LBADS 0 -> not a valid block size */
    test_check(axl_nvme_decode_identify_namespace(id, sizeof(id), 9, &ns) == AXL_OK
                   && ns.block_size == 0 && ns.capacity_bytes == 0,
               "nvme id-ns: out-of-range LBADS -> block_size 0");

    test_check(axl_nvme_decode_identify_namespace(id, 100, 1, &ns) == AXL_ERR,
               "nvme id-ns: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// SMART / Health
// ---------------------------------------------------------------------------

static void
build_smart(uint8_t log[512], uint8_t crit_warn, uint16_t temp_k)
{
    for (int i = 0; i < 512; i++) {
        log[i] = 0;
    }
    log[0] = crit_warn;            /* Critical Warning */
    put_le16(log, 1, temp_k);      /* Composite Temperature (Kelvin) */
    log[3] = 100;                  /* Available Spare % */
    log[4] = 10;                   /* Available Spare Threshold % */
    log[5] = 5;                    /* Percentage Used */
    put_le64(log, 32, 12345);      /* Data Units Read (low 64 of 128) */
    put_le64(log, 48, 67890);      /* Data Units Written */
    put_le64(log, 64, 1000);       /* Host Read Commands */
    put_le64(log, 80, 2000);       /* Host Write Commands */
    put_le64(log, 112, 42);        /* Power Cycles */
    put_le64(log, 128, 8760);      /* Power On Hours */
    put_le64(log, 144, 3);         /* Unsafe Shutdowns */
    put_le64(log, 160, 0);         /* Media Errors */
    put_le64(log, 176, 7);         /* Error Log Entries */
}

static void
test_smart(void)
{
    uint8_t log[512];
    AxlNvmeSmart s = { 0 };

    /* Healthy: critical warning 0, 313 K = 40 C. */
    build_smart(log, 0x00, 313);
    test_check(axl_nvme_decode_smart(log, sizeof(log), &s) == AXL_OK,
               "nvme smart: decode -> AXL_OK");
    test_check(s.healthy, "nvme smart: healthy when critical_warning == 0");
    test_check(s.temperature_c == 40, "nvme smart: 313 K -> 40 C");
    test_check(s.available_spare == 100 && s.available_spare_threshold == 10,
               "nvme smart: spare + threshold");
    test_check(s.percent_used == 5, "nvme smart: percent_used");
    test_check(s.data_units_read == 12345 && s.data_units_written == 67890,
               "nvme smart: data units");
    test_check(s.power_cycles == 42 && s.power_on_hours == 8760
                   && s.unsafe_shutdowns == 3 && s.error_log_entries == 7,
               "nvme smart: counters");
    test_check(s.media_errors == 0, "nvme smart: zero media errors is not 'unknown'");

    /* Critical warnings set: spare-low (bit0) + reliability (bit2). */
    build_smart(log, 0x05, 320);
    test_check(axl_nvme_decode_smart(log, sizeof(log), &s) == AXL_OK
                   && !s.healthy && s.warn_spare_low && s.warn_reliability
                   && !s.warn_temperature && !s.warn_read_only,
               "nvme smart: critical warning bits decode + unhealthy");

    /* Temperature of 0 K is 'not reported' -> sentinel, not -273 C. */
    build_smart(log, 0x00, 0);
    test_check(axl_nvme_decode_smart(log, sizeof(log), &s) == AXL_OK
                   && s.temperature_c == INT32_MIN,
               "nvme smart: 0 K temperature -> INT32_MIN sentinel");

    /* A 128-bit counter whose high half is non-zero saturates. */
    build_smart(log, 0x00, 300);
    put_le64(log, 160 + 8, 1);   /* set high 64 bits of Media Errors */
    test_check(axl_nvme_decode_smart(log, sizeof(log), &s) == AXL_OK
                   && s.media_errors == UINT64_MAX,
               "nvme smart: 128-bit overflow saturates to UINT64_MAX");

    test_check(axl_nvme_decode_smart(log, 511, &s) == AXL_ERR,
               "nvme smart: short buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Device Self-test log
// ---------------------------------------------------------------------------

static void
test_self_test_log(void)
{
    uint8_t log[564] = { 0 };
    AxlNvmeSelfTestResult r = { 0 };

    /* No self-test ever run: most-recent entry result code 0xF. */
    log[0] = 0x00;                 /* no operation in progress */
    log[4] = 0x0F;                 /* newest entry: status nibble = 0xF (unused) */
    test_check(axl_nvme_decode_self_test_log(log, sizeof(log), &r) == AXL_OK
                   && !r.in_progress && !r.passed && r.result_code == 0x0F,
               "nvme self-test: no prior run -> result 0x0F, not passed");

    /* Short test in progress at 50%, a previous run that passed. */
    log[0] = 0x01;                 /* short self-test in progress */
    log[1] = 50;                   /* 50% complete */
    log[4] = 0x10;                 /* newest completed entry: code 0x0, short(0x1) */
    test_check(axl_nvme_decode_self_test_log(log, sizeof(log), &r) == AXL_OK
                   && r.in_progress && r.percent_complete == 50
                   && r.passed && r.result_code == 0,
               "nvme self-test: in progress + prior pass");

    /* Completed, failed self-test (result code 7). */
    log[0] = 0x00;
    log[1] = 0;
    log[4] = 0x27;                 /* extended(0x2) | result 0x7 */
    test_check(axl_nvme_decode_self_test_log(log, sizeof(log), &r) == AXL_OK
                   && !r.in_progress && r.percent_complete == 0
                   && !r.passed && r.result_code == 7,
               "nvme self-test: completed failure -> result 7, not passed");

    test_check(axl_nvme_decode_self_test_log(log, 563, &r) == AXL_ERR,
               "nvme self-test: short buffer -> AXL_ERR");
}

static int
test_nvme_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlNvme");

    test_identify_controller();
    test_identify_namespace();
    test_smart();
    test_self_test_log();

    return test_print_results();
}

AXL_APP(test_nvme_main)
