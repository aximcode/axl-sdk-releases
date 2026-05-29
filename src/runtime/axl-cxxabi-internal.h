/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi-internal.h
    Internal hook used by src/runtime/axl-runtime.c to drive C++
    static-initializer constructors during _axl_init.  Not a public
    header.
**/

#ifndef AXL_CXXABI_INTERNAL_H
#define AXL_CXXABI_INTERNAL_H

/** Walks the binary's .init_array, invoking every registered
 *  global constructor in order.  No-op for binaries with no C++
 *  static initializers (the array bounds are equal). */
void _axl_cxxabi_run_init_array(void);

#endif /* AXL_CXXABI_INTERNAL_H */
