/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-crypto.c
    Unit tests for axl_pk_verify() (public-key signature verification).

    Two layers:
      - Argument validation and the fail-closed contract run in every
        build (the stub returns AXL_ERR without AXL_TLS).
      - The real ECDSA P-256 verify outcomes (valid / tampered / wrong
        key) require an AXL_TLS=1 build (mbedTLS). They are guarded by
        AXL_HAVE_TLS; the non-TLS build asserts the unavailable
        fail-closed contract instead. Exercise the real path via
        test/integration/test-pk-verify-qemu.sh (AXL_TLS=1).
**/

#include <axl.h>
#include "axl-test.h"
#include "../data/pk-ecdsa-p256-vector.h"

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
// Cryptographic outcomes — require a real mbedTLS (AXL_TLS=1) build.
// ---------------------------------------------------------------------------

static void
test_verify_outcomes(void)
{
#ifdef AXL_HAVE_TLS
    test_check(axl_pk_available() == true,
               "pk_available: true in AXL_TLS build");

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
#else
    /* Without AXL_TLS, verification is not compiled in: it must fail
       closed (the building block any consumer relies on for safety). */
    test_check(axl_pk_available() == false,
               "pk_available: false without AXL_TLS");

    test_check(axl_pk_verify(AXL_PK_ECDSA_P256,
                             pk_pub, pk_pub_len,
                             pk_msg, pk_msg_len,
                             pk_sig, pk_sig_len) == AXL_ERR,
               "pk_verify: valid sig -> AXL_ERR (verification not built)");
#endif /* AXL_HAVE_TLS */
}

// ---------------------------------------------------------------------------
// Key handles — keygen, serialize, sign, verify (require AXL_TLS).
// ---------------------------------------------------------------------------

static void
test_key_handle(void)
{
#ifdef AXL_HAVE_TLS
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
#else
    // Without AXL_TLS the whole key-handle API fails closed.
    test_check(axl_pk_key_new(AXL_PK_ECDSA_P256) == NULL,
               "keygen: NULL without AXL_TLS");
    test_check(axl_pk_key_load_private(pk_rsa3072_pkcs8,
                                       pk_rsa3072_pkcs8_len) == NULL,
               "load_private: NULL without AXL_TLS");
    test_check(axl_pk_key_alg(NULL) == AXL_PK_ED25519,
               "key_alg: NULL -> reserved zero");
    uint8_t s[8];
    size_t  sl = sizeof(s);
    test_check(axl_pk_key_sign(NULL, pk_msg, pk_msg_len, AXL_PK_SIG_DER,
                               s, &sl) == AXL_ERR,
               "sign: NULL key -> AXL_ERR");
#endif /* AXL_HAVE_TLS */
}

// ---------------------------------------------------------------------------
// AEAD — AES-GCM and ChaCha20-Poly1305 (require AXL_TLS).
// ---------------------------------------------------------------------------

#ifdef AXL_HAVE_TLS
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
#endif /* AXL_HAVE_TLS */

static void
test_aead(void)
{
#ifdef AXL_HAVE_TLS
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
#else
    uint8_t ct[8], tag[16];
    test_check(axl_aead_seal(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN, NULL, 0,
                             aead_pt, 8, ct, tag, 16) == AXL_ERR,
               "aead: seal fails closed without AXL_TLS");
    test_check(axl_aead_open(AXL_AEAD_AES_256_GCM, aead_key32, 32,
                             aead_nonce, AXL_AEAD_NONCE_LEN, NULL, 0,
                             ct, 8, tag, 16, ct) == AXL_ERR,
               "aead: open fails closed without AXL_TLS");
#endif /* AXL_HAVE_TLS */
}

// ---------------------------------------------------------------------------
// AES-CTR stream cipher (requires AXL_TLS).
// ---------------------------------------------------------------------------

static void
test_cipher(void)
{
#ifdef AXL_HAVE_TLS
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
#else
    uint8_t out[8];
    test_check(axl_cipher_ctr_new(AXL_CIPHER_AES_256_CTR, ctr_key32, 32, ctr_iv) == NULL,
               "cipher: new fails closed without AXL_TLS");
    test_check(axl_cipher_ctr_xcrypt(NULL, aead_pt, 8, out) == AXL_ERR,
               "cipher: xcrypt fails closed without AXL_TLS");
#endif /* AXL_HAVE_TLS */
}

// ---------------------------------------------------------------------------
// ECDH key agreement (requires AXL_TLS).
// ---------------------------------------------------------------------------

#ifdef AXL_HAVE_TLS
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
#endif

static void
test_ecdh(void)
{
#ifdef AXL_HAVE_TLS
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
#else
    uint8_t out[32];
    size_t  n = sizeof(out);
    test_check(axl_ecdh_new(AXL_ECDH_P256) == NULL,
               "ecdh: new fails closed without AXL_TLS");
    test_check(axl_ecdh_get_public(NULL, out, &n) == AXL_ERR,
               "ecdh: get_public fails closed without AXL_TLS");
#endif /* AXL_HAVE_TLS */
}

static int
test_crypto_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlCrypto");

    test_arg_validation();
    test_verify_outcomes();
    test_key_handle();
    test_aead();
    test_cipher();
    test_ecdh();

#ifndef AXL_HAVE_TLS
    /* Each function above still runs without TLS, but only to assert its
       fail-closed path -- roughly 20 assertions standing in for roughly 87.
       Name what did not run, so the footer says "SKIPPED" rather than looking
       like a clean sweep, and so TEST_REQUIRE_TLS=1 can refuse the run. */
    test_skip("crypto: ECDSA/RSA key handles — keygen, serialize, sign, verify "
              "(needs AXL_TLS=1)");
    test_skip("crypto: PK verify known-answer vectors (needs AXL_TLS=1)");
    test_skip("crypto: AEAD seal/open — AES-GCM, ChaCha20-Poly1305 (needs AXL_TLS=1)");
    test_skip("crypto: AES-CTR stream cipher (needs AXL_TLS=1)");
    test_skip("crypto: ECDH key agreement, both curves (needs AXL_TLS=1)");
#endif

    return test_print_results();
}

AXL_APP(test_crypto_main)
