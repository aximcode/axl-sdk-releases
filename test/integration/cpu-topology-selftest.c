/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * cpu-topology-selftest.c — exercise axl_cpu_topology() against a
 * chosen QEMU -smp layout.
 *
 * The unit suite boots a single vCPU, where EFI_MP_SERVICES_PROTOCOL
 * is absent or reports one processor — so the count-and-fill path,
 * truncation, and per-CPU location/status can only be exercised with
 * an explicit multi-processor topology. This app queries the counts,
 * fills a buffer, prints a machine-readable inventory, and checks the
 * contract invariants (counts decoupled from fill, dense index-keyed
 * entries, exactly one BSP, truncation honored).
 *
 * Driven by test/integration/test-cpu-topology-qemu.sh under several
 * -smp layouts; the script asserts the layout-specific exact counts by
 * grepping the "TOPO:" / "CPU[" lines.
 *
 * Final line: "CPU-TOPOLOGY-SELFTEST: <N> passed, <M> failed".
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

    /* 1) Counts-only query (out == NULL). */
    size_t total = 0, enabled = 0, out_n = 99;
    int rc = axl_cpu_topology(&total, &enabled, NULL, 0, &out_n);
    check(rc == AXL_OK, "counts-only query succeeds");
    check(out_n == 0, "out == NULL leaves out_n == 0");
    check(total >= 1, "total >= 1");
    check(enabled >= 1 && enabled <= total, "1 <= enabled <= total");

    /* Machine-readable summary for the driver script. */
    axl_printf("TOPO: total=%zu enabled=%zu\n", total, enabled);

    /* 2) Full fill into a generously-sized buffer (cap >= total). */
    AxlCpuProcessor procs[64];
    size_t cap = sizeof(procs) / sizeof(procs[0]);
    size_t want = (total <= cap) ? total : cap;
    size_t total2 = 0, enabled2 = 0, fill_n = 99;
    rc = axl_cpu_topology(&total2, &enabled2, procs, cap, &fill_n);
    check(rc == AXL_OK, "buffered query succeeds");
    check(total2 == total, "total stable across calls");
    check(enabled2 == enabled, "enabled stable across calls");
    check(fill_n == want, "out_n == min(total, cap)");

    /* Inventory + invariant tally over the written entries. */
    size_t bsp_count = 0, enabled_seen = 0;
    for (size_t i = 0; i < fill_n; i++) {
        AxlCpuProcessor *p = &procs[i];
        axl_printf("CPU[%zu]: pkg=%u core=%u thread=%u bsp=%d enabled=%d healthy=%d\n",
                   i, p->package, p->core, p->thread,
                   p->bsp, p->enabled, p->healthy);
        if (p->bsp) {
            bsp_count++;
        }
        if (p->enabled) {
            enabled_seen++;
        }
    }

    if (fill_n > 0) {
        check(bsp_count == 1, "exactly one BSP among written entries");
        check(enabled_seen == enabled, "enabled flags agree with enabled count");
    }

    /* 3) Truncation: a buffer smaller than total reports the true
       counts but writes only out_cap entries. Only meaningful with
       more than one processor. */
    if (total >= 2) {
        AxlCpuProcessor one[1];
        size_t t = 0, e = 0, n = 99;
        rc = axl_cpu_topology(&t, &e, one, 1, &n);
        check(rc == AXL_OK, "truncated query succeeds");
        check(t == total, "truncation keeps total accurate");
        check(n == 1, "truncation writes exactly out_cap entries");
    } else {
        /* Balance: single-processor layouts can't exercise truncation. */
        check(total == 1, "single-processor layout (truncation n/a)");
    }

    axl_printf("CPU-TOPOLOGY-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
