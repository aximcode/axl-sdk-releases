/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-crypto.c
    Unit tests for axl_pk_verify() (public-key signature verification).

    Argument validation, the fail-closed contract, and the real ECDSA
    P-256 verify outcomes (valid / tampered / wrong key) all run here.

    They used to be two layers: mbedTLS was optional, so the real
    outcomes sat behind AXL_HAVE_TLS and a non-TLS build asserted an
    unavailable-stub contract instead. mbedTLS is unconditional now, so
    there is one layer and the stub branch is gone.
**/

#include <axl.h>
#include "axl-test.h"
#include "../data/pk-ecdsa-p256-vector.h"
#include "axl-pk-provider.h"
#include "../../deps/ed25519-ref10/crypto_api.h"

// ---------------------------------------------------------------------------
// axl_pk_sig_params_ok() -- the one enforcement point for spec §5's
// domain-separation rule. Both real call sites (axl_pk_key_sign,
// axl_pk_key_verify) pass compile-time constants that a compiler can
// fold to a constant result, so this is the only place that exercises
// the function with values it cannot see at compile time.
// ---------------------------------------------------------------------------

/* Route every argument through a volatile round-trip so the compiler
   cannot treat (mode, ctx, ctx_len) as compile-time constants and fold
   the whole call to a literal true/false -- which would make this test
   pass without ever running axl_pk_sig_params_ok()'s logic. */
static bool
sig_params_ok_runtime(AxlPkSigMode mode, const void *ctx, size_t ctx_len)
{
    volatile AxlPkSigMode v_mode    = mode;
    const void *volatile  v_ctx     = ctx;
    volatile size_t       v_ctx_len = ctx_len;

    return axl_pk_sig_params_ok(v_mode, v_ctx, v_ctx_len);
}

static void
test_pk_sig_params_ok(void)
{
    static const uint8_t some_ctx[1] = { 0x2a };

    /* Pure mode, NULL ctx, len 0 -- the one legal combination. */
    test_check(sig_params_ok_runtime(AXL_PK_SIG_MODE_PURE, NULL, 0) == true,
               "sig_params_ok: pure/NULL/0 -> true");

    /* Non-NULL ctx with len 0 -- the ambiguous case three review rounds
       settled: an ignored context must not silently pass. */
    test_check(sig_params_ok_runtime(AXL_PK_SIG_MODE_PURE, some_ctx, 0)
                   == false,
               "sig_params_ok: pure/non-NULL ctx/len 0 -> false");

    /* NULL ctx with a non-zero length -- there is nothing to read. */
    test_check(sig_params_ok_runtime(AXL_PK_SIG_MODE_PURE, NULL, 1) == false,
               "sig_params_ok: pure/NULL ctx/len != 0 -> false");

    /* Non-NULL ctx with a non-zero length -- still refused, because pure
       mode carries no context at all. */
    test_check(sig_params_ok_runtime(AXL_PK_SIG_MODE_PURE, some_ctx, 1)
                   == false,
               "sig_params_ok: pure/non-NULL ctx/len != 0 -> false");
}

// ---------------------------------------------------------------------------
// Argument validation — real in every build (validation precedes mbedTLS).
// ---------------------------------------------------------------------------

static void
test_arg_validation(void)
{
    /* NULL public key */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             NULL, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: NULL pubkey -> AXL_ERR");

    /* Zero-length public key */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, 0,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: zero-length pubkey -> AXL_ERR");

    /* NULL signature */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             NULL, pk_sig_len) == AXL_ERR,
               "pk_verify: NULL sig -> AXL_ERR");

    /* Zero-length signature */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, 0) == AXL_ERR,
               "pk_verify: zero-length sig -> AXL_ERR");

    /* NULL message with non-zero length */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             NULL, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: NULL msg with msg_len > 0 -> AXL_ERR");

    /* Reserved/unsupported algorithm (Ed25519) with otherwise-valid args */
    test_check(axl_pk_verify(AXL_PK_ED25519,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: AXL_PK_ED25519 (unsupported) -> AXL_ERR");

    /* Out-of-range algorithm value */
    test_check(axl_pk_verify((AxlPkAlg)99,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: bogus alg -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Cryptographic outcomes, against a real mbedTLS.
// ---------------------------------------------------------------------------

static void
test_verify_outcomes(void)
{
    test_check(axl_pk_available() == true,
               "pk_available: true");

    /* The other half of AxlTestEd25519's missing-provider set: this
       binary calls axl_pk_enable_mbedtls() (in test_crypto_main), so the
       queries that answer "no" there answer "yes" here. If both binaries
       ever agree, the weak seam has stopped discriminating. */
    test_check(axl_pk_alg_available(AXL_PK_ECDSA_P256)
               && axl_pk_alg_available(AXL_PK_ECDSA_P384)
               && axl_pk_alg_available(AXL_PK_RSA),
               "pk_alg_available: ECDSA P-256 / P-384 / RSA with the mbedTLS pull");
    test_check(_axl_pk_provider_for(AXL_PK_RSA) != NULL,
               "pk_alg_available: the mbedTLS provider resolves");

    /* The known-good vector verifies. */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_OK,
               "pk_verify: valid ECDSA-P256 signature -> AXL_OK");

    /* Tampered message: flip one byte of the body. */
    {
        uint8_t bad_msg[sizeof(pk_msg)];
        axl_memcpy(bad_msg, pk_msg, sizeof(pk_msg));
        bad_msg[0] ^= 0x01;
        test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                                 pk_pub, pk_pub_len,
                                 bad_msg, pk_msg_len,
                                 pk_sig, pk_sig_len) == AXL_ERR,
                   "pk_verify: tampered message -> AXL_ERR");
    }

    /* Tampered signature: flip one byte of an INTEGER value. */
    {
        uint8_t bad_sig[sizeof(pk_sig)];
        axl_memcpy(bad_sig, pk_sig, sizeof(pk_sig));
        bad_sig[sizeof(pk_sig) - 1] ^= 0x01;
        test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                                 pk_pub, pk_pub_len,
                                 pk_msg, pk_msg_len,
                                 bad_sig, pk_sig_len) == AXL_ERR,
                   "pk_verify: tampered signature -> AXL_ERR");
    }

    /* Truncated signature (drops trailing DER byte). */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len - 1) == AXL_ERR,
               "pk_verify: truncated signature -> AXL_ERR");

    /* Correct signature, wrong (unrelated) public key. */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub2, pk_pub2_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: wrong public key -> AXL_ERR");

    /* Malformed public key (truncated SubjectPublicKeyInfo). */
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len - 4,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: malformed pubkey -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Key handles — keygen, serialize, sign, verify.
// ---------------------------------------------------------------------------

static void
test_key_handle(void)
{
    // --- ECDSA P-256: live keygen + sign/verify round-trips ---
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ECDSA_P256);
    test_check(k != NULL, "keygen: ECDSA P-256 -> key");
    test_check(axl_pk_key_alg(k) == AXL_PK_ECDSA_P256,
               "keygen: ECDSA key reports its alg");

    // Raw (P1363) signature round-trip.
    uint8_t sig[512];
    size_t  sl = sizeof(sig);
    test_check(axl_pk_key_sign(k, pk_msg, pk_msg_len, AXL_PK_SIG_RAW,
                               sig, &sl) == AXL_OK,
               "sign: ECDSA raw -> AXL_OK");
    test_check(sl == 64, "sign: ECDSA raw signature is 64 bytes");
    test_check(axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_RAW,
                                 sig, sl) == AXL_OK,
               "verify: ECDSA raw round-trip -> AXL_OK");
    sig[0] ^= 0x01;
    test_check(axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_RAW,
                                 sig, sl) == AXL_ERR,
               "verify: tampered raw signature -> AXL_ERR");
    sig[0] ^= 0x01;
    // A raw signature read as DER must not validate.
    test_check(axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                 sig, sl) == AXL_ERR,
               "verify: raw signature mislabeled DER -> AXL_ERR");
    // Wrong raw length is rejected.
    test_check(axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_RAW,
                                 sig, sl - 1) == AXL_ERR,
               "verify: raw signature wrong length -> AXL_ERR");

    // DER signature round-trip.
    uint8_t dsig[512];
    size_t  dl = sizeof(dsig);
    test_check(axl_pk_key_sign(k, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               dsig, &dl) == AXL_OK,
               "sign: ECDSA DER -> AXL_OK");
    test_check(axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                 dsig, dl) == AXL_OK,
               "verify: ECDSA DER round-trip -> AXL_OK");

    // Size queries.
    size_t q = 0;
    test_check(axl_pk_key_sign(k, pk_msg, pk_msg_len, AXL_PK_SIG_RAW,
                               NULL, &q) == AXL_OK && q == 64,
               "sign: raw size query -> 64");
    q = 0;
    test_check(axl_pk_key_sign(k, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               NULL, &q) == AXL_OK && q >= dl,
               "sign: DER size query -> safe upper bound");

    // Serialize private -> reload -> the reloaded key verifies sigs from
    // the original (proves the same key survived the round-trip).
    uint8_t prv[4096];
    size_t  pl = sizeof(prv);
    test_check(axl_pk_key_get_private_der(k, prv, &pl) == AXL_OK && pl > 0,
               "serialize: ECDSA private DER");
    AxlPkKey *k2 = axl_pk_key_load_private(prv, pl);
    test_check(k2 != NULL && axl_pk_key_alg(k2) == AXL_PK_ECDSA_P256,
               "load: ECDSA private DER round-trip");
    uint8_t s2[512];
    size_t  s2l = sizeof(s2);
    test_check(axl_pk_key_sign(k2, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               s2, &s2l) == AXL_OK
               && axl_pk_key_verify(k, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                    s2, s2l) == AXL_OK,
               "round-trip: reloaded private key signs, original verifies");

    // Serialize public -> load public-only -> verify; private ops fail.
    uint8_t pub[512];
    size_t  publ = sizeof(pub);
    test_check(axl_pk_key_get_public_der(k, pub, &publ) == AXL_OK && publ > 0,
               "serialize: ECDSA public DER");
    AxlPkKey *kp = axl_pk_key_load_public(pub, publ);
    test_check(kp != NULL, "load: ECDSA public DER");
    test_check(axl_pk_key_verify(kp, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                 dsig, dl) == AXL_OK,
               "verify: public-only key verifies a DER signature");
    size_t tmpq = sizeof(prv);
    test_check(axl_pk_key_get_private_der(kp, prv, &tmpq) == AXL_ERR,
               "serialize: private DER from public-only key -> AXL_ERR");
    test_check(axl_pk_key_sign(kp, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               s2, &s2l) == AXL_ERR,
               "sign: public-only key -> AXL_ERR");

    // Interop: the serialized public key + a DER signature verify through
    // the raw-bytes axl_pk_verify() too.
    test_check(axl_pk_verify(AXL_PK_ECDSA_P256, pub, publ,
                             pk_msg, pk_msg_len, dsig, dl) == AXL_OK,
               "interop: axl_pk_verify accepts key-handle pubkey + sig");

    // Misuse / unsupported.
    test_check(axl_pk_key_alg(NULL) == AXL_PK_ED25519,
               "key_alg: NULL -> reserved zero");
    test_check(axl_pk_key_load_public(pk_msg, pk_msg_len) == NULL,
               "load: garbage public DER -> NULL");
    test_check(axl_pk_key_new(AXL_PK_ED25519) == NULL,
               "keygen: Ed25519 (unsupported) -> NULL");

    axl_pk_key_free(kp);
    axl_pk_key_free(k2);
    axl_pk_key_free(k);

    // --- RSA via a pre-generated key (fmt is ignored for RSA) ---
    AxlPkKey *r = axl_pk_key_load_private(pk_rsa3072_pkcs8,
                                          pk_rsa3072_pkcs8_len);
    test_check(r != NULL && axl_pk_key_alg(r) == AXL_PK_RSA,
               "load: RSA-3072 private key");
    uint8_t rsig[512];
    size_t  rl = sizeof(rsig);
    test_check(axl_pk_key_sign(r, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               rsig, &rl) == AXL_OK
               && axl_pk_key_verify(r, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                    rsig, rl) == AXL_OK,
               "round-trip: RSA sign/verify");
    rsig[0] ^= 0x01;
    test_check(axl_pk_key_verify(r, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                 rsig, rl) == AXL_ERR,
               "verify: tampered RSA signature -> AXL_ERR");
    rsig[0] ^= 0x01;
    uint8_t rpub[1024];
    size_t  rpl = sizeof(rpub);
    test_check(axl_pk_key_get_public_der(r, rpub, &rpl) == AXL_OK,
               "serialize: RSA public DER");
    AxlPkKey *rp = axl_pk_key_load_public(rpub, rpl);
    test_check(rp != NULL
               && axl_pk_key_verify(rp, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                    rsig, rl) == AXL_OK,
               "verify: reloaded RSA public key verifies");
    axl_pk_key_free(rp);
    axl_pk_key_free(r);

    // --- RSA live keygen (slower; proves the keygen path) ---
    AxlPkKey *rk = axl_pk_key_new(AXL_PK_RSA);
    test_check(rk != NULL && axl_pk_key_alg(rk) == AXL_PK_RSA,
               "keygen: RSA-3072 -> key");
    rl = sizeof(rsig);
    test_check(axl_pk_key_sign(rk, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               rsig, &rl) == AXL_OK
               && axl_pk_key_verify(rk, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                                    rsig, rl) == AXL_OK,
               "round-trip: generated RSA key signs and verifies");
    axl_pk_key_free(rk);
}

/* Captures AxlPk's missing-provider error line (domain "pk"). */
static size_t pk_ed_absent_errors;
static char   pk_ed_absent_msg[256];

static void
pk_ed_absent_capture(int level, const char *domain, const char *message,
                     const AxlRealtime *stamp, void *data)
{
    (void)stamp;
    (void)data;
    if (level == AXL_LOG_ERROR && domain != NULL
        && axl_strcmp(domain, "pk") == 0) {
        pk_ed_absent_errors++;
        axl_snprintf(pk_ed_absent_msg, sizeof(pk_ed_absent_msg), "%s",
                     message);
    }
}

static void
pk_ed_absent_reset(void)
{
    pk_ed_absent_errors = 0;
    pk_ed_absent_msg[0] = '\0';
}

static void
test_pk_reserved_alg_fails_closed(void)
{
    /* AXL_PK_ED25519 is a valid enumerator with no implementation. It
       must fail closed at every entry point rather than being quietly
       treated as P-256 -- md_for_alg and ec_order_bytes are ternaries,
       so no -Wswitch names them when an algorithm is added. */
    pk_ed_absent_reset();
    test_check(axl_log_add_handler(pk_ed_absent_capture, NULL) == AXL_OK,
               "pk reserved: capture handler installed");
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ED25519);
    test_check(k == NULL, "pk reserved: key_new(ED25519) returns NULL");
    axl_log_remove_handler(pk_ed_absent_capture);
    axl_pk_key_free(k);
    /* ...and says which function would have made it work: this binary
       never calls axl_pk_enable_ed25519(). Exactly one line, verbatim. */
    test_check(pk_ed_absent_errors == 1
               && axl_strcmp(pk_ed_absent_msg,
                             "Ed25519 unavailable: call axl_pk_enable_ed25519()"
                             " before using Ed25519 keys") == 0,
               "pk reserved: key_new(ED25519) logs the exact enable line once");

    /* The DER-import half of the same diagnostic: an Ed25519
       SubjectPublicKeyInfo (RFC 8032 TEST 1's key) names id-Ed25519, so
       the refusal is blamed on the missing provider -- once, verbatim. */
    static const uint8_t ed_spki[44] = {
        0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21,
        0x00, 0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b,
        0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda,
        0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
    };
    pk_ed_absent_reset();
    test_check(axl_log_add_handler(pk_ed_absent_capture, NULL) == AXL_OK,
               "pk reserved: capture handler installed (DER)");
    AxlPkKey *ek = axl_pk_key_load_public(ed_spki, sizeof(ed_spki));
    axl_log_remove_handler(pk_ed_absent_capture);
    test_check(ek == NULL,
               "pk reserved: load_public(Ed25519 SPKI) returns NULL");
    axl_pk_key_free(ek);
    test_check(pk_ed_absent_errors == 1
               && axl_strcmp(pk_ed_absent_msg,
                             "Ed25519 unavailable: call axl_pk_enable_ed25519()"
                             " before using Ed25519 keys") == 0,
               "pk reserved: load_public(Ed25519 SPKI) logs the exact enable line once");

    /* A real P-256 key asked to behave as a reserved algorithm cannot
       be constructed through the public API, so drive the reserved
       path through the one-shot verifier, which takes the algorithm
       as an argument. It returns int (AXL_OK / AXL_ERR), not bool, and
       is AXL_WARN_UNUSED -- the header documents AXL_PK_ED25519 as
       returning AXL_ERR, and this is what pins that promise. */
    static const uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    test_check(axl_pk_verify(AXL_PK_ED25519, junk, sizeof(junk),
                             junk, sizeof(junk), junk, sizeof(junk))
                   == AXL_ERR,
               "pk reserved: verify(ED25519) refuses");

    /* axl_pk_alg_available reports per-algorithm availability, which
       is what distinguishes 'not linked' from 'failed'. This binary
       enables ECDSA/RSA and not Ed25519. */
    test_check(axl_pk_alg_available(AXL_PK_ECDSA_P256),
               "pk reserved: P-256 is available");
    test_check(axl_pk_alg_available(AXL_PK_RSA),
               "pk reserved: RSA is available");
    test_check(!axl_pk_alg_available(AXL_PK_ED25519),
               "pk reserved: Ed25519 is not enabled in this image");
}

static void
test_ed25519_vendored_links(void)
{
    /* E2b Task 1 vendors ref10 and wires its three externals, but
       registers NO provider -- so the algorithm must STILL fail closed
       through the public API. This test pins both halves at once,
       which is the only claim Task 1 is entitled to make:

         - the vendored code, its externals and the build rule are all
           correct, evidenced by it reproducing a vector it did not
           choose;
         - and nothing about AXL_PK_ED25519's reachability has moved.

       test_pk_reserved_alg_fails_closed() above owns the full
       fail-closed set; the one assertion repeated here is what ties
       the two halves together in a single test. */
    static const uint8_t seed1[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
    };
    /* RFC 8032 §7.1 TEST 1. Bytes specified by someone else, so
       passing is evidence about the standard rather than about us. */
    static const char *PUB1 =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

    uint8_t pk[crypto_sign_ed25519_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_ed25519_SECRETKEYBYTES];

    test_check(crypto_sign_ed25519_keypair_from_seed(pk, sk, seed1) == 0,
               "ed25519 vendored: keypair_from_seed succeeds");
    test_check(test_hex_equals(pk, sizeof(pk), PUB1),
               "ed25519 vendored: RFC 8032 TEST 1 public key");
    /* ref10's secret key is seed || public key, not a clamped scalar.
       Pinning that layout here is what the provider's union arm relies
       on, and it is the half of the contract a resync could silently
       change. */
    test_check(test_hex_equals(sk + 32, 32, PUB1),
               "ed25519 vendored: sk's second half is the public key");

    /* Still fails closed -- no provider is registered until Task 3. */
    test_check(!axl_pk_alg_available(AXL_PK_ED25519),
               "ed25519 vendored: still unavailable without a provider");
}

static void
test_pk_raw_public_roundtrip(void)
{
    /* SEC1 uncompressed is 0x04 || X || Y -- 65 bytes for P-256. A
       round-trip through a freshly generated key is self-checking
       without pinning a key we do not control: export, re-import,
       export again, and require the two exports to be identical. */
    AxlPkKey *key = axl_pk_key_new(AXL_PK_ECDSA_P256);
    test_check(key != NULL, "pk raw: keygen P-256");
    if (key == NULL) {
        return;
    }

    size_t need = 0;
    int rc = axl_pk_key_get_raw_public(key, NULL, &need);
    test_check(rc == AXL_OK && need == 65,
               "pk raw: P-256 size query reports 65");

    uint8_t raw[65];
    size_t  len = sizeof(raw);
    rc = axl_pk_key_get_raw_public(key, raw, &len);
    test_check(rc == AXL_OK && len == 65, "pk raw: P-256 export");
    test_check(raw[0] == 0x04, "pk raw: SEC1 uncompressed tag");

    AxlPkKey *back = axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256, raw, len);
    test_check(back != NULL, "pk raw: re-import");
    test_check(back != NULL && axl_pk_key_alg(back) == AXL_PK_ECDSA_P256,
               "pk raw: re-imported key reports P-256");

    uint8_t raw2[65];
    size_t  len2 = sizeof(raw2);
    rc = axl_pk_key_get_raw_public(back, raw2, &len2);
    test_check(rc == AXL_OK && len2 == 65, "pk raw: re-export");
    test_check(len2 == 65 && axl_memcmp(raw, raw2, 65) == 0,
               "pk raw: round-trip is byte-identical");

    axl_pk_key_free(back);
    axl_pk_key_free(key);
}

static void
test_pk_raw_public_rejects(void)
{
    static const uint8_t junk[65] = { 0x04 };

    /* RSA has no raw form. */
    test_check(axl_pk_key_from_raw_public(AXL_PK_RSA, junk, sizeof(junk))
                   == NULL,
               "pk raw: RSA has no raw form");

    /* Ed25519 is opt-in (axl_pk_enable_ed25519), and this binary never
       enables it, so no provider takes the raw key. */
    test_check(axl_pk_key_from_raw_public(AXL_PK_ED25519, junk, 32) == NULL,
               "pk raw: Ed25519 not enabled in this image");

    /* Wrong length for the curve. */
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256, junk, 64)
                   == NULL,
               "pk raw: P-256 rejects a 64-byte input");
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256, junk, 97)
                   == NULL,
               "pk raw: P-256 rejects a P-384-sized input");

    /* Right length, wrong leading tag -- 0x02/0x03 is a COMPRESSED
       point, which this entry point does not accept. Distinguishing
       'unsupported encoding' from 'bad point' matters: a compressed
       point is well-formed, just not what we take. */
    uint8_t compressed[65];
    axl_memset(compressed, 0, sizeof(compressed));
    compressed[0] = 0x02;
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256,
                                          compressed, 65) == NULL,
               "pk raw: rejects a compressed-point tag");

    /* Right length and tag, but not on the curve. */
    uint8_t offcurve[65];
    axl_memset(offcurve, 0xAB, sizeof(offcurve));
    offcurve[0] = 0x04;
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256,
                                          offcurve, 65) == NULL,
               "pk raw: rejects a point not on the curve");

    /* NULL guards. */
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256, NULL, 65)
                   == NULL,
               "pk raw: NULL input refused");
    size_t len = 0;
    test_check(axl_pk_key_get_raw_public(NULL, NULL, &len) == AXL_ERR,
               "pk raw: NULL key refused");

    /* RSA has no raw export either -- the flen == 0 rejection on the
       export side, not just the import side already covered above. */
    AxlPkKey *rsa = axl_pk_key_load_private(pk_rsa3072_pkcs8,
                                            pk_rsa3072_pkcs8_len);
    test_check(rsa != NULL, "pk raw: load RSA key for export-reject check");
    if (rsa != NULL) {
        uint8_t rsa_buf[512];
        size_t  rsa_len = sizeof(rsa_buf);
        test_check(axl_pk_key_get_raw_public(rsa, rsa_buf, &rsa_len)
                       == AXL_ERR,
                   "pk raw: RSA export has no raw form -> AXL_ERR");
        axl_pk_key_free(rsa);
    }

    /* Too-small output buffer: AXL_ERR, and *len is set to the size
       that would have been required, per the size-query protocol. */
    AxlPkKey *p256 = axl_pk_key_new(AXL_PK_ECDSA_P256);
    test_check(p256 != NULL, "pk raw: keygen P-256 for too-small check");
    if (p256 != NULL) {
        uint8_t small_buf[8];
        size_t  small_len = sizeof(small_buf);
        test_check(axl_pk_key_get_raw_public(p256, small_buf, &small_len)
                       == AXL_ERR && small_len == 65,
                   "pk raw: too-small buffer -> AXL_ERR + required size");
        axl_pk_key_free(p256);
    }
}

// ---------------------------------------------------------------------------
// AEAD — AES-GCM and ChaCha20-Poly1305.
// ---------------------------------------------------------------------------

static bool
buf_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* KAT: seal must reproduce the reference ciphertext+tag exactly, and
   open must recover the plaintext. */
static void
aead_kat(AxlAeadAlg alg, const char *name,
         const uint8_t *key, size_t key_len,
         const uint8_t *ref_ct, const uint8_t *ref_tag)
{
    uint8_t ct[64], tag[AXL_AEAD_TAG_LEN], pt[64];
    char    msg[96];

    size_t n = aead_pt_len;
    int rc = axl_aead_seal(alg, key, key_len, aead_nonce, AXL_AEAD_NONCE_LEN,
                           aead_aad, aead_aad_len, aead_pt, n,
                           ct, tag, AXL_AEAD_TAG_LEN);
    axl_snprintf(msg, sizeof(msg), "aead %s: seal ciphertext matches KAT", name);
    test_check(rc == AXL_OK && buf_eq(ct, ref_ct, n), msg);
    axl_snprintf(msg, sizeof(msg), "aead %s: seal tag matches KAT", name);
    test_check(buf_eq(tag, ref_tag, AXL_AEAD_TAG_LEN), msg);

    rc = axl_aead_open(alg, key, key_len, aead_nonce, AXL_AEAD_NONCE_LEN,
                       aead_aad, aead_aad_len, ct, n, tag, AXL_AEAD_TAG_LEN, pt);
    axl_snprintf(msg, sizeof(msg), "aead %s: open recovers plaintext", name);
    test_check(rc == AXL_OK && buf_eq(pt, aead_pt, n), msg);

    /* Tampered ciphertext, tag, and AAD must each fail closed. */
    ct[0] ^= 0x01;
    axl_snprintf(msg, sizeof(msg), "aead %s: tampered ciphertext -> AXL_ERR", name);
    test_check(axl_aead_open(alg, key, key_len, aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, ct, n, tag,
                             AXL_AEAD_TAG_LEN, pt) == AXL_ERR, msg);
    ct[0] ^= 0x01;
    tag[0] ^= 0x01;
    axl_snprintf(msg, sizeof(msg), "aead %s: tampered tag -> AXL_ERR", name);
    test_check(axl_aead_open(alg, key, key_len, aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, ct, n, tag,
                             AXL_AEAD_TAG_LEN, pt) == AXL_ERR, msg);
    tag[0] ^= 0x01;
    uint8_t bad_aad[64];
    axl_memcpy(bad_aad, aead_aad, aead_aad_len);
    bad_aad[0] ^= 0x01;
    axl_snprintf(msg, sizeof(msg), "aead %s: tampered AAD -> AXL_ERR", name);
    test_check(axl_aead_open(alg, key, key_len, aead_nonce, AXL_AEAD_NONCE_LEN,
                             bad_aad, aead_aad_len, ct, n, tag,
                             AXL_AEAD_TAG_LEN, pt) == AXL_ERR, msg);
}

static void
test_aead(void)
{
    aead_kat(AXL_AEAD_AES_256_GCM, "aes256gcm", aead_key32, 32,
             aead_gcm256_ct, aead_gcm256_tag);
    aead_kat(AXL_AEAD_AES_128_GCM, "aes128gcm", aead_key16, 16,
             aead_gcm128_ct, aead_gcm128_tag);
    aead_kat(AXL_AEAD_CHACHA20_POLY1305, "chachapoly", aead_key32, 32,
             aead_chacha_ct, aead_chacha_tag);

    // In-place: ciphertext aliases plaintext, then open in place.
    uint8_t buf[64], tag[AXL_AEAD_TAG_LEN];
    size_t  n = aead_pt_len;
    axl_memcpy(buf, aead_pt, n);
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, buf, n,
                             buf, tag, AXL_AEAD_TAG_LEN) == AXL_OK
               && buf_eq(buf, aead_gcm256_ct, n),
               "aead: in-place seal matches KAT");
    test_check(axl_aead_open(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, buf, n, tag,
                             AXL_AEAD_TAG_LEN, buf) == AXL_OK
               && buf_eq(buf, aead_pt, n),
               "aead: in-place open round-trips");

    // Empty plaintext: still authenticates.
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, NULL, 0,
                             NULL, tag, AXL_AEAD_TAG_LEN) == AXL_OK,
               "aead: empty-plaintext seal -> AXL_OK");
    test_check(axl_aead_open(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, NULL, 0, tag,
                             AXL_AEAD_TAG_LEN, NULL) == AXL_OK,
               "aead: empty-plaintext open -> AXL_OK");
    // ChaCha20-Poly1305 empty-plaintext path (NULL in/out, len 0).
    test_check(axl_aead_seal(AXL_AEAD_CHACHA20_POLY1305, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN,
                             aead_aad, aead_aad_len, NULL, 0,
                             NULL, tag, AXL_AEAD_TAG_LEN) == AXL_OK
               && axl_aead_open(AXL_AEAD_CHACHA20_POLY1305, aead_key32, 32,
                                aead_nonce, AXL_AEAD_NONCE_LEN,
                                aead_aad, aead_aad_len, NULL, 0, tag,
                                AXL_AEAD_TAG_LEN, NULL) == AXL_OK,
               "aead: ChaChaPoly empty-plaintext round-trip -> AXL_OK");

    // Argument validation.
    uint8_t ct[64];
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 16,
                             aead_nonce, AXL_AEAD_NONCE_LEN, aead_aad,
                             aead_aad_len, aead_pt, n, ct, tag,
                             AXL_AEAD_TAG_LEN) == AXL_ERR,
               "aead: wrong key length -> AXL_ERR");
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, 8, aead_aad, aead_aad_len, aead_pt,
                             n, ct, tag, AXL_AEAD_TAG_LEN) == AXL_ERR,
               "aead: wrong nonce length -> AXL_ERR");
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN, aead_aad,
                             aead_aad_len, aead_pt, n, ct, tag, 8) == AXL_ERR,
               "aead: wrong tag length -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// AES-CTR stream cipher.
// ---------------------------------------------------------------------------

static void
test_cipher(void)
{
    size_t n = aead_pt_len;

    /* KAT: AES-256-CTR encrypt reproduces the reference ciphertext. */
    AxlCipher *c = axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 32, ctr_iv);
    uint8_t    ct[64];
    test_check(c != NULL
               && axl_cipher_ctr_xcrypt(c, aead_pt, n, ct) == AXL_OK
               && buf_eq(ct, ctr_ct256, n),
               "cipher aes256ctr: encrypt matches KAT");
    axl_cipher_free(c);

    /* Decrypt is the same operation: ct -> plaintext. */
    c = axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 32, ctr_iv);
    uint8_t pt[64];
    test_check(c != NULL
               && axl_cipher_ctr_xcrypt(c, ct, n, pt) == AXL_OK
               && buf_eq(pt, aead_pt, n),
               "cipher aes256ctr: decrypt round-trips");
    axl_cipher_free(c);

    /* AES-128-CTR KAT. */
    c = axl_cipher_ctr_new(AXL_CIPHER_AES_128_CTR, ctr_key16, 16, ctr_iv);
    test_check(c != NULL
               && axl_cipher_ctr_xcrypt(c, aead_pt, n, ct) == AXL_OK
               && buf_eq(ct, ctr_ct128, n),
               "cipher aes128ctr: encrypt matches KAT");
    axl_cipher_free(c);

    /* The keystream carries across calls: chunked == one-shot. */
    c = axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 32, ctr_iv);
    uint8_t chunked[64];
    bool ok = c != NULL
              && axl_cipher_ctr_xcrypt(c, aead_pt, 10, chunked) == AXL_OK
              && axl_cipher_ctr_xcrypt(c, aead_pt + 10, n - 10, chunked + 10) == AXL_OK
              && buf_eq(chunked, ctr_ct256, n);
    test_check(ok, "cipher: chunked xcrypt continues one keystream");
    axl_cipher_free(c);

    /* In-place. */
    c = axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 32, ctr_iv);
    uint8_t buf[64];
    axl_memcpy(buf, aead_pt, n);
    test_check(c != NULL
               && axl_cipher_ctr_xcrypt(c, buf, n, buf) == AXL_OK
               && buf_eq(buf, ctr_ct256, n),
               "cipher: in-place encrypt matches KAT");
    axl_cipher_free(c);

    /* Argument validation. */
    test_check(axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 16, ctr_iv) == NULL,
               "cipher: wrong key length -> NULL");
    test_check(axl_cipher_ctr_xcrypt(NULL, aead_pt, n, ct) == AXL_ERR,
               "cipher: NULL context -> AXL_ERR");
    axl_cipher_free(NULL);  /* NULL-safe */
}

// ---------------------------------------------------------------------------
// ECDH key agreement.
// ---------------------------------------------------------------------------

/* Two-party agreement for one curve: both sides derive the same secret. */
static void
ecdh_agreement(AxlEcdhAlg alg, const char *name, size_t pub_len)
{
    char     msg[96];
    AxlEcdh *a = axl_ecdh_new(alg);
    AxlEcdh *b = axl_ecdh_new(alg);

    uint8_t pa[65], pb[65];
    size_t  pal = sizeof(pa), pbl = sizeof(pb);
    bool    pub_ok = a != NULL && b != NULL
                     && axl_ecdh_get_public(a, pa, &pal) == AXL_OK
                     && axl_ecdh_get_public(b, pb, &pbl) == AXL_OK
                     && pal == pub_len && pbl == pub_len;
    axl_snprintf(msg, sizeof(msg), "ecdh %s: public keys are %zu bytes", name, pub_len);
    test_check(pub_ok, msg);

    uint8_t sa[32], sb[32];
    size_t  sal = sizeof(sa), sbl = sizeof(sb);
    bool    agree = pub_ok
                    && axl_ecdh_compute(a, pb, pbl, sa, &sal) == AXL_OK
                    && axl_ecdh_compute(b, pa, pal, sb, &sbl) == AXL_OK
                    && sal == 32 && sbl == 32 && buf_eq(sa, sb, 32);
    axl_snprintf(msg, sizeof(msg), "ecdh %s: both sides derive the same secret", name);
    test_check(agree, msg);

    /* A wrong-length peer key fails closed for either curve. */
    uint8_t bad[65];
    axl_memset(bad, 0xAB, sizeof(bad));
    sal = sizeof(sa);
    axl_snprintf(msg, sizeof(msg), "ecdh %s: wrong-length peer key -> AXL_ERR", name);
    test_check(a != NULL
               && axl_ecdh_compute(a, bad, pub_len - 1, sa, &sal) == AXL_ERR, msg);

    axl_ecdh_free(a);
    axl_ecdh_free(b);
}

static void
test_ecdh(void)
{
    ecdh_agreement(AXL_ECDH_P256, "p256", 65);
    ecdh_agreement(AXL_ECDH_X25519, "x25519", 32);

    /* Size queries. */
    AxlEcdh *e = axl_ecdh_new(AXL_ECDH_P256);
    size_t   q = 0;
    test_check(e != NULL
               && axl_ecdh_get_public(e, NULL, &q) == AXL_OK && q == 65,
               "ecdh: public-key size query (P-256) -> 65");
    q = 0;
    test_check(axl_ecdh_compute(e, (const uint8_t *)"x", 1, NULL, &q) == AXL_OK
               && q == 32,
               "ecdh: shared-secret size query -> 32");
    /* Too-small public-key buffer. */
    uint8_t small[8];
    size_t  need = 0;
    test_check(axl_ecdh_get_public(e, small, &need) == AXL_ERR && need == 65,
               "ecdh: too-small public buffer -> AXL_ERR + required size");
    /* P-256 rejects an off-curve / malformed point (unlike X25519, where
       every 32-byte string is a valid u-coordinate). */
    uint8_t off_curve[65];
    axl_memset(off_curve, 0xAB, sizeof(off_curve));
    uint8_t secret[32];
    size_t  sl = sizeof(secret);
    test_check(axl_ecdh_compute(e, off_curve, sizeof(off_curve), secret, &sl) == AXL_ERR,
               "ecdh: P-256 off-curve peer point -> AXL_ERR");
    axl_ecdh_free(e);

    /* Misuse. */
    test_check(axl_ecdh_new((AxlEcdhAlg)99) == NULL,
               "ecdh: bad algorithm -> NULL");
    test_check(axl_ecdh_get_public(NULL, small, &need) == AXL_ERR,
               "ecdh: get_public NULL context -> AXL_ERR");
    axl_ecdh_free(NULL);  /* NULL-safe */
}

// ---------------------------------------------------------------------------
// Entropy policy -- axl_crypto_set_weak_entropy_allowed() /
// axl_crypto_is_weak_entropy_allowed().
//
// The DRBG behind every key this SDK produces has one entropy source,
// EFI_RNG_PROTOCOL. What happens when that source is missing is a
// security decision, so it is pinned here rather than left to whatever
// the platform shim happens to do. The fallback path itself is only
// reachable on firmware that publishes no RNG -- see the sabotage run
// recorded in the commit message.
// ---------------------------------------------------------------------------

static void
test_weak_entropy_policy(void)
{
    /* The default is the whole point: a platform with no hardware RNG
       fails to produce a key rather than deriving one from a clock. */
    test_check(!axl_crypto_is_weak_entropy_allowed(),
               "entropy: weak fallback is DISALLOWED by default");

    axl_crypto_set_weak_entropy_allowed(true);
    test_check(axl_crypto_is_weak_entropy_allowed(),
               "entropy: opt-in reads back true");

    axl_crypto_set_weak_entropy_allowed(false);
    test_check(!axl_crypto_is_weak_entropy_allowed(),
               "entropy: opt-in is revocable");

    /* Failing closed costs nothing where the firmware publishes
       EFI_RNG_PROTOCOL -- the DRBG still seeds and keys are still
       produced. axl_rng_bytes() is the honest probe for that: it is
       the one entry point that never falls back. */
    uint8_t probe[8];
    if (axl_rng_bytes(probe, sizeof probe) != AXL_OK) {
        /* One balancer for the one populated assertion below. */
        test_skip_n(1, "entropy: no EFI_RNG_PROTOCOL -- fail-closed keygen "
                       "SKIPPED");
        return;
    }
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ECDSA_P256);
    test_check(k != NULL,
               "entropy: hardware RNG present -- keygen succeeds fail-closed");
    axl_pk_key_free(k);
}

static int
test_crypto_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlCrypto");

    /* ECDSA/RSA through the key-handle API are opt-in. (axl_pk_verify()
       below would link the provider by itself; the call is still the
       documented way, and what a direct user writes.) Deliberately NOT
       axl_pk_enable_ed25519() -- its fail-closed half is asserted here. */
    axl_pk_enable_mbedtls();

    test_weak_entropy_policy();
    test_pk_sig_params_ok();
    test_arg_validation();
    test_verify_outcomes();
    test_key_handle();
    test_pk_reserved_alg_fails_closed();
    test_ed25519_vendored_links();
    test_pk_raw_public_roundtrip();
    test_pk_raw_public_rejects();
    test_aead();
    test_cipher();
    test_ecdh();


    return test_print_results();
}

AXL_APP(test_crypto_main)
