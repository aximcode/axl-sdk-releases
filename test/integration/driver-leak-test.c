/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * driver-leak-test.c — exercises axl_driver_load + axl_driver_set_load_options
 * + axl_driver_unload and asserts the load-options copy is freed at unload.
 *
 * The leak shape: axl_driver_set_load_options allocates `axl_malloc(size)`
 * and hands the pointer to the firmware via `LoadedImage->LoadOptions`.
 * The firmware retains the pointer; AXL is responsible for freeing it on
 * unload.
 *
 * Test sequence:
 *   1. snapshot mem stats (baseline)
 *   2. basic cycle: load + set_load_options + unload, count must match
 *   3. re-set: load + set_options(A) + set_options(B) + unload — old
 *      copy must be freed when the second set_options replaces it
 *   4. out-of-slots: fill the 16-slot table by loading driver.efi 17×,
 *      asserting the 17th set_load_options returns AXL_ERR (and that
 *      the AXL_ERR path doesn't itself leak the would-be copy)
 *
 * Driver.efi's own DriverEntry is NOT invoked (no axl_driver_start), so
 * driver-side allocations don't muddy the comparison. Only the AXL-side
 * load-options leak is what this test checks.
 */

#include <axl.h>

#define LOAD_OPTIONS_TABLE_SIZE  16  /* must mirror src/util/axl-driver.c */

static int g_passes = 0;
static int g_fails  = 0;

static void
pass(const char *msg)
{
    axl_printf("PASS: %s\n", msg);
    g_passes++;
}

static void
fail(const char *msg)
{
    axl_printf("FAIL: %s\n", msg);
    g_fails++;
}

/* Phase 1: basic load + set_options + unload — must show zero leak. */
static void
phase_basic(AxlMemStats *baseline)
{
    AxlDriverHandle drv = NULL;
    AxlMemStats     after;

    if (axl_driver_load("fs0:\\driver.efi", &drv) != AXL_OK || drv == NULL) {
        fail("phase_basic: axl_driver_load");
        return;
    }
    pass("phase_basic: axl_driver_load");

    if (axl_driver_set_load_options(drv, "x", 1) != AXL_OK) {
        fail("phase_basic: axl_driver_set_load_options");
    } else {
        pass("phase_basic: axl_driver_set_load_options");
    }

    if (axl_driver_unload(drv) != AXL_OK) {
        fail("phase_basic: axl_driver_unload");
        return;
    }
    pass("phase_basic: axl_driver_unload");

    axl_mem_get_stats(&after);
    if (after.count == baseline->count) {
        pass("phase_basic: no leak");
    } else {
        axl_printf("FAIL: phase_basic: leak (delta=%lld)\n",
                   (long long)((intptr_t)after.count - (intptr_t)baseline->count));
        axl_mem_dump_leaks();
        g_fails++;
    }
}

/* Phase 2: re-set on same handle. Second set_options must free the
   first copy; net leak across the cycle is still zero. */
static void
phase_reset(AxlMemStats *baseline)
{
    AxlDriverHandle drv = NULL;
    AxlMemStats     after;
    char            big[128];
    axl_memset(big, 'A', sizeof(big));

    if (axl_driver_load("fs0:\\driver.efi", &drv) != AXL_OK || drv == NULL) {
        fail("phase_reset: axl_driver_load");
        return;
    }

    /* First set: 1-byte copy. */
    if (axl_driver_set_load_options(drv, "x", 1) != AXL_OK) {
        fail("phase_reset: first set_load_options");
        axl_driver_unload(drv);
        return;
    }

    /* Second set: 128-byte copy. The 1-byte copy must be freed by the
       set path before installing the new one — otherwise unload only
       releases the second, leaving the first leaked. */
    if (axl_driver_set_load_options(drv, big, sizeof(big)) != AXL_OK) {
        fail("phase_reset: second set_load_options");
        axl_driver_unload(drv);
        return;
    }
    pass("phase_reset: re-set succeeded");

    if (axl_driver_unload(drv) != AXL_OK) {
        fail("phase_reset: axl_driver_unload");
        return;
    }

    axl_mem_get_stats(&after);
    if (after.count == baseline->count) {
        pass("phase_reset: no leak after re-set+unload");
    } else {
        axl_printf("FAIL: phase_reset: leak (delta=%lld bytes_delta=%lld)\n",
                   (long long)((intptr_t)after.count - (intptr_t)baseline->count),
                   (long long)((intptr_t)after.bytes - (intptr_t)baseline->bytes));
        axl_mem_dump_leaks();
        g_fails++;
    }
}

/* Phase 3: out-of-slots. Load driver.efi LOAD_OPTIONS_TABLE_SIZE+1 times,
   set options on each. The final set must return AXL_ERR (table full)
   AND must NOT itself leak the would-be copy. */
static void
phase_table_full(AxlMemStats *baseline)
{
    AxlDriverHandle drvs[LOAD_OPTIONS_TABLE_SIZE + 1] = { NULL };
    int             i;
    int             loaded = 0;
    int             rc;

    /* Load + set_options on the first LOAD_OPTIONS_TABLE_SIZE — all
       must succeed. */
    for (i = 0; i < LOAD_OPTIONS_TABLE_SIZE; i++) {
        if (axl_driver_load("fs0:\\driver.efi", &drvs[i]) != AXL_OK
            || drvs[i] == NULL) {
            axl_printf("FAIL: phase_table_full: load #%d\n", i);
            g_fails++;
            goto cleanup;
        }
        loaded++;
        if (axl_driver_set_load_options(drvs[i], "x", 1) != AXL_OK) {
            axl_printf("FAIL: phase_table_full: set_options #%d (table not yet full)\n", i);
            g_fails++;
            goto cleanup;
        }
    }
    pass("phase_table_full: filled table to capacity");

    /* Load #17: set_options must fail with AXL_ERR. */
    if (axl_driver_load("fs0:\\driver.efi", &drvs[LOAD_OPTIONS_TABLE_SIZE]) != AXL_OK
        || drvs[LOAD_OPTIONS_TABLE_SIZE] == NULL) {
        fail("phase_table_full: load #17");
        goto cleanup;
    }
    loaded++;

    rc = axl_driver_set_load_options(drvs[LOAD_OPTIONS_TABLE_SIZE], "x", 1);
    if (rc == AXL_ERR) {
        pass("phase_table_full: 17th set_load_options returns AXL_ERR");
    } else {
        axl_printf("FAIL: phase_table_full: 17th set_load_options "
                   "expected AXL_ERR, got %d\n", rc);
        g_fails++;
    }

cleanup:
    /* Unload everything we loaded. */
    for (i = 0; i < loaded; i++) {
        if (drvs[i] != NULL) {
            axl_driver_unload(drvs[i]);
        }
    }

    AxlMemStats after;
    axl_mem_get_stats(&after);
    if (after.count == baseline->count) {
        pass("phase_table_full: no leak (table-full path frees would-be copy)");
    } else {
        axl_printf("FAIL: phase_table_full: leak (delta=%lld)\n",
                   (long long)((intptr_t)after.count - (intptr_t)baseline->count));
        axl_mem_dump_leaks();
        g_fails++;
    }
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AxlMemStats baseline;

    axl_printf("driver-leak-test: start\n");

    axl_mem_get_stats(&baseline);

    phase_basic(&baseline);
    phase_reset(&baseline);
    phase_table_full(&baseline);

    axl_printf("driver-leak-test: %d passed, %d failed\n", g_passes, g_fails);
    return g_fails == 0 ? 0 : 1;
}
