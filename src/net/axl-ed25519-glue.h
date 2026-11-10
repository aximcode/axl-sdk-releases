/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ed25519-glue.h
    What the Ed25519 provider needs to know about the externals it
    supplies to the vendored ref10 code (axl-ed25519-glue.c), because
    ref10 itself cannot tell it.

    ref10 calls crypto_hash_sha512 five times and randombytes once and
    ignores every result. So a failure inside either reaches ref10 as a
    digest or a seed of ALL ZEROS, and that is not a safe value to carry
    on with: a zeroed nonce hash makes a signature that verifies AND
    discloses the private scalar, a zeroed challenge hash makes
    R = encode(B), S = 1 verify under every public key, and a zeroed seed
    is a valid key whose private half is public. The glue therefore
    counts every failure, and the provider reads the count before and
    after each call into ref10: if it moved, the operation is refused,
    whatever ref10 returned.

    Internal: not installed, not public API. The two hooks below exist
    for test/unit/axl-test-ed25519.c, because the shipped configuration
    has no way to make the hash fail (the glue #errors on the mbedTLS
    options that would give it one) -- a check that can never fire is
    one nobody can show works.
**/

#ifndef AXL_ED25519_GLUE_H
#define AXL_ED25519_GLUE_H

#include <stdint.h>

/** Failures the glue has seen since boot: every crypto_hash_sha512 that
    could not hash, and every randombytes that could not draw. Only ever
    increases; compare two readings, never a reading with a constant. */
uint32_t _axl_ed25519_glue_failures(void);

/** TEST HOOK. Make the @a n-th crypto_hash_sha512 call from now fail as
    a real failure would (zeroed digest, counted, logged); 0 disarms.
    Mirrors axl_mem_fail_next_alloc(). */
void _axl_ed25519_glue_fail_hash(uint32_t n);

/** TEST HOOK. How many times randombytes has been called since boot.
    AXL's own keygen draws its seed itself and must never reach it; the
    unit test pins that with this count. */
uint32_t _axl_ed25519_glue_randombytes_calls(void);

#endif /* AXL_ED25519_GLUE_H */
