/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-rng.h
    Cryptographic random bytes.

    Thin wrapper over the firmware's `EFI_RNG_PROTOCOL` (UEFI 2.11
    §37.5). The protocol is published by most modern firmware on
    platforms with an entropy source — RDRAND on x86, an SBSA
    TRNG on aa64. If the protocol isn't available, calls return -1
    rather than fall back to a software PRNG; consumers that want
    a deterministic-on-failure path layer their own fallback.

    @code
    uint8_t nonce[16];
    if (axl_rng_bytes(nonce, sizeof(nonce)) == 0) {
        // use nonce
    } else {
        // RNG not available — fall back or fail
    }
    @endcode
**/

#ifndef AXL_RNG_H
#define AXL_RNG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill @p out with @p len cryptographically random bytes.
 *
 * Uses whatever algorithm the firmware advertises as default —
 * SP800-90A on most modern systems. Callers that need a specific
 * algorithm can layer one of EDK2's RngLib variants on top.
 *
 * @return 0 on success, -1 if EFI_RNG_PROTOCOL is unavailable or
 *     the firmware reports an error.
 */
int
axl_rng_bytes(
    void   *out,   ///< destination buffer
    size_t  len    ///< number of bytes to fill
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RNG_H */
