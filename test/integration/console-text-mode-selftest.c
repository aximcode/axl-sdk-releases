/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * console-text-mode-selftest.c — text-console mode enumerate / switch
 * round-trip.
 *
 * Exercises axl_console_text_mode_count / query_mode / current_mode /
 * find_mode / max_mode / set_mode against a console that has MORE THAN ONE
 * text mode.  The -nographic unit suite has a serial console with (usually)
 * a single 80x25 mode, so it can only assert the enumerate contract; this
 * app runs under a virtual GPU (scripts/run-qemu.sh --gpu), where OVMF's
 * graphics console publishes several text modes (80x25, 100x31, ...), and:
 *   - enumerates every mode and prints its geometry,
 *   - checks the current mode index agrees with the enumeration,
 *   - picks a DIFFERENT-geometry mode, switches to it, and confirms
 *     current_mode reports the new mode,
 *   - restores the original mode and confirms it came back.
 *
 * Run via: scripts/run-qemu.sh --gpu console-text-mode-selftest.efi
 * Driven by: test/integration/test-console-text-mode-qemu.sh
 *
 * Emits "TEXT-MODE-SELFTEST: <N> passed, <M> failed" as the final line so
 * the harness can scrape a verdict.
 */

#include <axl.h>

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

    uint32_t n = axl_console_text_mode_count();
    axl_printf("TEXT-MODE-SELFTEST: %u modes enumerated\n", n);
    check(n > 0, "mode_count > 0");

    /* Enumerate. Per the contract an in-range mode may legally fail
       QueryMode — an optional mode the firmware doesn't support; OVMF's
       graphics console does exactly this for one of its modes — so a
       QueryMode failure is "skip", not "fail". Mode 0 (80x25, the only one
       UEFI guarantees) must always query, and every mode that DOES query
       must report its own index and positive geometry. */
    bool mode0_ok = false;
    bool sane     = true;
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        if (axl_console_text_query_mode(i, &m) != AXL_OK) {
            axl_printf("  mode %u: (unsupported — skipped)\n", i);
            continue;
        }
        if (m.index != i || m.columns == 0 || m.rows == 0) {
            sane = false;
            axl_printf("  mode %u: BAD %ux%u idx=%u\n",
                       i, m.columns, m.rows, m.index);
            continue;
        }
        if (i == 0) {
            mode0_ok = true;
        }
        axl_printf("  mode %u: %ux%u\n", i, m.columns, m.rows);
    }
    check(mode0_ok, "mode 0 queries OK (the guaranteed 80x25)");
    check(sane, "every queryable mode reports its index + positive geometry");

    /* Out-of-range index is rejected. */
    AxlConsoleTextMode oob;
    check(axl_console_text_query_mode(n, &oob) == AXL_ERR,
          "query_mode(count) -> AXL_ERR (out of range)");

    /* max_mode: enumerable, and no smaller (by area) than ANY other mode. */
    AxlConsoleTextMode mx;
    bool max_ok = (axl_console_text_max_mode(&mx) == AXL_OK) && (mx.index < n);
    if (max_ok) {
        uint64_t max_area = (uint64_t)mx.columns * mx.rows;
        for (uint32_t i = 0; i < n; i++) {
            AxlConsoleTextMode m;
            if (axl_console_text_query_mode(i, &m) == AXL_OK
                && (uint64_t)m.columns * m.rows > max_area) {
                max_ok = false;
                break;
            }
        }
        axl_printf("Max mode: %u = %ux%u\n", mx.index, mx.columns, mx.rows);
    }
    check(max_ok, "max_mode is the largest-area mode");

    /* Current mode: in range. Its geometry round-trips through find_mode. */
    uint32_t cur = 0;
    check(axl_console_text_current_mode(&cur) == AXL_OK && cur < n,
          "current_mode in [0, count)");

    /* Zero-init: query_mode leaves out untouched on AXL_ERR (the current
       mode could legally be an unqueryable optional mode), and cm.columns/
       .rows are read below in the target-selection loop regardless. */
    AxlConsoleTextMode cm = { 0 };
    bool cur_ok = (axl_console_text_query_mode(cur, &cm) == AXL_OK);
    if (cur_ok) {
        axl_printf("Console: current mode %u = %ux%u\n",
                   cur, cm.columns, cm.rows);
        uint32_t found = 0;
        AxlConsoleTextMode fm;
        cur_ok = (axl_console_text_find_mode(cm.columns, cm.rows, &found)
                      == AXL_OK)
                 && (axl_console_text_query_mode(found, &fm) == AXL_OK)
                 && fm.columns == cm.columns && fm.rows == cm.rows;
    }
    check(cur_ok, "find_mode(current CxR) resolves to a matching mode");

    uint32_t miss = 0;
    check(axl_console_text_find_mode(1, 1, &miss) == AXL_ERR,
          "find_mode(1x1) -> AXL_ERR (no such mode)");

    /* Pick a mode with a DIFFERENT geometry than current, switch to it, and
       confirm the switch took effect; then restore the original. */
    uint32_t target = n;  /* sentinel = none found */
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        if (axl_console_text_query_mode(i, &m) == AXL_OK
            && (m.columns != cm.columns || m.rows != cm.rows)) {
            target = i;
            break;
        }
    }

    if (target < n) {
        AxlConsoleTextMode tm = { 0 };
        check(axl_console_text_query_mode(target, &tm) == AXL_OK,
              "query_mode(target) -> AXL_OK before switch");
        axl_printf("Switching: mode %u (%ux%u) -> mode %u (%ux%u)\n",
                   cur, cm.columns, cm.rows, target, tm.columns, tm.rows);

        check(axl_console_text_set_mode(target) == AXL_OK,
              "set_mode(target) -> AXL_OK");

        uint32_t now = 0;
        check(axl_console_text_current_mode(&now) == AXL_OK && now == target,
              "current_mode() == target after set_mode");

        /* Restore the original mode. */
        check(axl_console_text_set_mode(cur) == AXL_OK,
              "set_mode(original) -> AXL_OK");
        uint32_t back = 0;
        check(axl_console_text_current_mode(&back) == AXL_OK && back == cur,
              "current_mode() == original after restore");
    } else {
        axl_printf("TEXT-MODE-SELFTEST: only one geometry exposed, "
                   "skipping switch\n");
    }

    /* Out-of-range set is rejected (and changes nothing). */
    check(axl_console_text_set_mode(n) == AXL_ERR, "set_mode(count) -> AXL_ERR");

    axl_printf("TEXT-MODE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
