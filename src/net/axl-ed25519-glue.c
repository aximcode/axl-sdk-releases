/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ed25519-glue.c
    The three externals the vendored ref10 calls but does not define.

    deps/ed25519-ref10/ed25519.c is byte-identical to OpenSSH's, which
    means it expects OpenSSH's environment: a SHA-512, a scrub, and a
    random source. Supplying them here rather than patching the vendored
    file is what keeps a resync a copy rather than a merge -- see the
    design spec's §6b for what a local patch to that file would cost.

    ref10 ignores the result of the hash at all five call sites, and
    randombytes is `void` upstream, so a failure in either reaches ref10
    as a digest or a seed of ALL ZEROS -- and there is no safe value to
    hand it instead. Measured against the previous glue: a zeroed NONCE
    hash yields R = identity, S = h*a, a signature that VERIFIES and from
    which anyone computes the private scalar a = S * h^-1 mod L; a zeroed
    CHALLENGE hash (h = 0) makes R = encode(B), S = 1 verify under every
    public key; a zeroed seed is a valid key whose private half is
    public. Any fixed fallback digest is forgeable, because the forger
    knows it.

    So each failure is COUNTED (_axl_ed25519_glue_failures, see
    axl-ed25519-glue.h), and axl-pk-ed25519.c reads the count before and
    after every call into ref10 and refuses the operation if it moved --
    whatever ref10 returned. The zeroing below stays, for whatever reads
    the buffer before that refusal, but it is not what makes a failure
    safe. The refusal is.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha512.h>
#include <axl/axl-log.h>
#include <axl/axl-macros.h>
#include "axl-crypto-internal.h"
#include "axl-ed25519-glue.h"
#include "crypto_api.h"

/* The hash below cannot fail in this configuration: mbedtls_sha512()
   returns non-zero only for a bad is384, or when a compressor it does
   not own reports one. These are the two mbedTLS options that hand it
   such a compressor (deps/mbedtls/library/sha512.c: the whole module
   under MBEDTLS_SHA512_ALT, and mbedtls_internal_sha512_process under
   MBEDTLS_SHA512_PROCESS_ALT, whose non-zero return update and finish
   pass straight up). The A64 FEAT_SHA512 paths are mbedTLS's own and
   return success unconditionally, so they are not listed.

   The failure count still refuses the operation if the hash does fail,
   so these are not the only guard -- but a hash that CAN fail turns
   every such failure into a refused signature or login, and whoever
   enables one should have read why. The Ed25519 design spec's E4 plans
   exactly this, a PROCESS_ALT that dispatches to a hardware kernel; it
   stays compatible so long as that function cannot fail, and it says so
   by defining AXL_SHA512_PROCESS_ALT_CANNOT_FAIL. */
#if defined(MBEDTLS_SHA512_ALT)
#error "MBEDTLS_SHA512_ALT replaces the SHA-512 ref10 hashes through; a failure there is a refused Ed25519 operation at best -- see axl-ed25519-glue.c before enabling it"
#endif
#if defined(MBEDTLS_SHA512_PROCESS_ALT) \
    && !defined(AXL_SHA512_PROCESS_ALT_CANNOT_FAIL)
#error "MBEDTLS_SHA512_PROCESS_ALT gives the Ed25519 hash a way to fail; define AXL_SHA512_PROCESS_ALT_CANNOT_FAIL only if the replacement returns 0 unconditionally -- see axl-ed25519-glue.c"
#endif

AXL_LOG_DOMAIN("ed25519");

/* Every failure since boot; see axl-ed25519-glue.h. Atomic as defence in
   depth, NOT because running Ed25519 on an application processor is
   supported: sign and verify allocate through axl_malloc, which reaches
   Boot Services, and AxlTask's contract forbids Boot Services on an AP.
   If that ever changed, the reader still only needs to see that the count
   MOVED -- which a lost update from two racing plain increments would also
   show -- and the builtin makes the argument unnecessary. */
static uint32_t g_failures;
/* TEST HOOK state: the crypto_hash_sha512 call that fails, counting down;
   0 = disarmed. */
static uint32_t g_fail_hash_in;
static uint32_t g_randombytes_calls;

uint32_t
_axl_ed25519_glue_failures(void)
{
    return __atomic_load_n(&g_failures, __ATOMIC_SEQ_CST);
}

void
_axl_ed25519_glue_fail_hash(uint32_t n)
{
    g_fail_hash_in = n;
}

uint32_t
_axl_ed25519_glue_randombytes_calls(void)
{
    return __atomic_load_n(&g_randombytes_calls, __ATOMIC_SEQ_CST);
}

static void
note_failure(void)
{
    __atomic_add_fetch(&g_failures, 1u, __ATOMIC_SEQ_CST);
}

/* A scrub the optimiser cannot elide. Deliberately NOT axl_memset: a
   compiler may remove a memset to storage that is dead afterwards,
   which is precisely the storage this is for. Same shape as
   src/data/axl-hmac.c's secure_zero; not shared with it because that
   one is static to a translation unit in a different module and
   exporting a scrub across module boundaries to save six lines would
   put a security primitive behind a link-time indirection. */
static void
secure_zero(
    void   *p,  ///< buffer to scrub
    size_t  n   ///< bytes to scrub
)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

/* ref10 calls this five times and IGNORES the return every time (it is
   `crypto_hash_sha512(x, sm, smlen)` with no check, upstream), so nothing
   written into `out` on failure is safe -- see the file comment for what
   an all-zero digest does at the nonce and at the challenge. The failure
   is COUNTED instead, and the provider refuses the operation it happened
   in. `out` is still zeroed, so a digest that failed is at least not
   whatever the caller's stack held; that is hygiene, not the guarantee.

   mbedtls_sha512() rather than axl_compute_checksum_digest(): the one-shot
   keeps its context on the stack, where AxlChecksum allocated one -- and
   an allocation failure there was a live path to both consequences above
   until 2026-09-20. It is the same mbedTLS code either way (AxlDigest's
   SHA-512 wraps it; the Ed25519 design spec, section 3). What is left to
   fail in this configuration is a bad argument, which ref10 never passes;
   the #errors above keep it that way. */
int
crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                   unsigned long long inlen)
{
    if (out == NULL) {
        note_failure();
        return -1;
    }
    bool injected = false;
    if (g_fail_hash_in != 0) {
        g_fail_hash_in--;
        injected = g_fail_hash_in == 0;
    }
    if (injected || mbedtls_sha512(in, (size_t)inlen, out, 0) != 0) {
        secure_zero(out, crypto_hash_sha512_BYTES);
        note_failure();
        axl_error("SHA-512 failed; the Ed25519 operation is refused");
        return -1;
    }
    return 0;
}

/* Weak: explicit_bzero is a leaf (no init/teardown dependency), and newlib's
   keeps the same non-elidable-scrub contract this one does -- so newlib's is
   free to win where a real libc is on the link line. Weak still serves a
   link with no libc at all: a weak definition is extracted when nothing else
   provides the symbol. See scripts/check-libc-overlap.py's header comment for
   the leaf-vs-lifecycle-hook policy this follows. */
void __attribute__((weak))
explicit_bzero(void *p, size_t n)
{
    if (p != NULL) {
        secure_zero(p, n);
    }
}

/* Upstream's crypto_api.h maps this to arc4random_buf, whose signature
   is `void` -- so ref10's crypto_sign_ed25519_keypair cannot check it
   and does not. Whatever this leaves in `buf` becomes a key: an all-zero
   seed is a VALID key whose private half anyone can derive, so zeroing is
   no defence. A failure is COUNTED instead, the same as the hash's, so
   any provider path that reached this would refuse the key.

   None does, and the unit test holds it there (via
   _axl_ed25519_glue_randombytes_calls): AXL's keygen draws its seed from
   axl_crypto_rng() itself, where the failure IS checkable, and calls
   crypto_sign_ed25519_keypair_from_seed. This exists only because ref10's
   crypto_sign_ed25519_keypair references it; nothing in the tree calls
   that function. */
void
randombytes(unsigned char *buf, unsigned long long len)
{
    __atomic_add_fetch(&g_randombytes_calls, 1u, __ATOMIC_SEQ_CST);
    if (buf == NULL) {
        note_failure();
        return;
    }
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL
        || mbedtls_ctr_drbg_random(rng, buf, (size_t)len) != 0) {
        secure_zero(buf, (size_t)len);
        note_failure();
        axl_error("RNG failed; the Ed25519 key is refused");
    }
}
