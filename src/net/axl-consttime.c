/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-consttime.c
    Constant-time buffer comparison for secret-dependent data.
    Always compiled (no AXL_TLS dependency).
**/

#include <stdint.h>
#include <axl/axl-crypto.h>

bool
axl_consttime_equal(
    const void *a,
    const void *b,
    size_t      len
    )
{
    if (len == 0) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    /* OR-accumulate every byte difference; no early exit, no data-dependent
       branch — running time depends only on len. */
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return acc == 0;
}
