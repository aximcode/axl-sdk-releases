/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * bss-probe.c — measure + validate the build's handling of a large
 * zero-initialized static array (.bss).
 *
 * Two jobs:
 *   1. Size: an 8 MiB static array makes the .efi's file size a direct
 *      readout of whether .bss is materialized as literal zero bytes in
 *      the file (the old .data-merge) or carried as an uninitialized PE
 *      section (no file bytes).
 *   2. Correctness: a real UEFI image must see this array zero-initialized
 *      and writable at runtime. The crt0 _start ZEROES [_bss, _bss_end) before
 *      any C runs (the loader is not trusted to zero-fill the NOBITS section);
 *      the loader still maps the span RW. This probe asserts both: reads zero
 *      across the span (the crt0 clear) and writes a pattern + reads it back
 *      (the loader's mapping). NOTE: on firmware that DOES hand back zeroed
 *      pages, this passes even without the crt0 clear — so it is NOT the
 *      regression guard for the clear; `make check-bss-clear` is (it asserts
 *      _start zeroes [_bss, _bss_end) by disassembly, firmware-independent).
 *
 * Emits "BSS-PROBE: <N> passed, <M> failed" as the final line.
 */

#include <axl.h>

#define BSS_BYTES  (8u * 1024u * 1024u)   /* 8 MiB */

static uint8_t  g_big[BSS_BYTES];          /* the .bss subject */
static uint32_t g_small_zero;              /* a scalar bss global too */

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    axl_printf("BSS-PROBE: %u-byte static array\n", BSS_BYTES);

    check(g_small_zero == 0, "scalar bss global is zero-initialized");

    /* Zero across the whole span (sample every 64 KiB + the ends). The
       loader must have zero-filled the section. */
    bool all_zero = (g_big[0] == 0) && (g_big[BSS_BYTES - 1] == 0);
    for (uint32_t i = 0; i < BSS_BYTES; i += 65536) {
        if (g_big[i] != 0) {
            all_zero = false;
            break;
        }
    }
    check(all_zero, "8 MiB static array is zero-initialized across its span");

    /* Writable + reads back (proves the pages are real, mapped RW). */
    for (uint32_t i = 0; i < BSS_BYTES; i += 65536) {
        g_big[i] = (uint8_t)(i * 7u + 1u);
    }
    g_big[BSS_BYTES - 1] = 0xAB;
    bool readback = (g_big[BSS_BYTES - 1] == 0xAB);
    for (uint32_t i = 0; i < BSS_BYTES; i += 65536) {
        if (g_big[i] != (uint8_t)(i * 7u + 1u)) {
            readback = false;
            break;
        }
    }
    check(readback, "8 MiB static array is writable and reads back");

    axl_printf("BSS-PROBE: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
