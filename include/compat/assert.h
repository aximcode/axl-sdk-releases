/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Minimal assert.h for freestanding UEFI */
#ifndef AXL_COMPAT_ASSERT_H
#define AXL_COMPAT_ASSERT_H


#ifdef __cplusplus
extern "C" {
#endif

#define assert(expr) ((void)0)

#ifdef __cplusplus
}
#endif

#endif
