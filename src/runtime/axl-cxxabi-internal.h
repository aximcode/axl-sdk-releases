/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi-internal.h
    Internal hook that drives C++ static-initializer constructors.
    Not a public header.

    TWO callers, one per image kind: src/runtime/axl-runtime.c
    (_axl_init) for an app and src/util/axl-driver.c
    (axl_driver_init) for a driver. Only the first existed until
    2026-08-18, so a driver image linked constructors that nothing
    ever ran.
**/

#ifndef AXL_CXXABI_INTERNAL_H
#define AXL_CXXABI_INTERNAL_H

/** Walks the binary's .init_array, invoking every registered
 *  global constructor in order.  No-op for binaries with no C++
 *  static initializers (the array bounds are equal). */
void _axl_cxxabi_run_init_array(void);

#endif /* AXL_CXXABI_INTERNAL_H */
