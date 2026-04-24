/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-atexit-internal.h
    Internal hooks used by src/runtime/axl-runtime.c to drive the
    atexit registry. Not a public header.
**/

#ifndef AXL_ATEXIT_INTERNAL_H
#define AXL_ATEXIT_INTERNAL_H

/** Called once by _axl_init before any user code runs. */
void _axl_atexit_init(void);

/** Called once by _axl_cleanup, before the resource-registry sweep.
 *  Walks live callbacks in descending-seq (LIFO) order. */
void _axl_atexit_run_all(void);

#endif /* AXL_ATEXIT_INTERNAL_H */
