/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mem-internal.h
    AxlMem internals shared with the runtime and the CRT0 stubs.
**/

#ifndef AXL_MEM_INTERNAL_H
#define AXL_MEM_INTERNAL_H

/**
 * Print the process-teardown leak report (debug builds).
 *
 * Same report as axl_mem_dump_leaks(), minus the "(live allocations)"
 * infix — see dump_leaks() in src/mem/axl-mem.c for why the two
 * spellings differ and what depends on it. Call ONLY from a teardown
 * path that runs after atexit callbacks and the tier-1 registry sweep:
 * anything printed here is a real leak, and the QEMU test harness
 * fails the run on it.
 */
void _axl_mem_dump_leaks_at_exit(void);

#ifdef AXL_MEM_DEBUG
/**
 * Release every block the free quarantine holds, running its
 * use-after-free checks on each (debug builds only).
 *
 * For a teardown that prints no leak report: a plain driver's unload
 * (axl_driver_cleanup). The ring pins up to 64 KiB of freed pool, which
 * otherwise outlives the image -- ~2.9 pages per load/unload cycle of a
 * C++ driver, measured. The two dump functions drain it themselves.
 */
void _axl_mem_quarantine_drain(void);
#endif

#endif /* AXL_MEM_INTERNAL_H */
