/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * gfx-mode-selftest.c — GOP display-mode enumerate / switch round-trip.
 *
 * Exercises axl_gfx_mode_count / query_mode / current_mode / find_mode /
 * set_mode against a REAL GOP — the one OVMF exposes when QEMU is launched
 * with a virtual GPU.  The unit suite runs `-nographic` (no GOP), so it can
 * only assert the NULL-arg + no-GOP contract; this app fills the gap:
 *   - enumerates every mode and prints its geometry,
 *   - checks the current mode index/geometry agrees with axl_gfx_get_info,
 *   - picks a DIFFERENT-resolution mode, switches to it, and confirms
 *     get_info + current_mode report the new geometry,
 *   - restores the original mode and confirms it came back.
 *
 * Run via: scripts/run-qemu.sh --gpu gfx-mode-selftest.efi
 * Driven by: test/integration/test-gfx-mode-qemu.sh
 *
 * Emits "MODE-SELFTEST: <N> passed, <M> failed" as the final line so the
 * harness can scrape a verdict.
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

    if (!axl_gfx_available()) {
        /* No GOP — the harness only runs us under --gpu, so this means a
           setup problem.  Emit a recognizable line and a nonzero verdict. */
        axl_printf("MODE-SELFTEST: no GOP available (run under --gpu)\n");
        axl_printf("MODE-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    uint32_t n = axl_gfx_mode_count();
    axl_printf("MODE-SELFTEST: %u modes enumerated\n", n);
    check(n > 0, "mode_count > 0 under --gpu");

    /* Enumerate: every mode must query OK with a positive geometry and a
       stride no smaller than its width. */
    bool all_ok = (n > 0);
    for (uint32_t i = 0; i < n; i++) {
        AxlGfxMode m;
        if (axl_gfx_query_mode(i, &m) != AXL_OK
            || m.index != i || m.width == 0 || m.height == 0
            || m.stride < m.width) {
            all_ok = false;
            axl_printf("  mode %u: QUERY FAILED / bad geometry\n", i);
            continue;
        }
        axl_printf("  mode %u: %ux%u stride=%u\n", i, m.width, m.height, m.stride);
    }
    check(all_ok, "every mode queries OK with sane geometry");

    /* Out-of-range index is rejected. */
    AxlGfxMode oob;
    check(axl_gfx_query_mode(n, &oob) == AXL_ERR,
          "query_mode(count) -> AXL_ERR (out of range)");

    /* max_mode: enumerable, and no smaller (by area) than ANY other mode. */
    AxlGfxMode mx;
    bool max_ok = (axl_gfx_max_mode(&mx) == AXL_OK) && (mx.index < n);
    if (max_ok) {
        uint64_t max_area = (uint64_t)mx.width * mx.height;
        for (uint32_t i = 0; i < n; i++) {
            AxlGfxMode m;
            if (axl_gfx_query_mode(i, &m) == AXL_OK
                && (uint64_t)m.width * m.height > max_area) {
                max_ok = false;
                break;
            }
        }
        axl_printf("Max mode: %u = %ux%u\n", mx.index, mx.width, mx.height);
    }
    check(max_ok, "max_mode is the largest-area mode");

    /* Current mode: in range, and its geometry matches get_info(). */
    uint32_t cur = 0;
    check(axl_gfx_current_mode(&cur) == AXL_OK && cur < n,
          "current_mode in [0, count)");

    AxlGfxInfo info;
    axl_gfx_get_info(&info);
    AxlGfxMode cm;
    check(axl_gfx_query_mode(cur, &cm) == AXL_OK
          && cm.width == info.width && cm.height == info.height,
          "current mode geometry matches get_info()");
    axl_printf("Display: current mode %u = %ux%u\n", cur, info.width, info.height);

    /* find_mode round-trips the current resolution back to an index whose
       geometry matches (it may not be `cur` if duplicate resolutions exist,
       but it must resolve to the same WxH). */
    uint32_t found = 0;
    AxlGfxMode fm;
    check(axl_gfx_find_mode(info.width, info.height, &found) == AXL_OK
          && axl_gfx_query_mode(found, &fm) == AXL_OK
          && fm.width == info.width && fm.height == info.height,
          "find_mode(current WxH) resolves to a matching mode");
    check(axl_gfx_find_mode(1, 1, &found) == AXL_ERR,
          "find_mode(1x1) -> AXL_ERR (no such mode)");

    /* Pick a mode with a DIFFERENT resolution than current, switch to it,
       and confirm the switch took effect. */
    uint32_t target = n;  /* sentinel = none found */
    for (uint32_t i = 0; i < n; i++) {
        AxlGfxMode m;
        if (axl_gfx_query_mode(i, &m) == AXL_OK
            && (m.width != info.width || m.height != info.height)) {
            target = i;
            break;
        }
    }

    if (target < n) {
        AxlGfxMode tm;
        axl_gfx_query_mode(target, &tm);
        axl_printf("Switching: mode %u (%ux%u) -> mode %u (%ux%u)\n",
                   cur, info.width, info.height, target, tm.width, tm.height);

        check(axl_gfx_set_mode(target) == AXL_OK, "set_mode(target) -> AXL_OK");

        AxlGfxInfo after;
        axl_gfx_get_info(&after);
        check(after.width == tm.width && after.height == tm.height,
              "get_info() reflects the new resolution after set_mode");

        uint32_t now = 0;
        check(axl_gfx_current_mode(&now) == AXL_OK && now == target,
              "current_mode() == target after set_mode");

        /* Restore the original mode. */
        check(axl_gfx_set_mode(cur) == AXL_OK, "set_mode(original) -> AXL_OK");
        AxlGfxInfo restored;
        axl_gfx_get_info(&restored);
        check(restored.width == info.width && restored.height == info.height,
              "get_info() restored to the original resolution");
    } else {
        axl_printf("MODE-SELFTEST: only one resolution exposed, skipping switch\n");
    }

    /* Out-of-range set is rejected (and changes nothing). */
    check(axl_gfx_set_mode(n) == AXL_ERR, "set_mode(count) -> AXL_ERR");

    axl_printf("MODE-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
