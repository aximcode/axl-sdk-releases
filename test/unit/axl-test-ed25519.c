/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-ed25519.c
    Unit tests for AXL_PK_ED25519, the ref10-backed AxlPkProvider.

    A SEPARATE binary from AxlTestCrypto on purpose. Ed25519 is reached
    through a WEAK symbol and costs an image ~120 KB, so it is opt-in --
    an image carries it only if it calls axl_pk_enable_ed25519() -- and
    the seam therefore has two halves that must both be tested:

      - AxlTestCrypto never calls it, and its
        test_pk_reserved_alg_fails_closed / test_pk_raw_public_rejects
        pin that the algorithm fails CLOSED at every entry point, with
        the exact line naming the function;
      - this binary calls it, and pins that it works.

    The mbedTLS provider is the mirror image: AxlTestCrypto enables it,
    this binary does not, and test_ed25519_mbedtls_absent() pins its
    missing-provider line.

    One binary cannot assert both, and dropping either half would leave
    the seam's whole reason for existing untested. Task 6 measures the
    two images against each other for the same reason.
**/

#include <axl.h>
#include "axl-test.h"
#include "axl-pk-provider.h"
#include "axl-ed25519-glue.h"   /* the failure count and its test hooks */

// ---------------------------------------------------------------------------
// RFC 8032 §7.1 vectors.
//
// Bytes specified by someone else, so passing is evidence about the
// standard rather than about ourselves. The seeds, public keys and
// signatures are the RFC's; the DER encodings were generated and the
// TEST 2 / TEST 3 signatures independently reproduced with OpenSSL
// 3.5.5 (TEST 1's empty message cannot go through `openssl pkeyutl
// -rawin`, so its signature is the RFC's value alone).
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;        /* RFC's label, for assertion text */
    const char *priv_der;    /* PKCS#8 PrivateKeyInfo, 48 bytes */
    const char *pub_der;     /* SubjectPublicKeyInfo, 44 bytes */
    const char *pub_raw;     /* compressed point, 32 bytes */
    const char *msg;         /* message, hex; "" is the empty message */
    size_t      msg_len;     /* bytes in msg */
    const char *sig;         /* detached signature, 64 bytes */
} Ed25519Vector;

static const Ed25519Vector VECTORS[3] = {
    {   "TEST 1",
        "302e020100300506032b657004220420"
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "302a300506032b6570032100"
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "", 0,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
        "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    {   "TEST 2",
        "302e020100300506032b657004220420"
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "302a300506032b6570032100"
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72", 1,
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    {   "TEST 3",
        "302e020100300506032b657004220420"
        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        "302a300506032b6570032100"
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82", 2,
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

#define ED25519_PRIV_DER_LEN  48u
#define ED25519_PUB_DER_LEN   44u
#define ED25519_RAW_PUB_LEN   32u
#define ED25519_SIG_LEN       64u

/* A message pointer for a vector. NULL for the empty message, which is
   what axl_pk_key_sign / _verify document for msg_len == 0 -- and TEST 1
   is the only vector in the RFC that exercises that path at all. */
static const uint8_t *
vector_msg(const Ed25519Vector *v, uint8_t *buf)
{
    if (v->msg_len == 0) {
        return NULL;
    }
    return test_hex_bytes(v->msg, buf, v->msg_len) ? buf : NULL;
}

// ---------------------------------------------------------------------------
// The provider is linked and reachable.
// ---------------------------------------------------------------------------

static void
test_ed25519_available(void)
{
    /* The other half of AxlTestCrypto's fail-closed set: this binary
       calls axl_pk_enable_ed25519(), so the same queries that
       must answer "no" there must answer "yes" here. If BOTH binaries
       ever agree, the weak seam has stopped discriminating and one of
       the two test binaries is asserting nothing. */
    test_check(axl_pk_alg_available(AXL_PK_ED25519),
               "ed25519: available in an image that opted in");
    test_check(_axl_pk_provider_for(AXL_PK_ED25519) != NULL,
               "ed25519: the provider resolves");

}

// ---------------------------------------------------------------------------
// The mbedTLS provider (ECDSA + RSA) is NOT linked here.
//
// This binary is the Ed25519-only image -- the shape of the sshd driver --
// and never calls axl_pk_enable_mbedtls(). AxlTestCrypto does, and asserts
// the other half. A direct key-handle call that needs ECDSA or RSA must
// fail closed AND say, in one line, which function to call.
// ---------------------------------------------------------------------------

#define PK_ABSENT_LINE \
    "ECDSA/RSA unavailable: call axl_pk_enable_mbedtls() before using " \
    "ECDSA/RSA keys"

static size_t pk_absent_errors;
static char   pk_absent_msg[256];

static void
pk_absent_capture(int level, const char *domain, const char *message,
                  const AxlRealtime *stamp, void *data)
{
    (void)stamp;
    (void)data;
    if (level == AXL_LOG_ERROR && domain != NULL
        && axl_strcmp(domain, "pk") == 0) {
        pk_absent_errors++;
        axl_snprintf(pk_absent_msg, sizeof(pk_absent_msg), "%s", message);
    }
}

static void
pk_absent_reset(void)
{
    pk_absent_errors = 0;
    pk_absent_msg[0] = '\0';
}

/* Exactly one error line, and it is the line. */
static bool
pk_absent_logged_once(void)
{
    return pk_absent_errors == 1
        && axl_strcmp(pk_absent_msg, PK_ABSENT_LINE) == 0;
}

static void
test_ed25519_mbedtls_absent(void)
{
    /* A P-256 SubjectPublicKeyInfo (id-ecPublicKey, prime256v1). */
    static const uint8_t p256_spki[] = {
        0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d,
        0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01,
        0x07, 0x03, 0x42, 0x00, 0x04, 0xea, 0x2f, 0x91, 0x5c, 0x8f, 0xc5,
        0x0e, 0x8d, 0xf2, 0x1f, 0xb3, 0x0d, 0x1b, 0x5a, 0x65, 0x03, 0x9c,
        0xa4, 0x34, 0x2f, 0x8a, 0x0f, 0xa7, 0xf0, 0x43, 0x8c, 0x1e, 0x6c,
        0x0e, 0x18, 0x26, 0x90, 0x68, 0x0b, 0xf6, 0xa9, 0xd1, 0x2e, 0xb2,
        0x88, 0xbd, 0xb6, 0x41, 0xda, 0x2f, 0x65, 0xd6, 0xd5, 0x70, 0xc1,
        0x61, 0x36, 0xa2, 0x3f, 0xad, 0xc8, 0x30, 0xd8, 0x4d, 0xda, 0xc0,
        0x5c, 0x28, 0x79,
    };
    /* The head of an RSA SubjectPublicKeyInfo: SEQUENCE, AlgorithmIdentifier
       { rsaEncryption, NULL }. Truncated -- it only has to NAME RSA. */
    static const uint8_t rsa_spki_head[] = {
        0x30, 0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48,
        0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00,
    };

    test_check(!axl_pk_alg_available(AXL_PK_ECDSA_P256)
               && !axl_pk_alg_available(AXL_PK_ECDSA_P384)
               && !axl_pk_alg_available(AXL_PK_RSA),
               "mbedtls absent: ECDSA P-256 / P-384 / RSA all report unavailable");
    test_check(_axl_pk_provider_for(AXL_PK_ECDSA_P256) == NULL
               && _axl_pk_provider_for(AXL_PK_RSA) == NULL,
               "mbedtls absent: the provider does not resolve");

    test_check(axl_log_add_handler(pk_absent_capture, NULL) == AXL_OK,
               "mbedtls absent: capture handler installed");

    /* The query itself is silent: it is how a caller asks. */
    pk_absent_reset();
    test_check(!axl_pk_alg_available(AXL_PK_RSA) && pk_absent_errors == 0,
               "mbedtls absent: axl_pk_alg_available logs nothing");

    pk_absent_reset();
    test_check(axl_pk_key_new(AXL_PK_ECDSA_P256) == NULL,
               "mbedtls absent: axl_pk_key_new(P-256) -> NULL");
    test_check(pk_absent_logged_once(),
               "mbedtls absent: key_new(P-256) logs the exact enable line once");

    pk_absent_reset();
    test_check(axl_pk_key_new(AXL_PK_RSA) == NULL,
               "mbedtls absent: axl_pk_key_new(RSA) -> NULL");
    test_check(pk_absent_logged_once(),
               "mbedtls absent: key_new(RSA) logs the exact enable line once");

    pk_absent_reset();
    test_check(axl_pk_key_from_raw_public(AXL_PK_ECDSA_P256,
                                          p256_spki + 26, 65) == NULL,
               "mbedtls absent: from_raw_public(P-256) -> NULL");
    test_check(pk_absent_logged_once(),
               "mbedtls absent: from_raw_public(P-256) logs the exact enable line once");

    pk_absent_reset();
    test_check(axl_pk_key_load_public(p256_spki, sizeof(p256_spki)) == NULL,
               "mbedtls absent: load_public(P-256 SPKI) -> NULL");
    test_check(pk_absent_logged_once(),
               "mbedtls absent: load_public(P-256 SPKI) logs the exact enable line once");

    pk_absent_reset();
    test_check(axl_pk_key_load_public(rsa_spki_head,
                                      sizeof(rsa_spki_head)) == NULL,
               "mbedtls absent: load_public(RSA SPKI) -> NULL");
    test_check(pk_absent_logged_once(),
               "mbedtls absent: load_public(RSA SPKI) logs the exact enable line once");

    /* No false positive: DER that names neither algorithm -- a malformed
       Ed25519 SPKI here -- fails as ordinary bad input, without blaming
       a missing provider. */
    uint8_t bad[ED25519_PUB_DER_LEN];
    test_check(test_hex_bytes(VECTORS[0].pub_der, bad, sizeof(bad)),
               "mbedtls absent: decode the Ed25519 SPKI");
    bad[sizeof(bad) - 1] ^= 0x01;
    bad[1] = 0x7f;  /* wrong outer length: unparseable */
    pk_absent_reset();
    test_check(axl_pk_key_load_public(bad, sizeof(bad)) == NULL
               && pk_absent_errors == 0,
               "mbedtls absent: bad Ed25519 DER logs no provider line");

    /* Control: Ed25519 still works through the same entry point, and
       says nothing. */
    test_check(test_hex_bytes(VECTORS[0].pub_der, bad, sizeof(bad)),
               "mbedtls absent: decode the Ed25519 SPKI again");
    pk_absent_reset();
    AxlPkKey *ok = axl_pk_key_load_public(bad, sizeof(bad));
    test_check(ok != NULL && pk_absent_errors == 0,
               "mbedtls absent: control -- Ed25519 SPKI loads, silently");
    axl_pk_key_free(ok);

    axl_log_remove_handler(pk_absent_capture);
}

// ---------------------------------------------------------------------------
// RFC 8032 positive vectors, driven through the public API.
// ---------------------------------------------------------------------------

static void
test_ed25519_rfc8032_vectors(void)
{
    for (size_t i = 0; i < 3; i++) {
        const Ed25519Vector *v = &VECTORS[i];
        char                 what[64];

        uint8_t der[ED25519_PRIV_DER_LEN];
        test_check(test_hex_bytes(v->priv_der, der, sizeof(der)),
                   "ed25519 rfc8032: the private DER vector is well-formed");

        AxlPkKey *k = axl_pk_key_load_private(der, sizeof(der));
        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s imports its PKCS#8 seed", v->name);
        test_check(k != NULL, what);
        if (k == NULL) {
            continue;
        }

        /* The dispatcher classified these bytes by asking each provider
           in turn; nothing told it the algorithm. That it lands on
           AXL_PK_ED25519 is E2a's der_import_any doing its job. */
        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s classifies as Ed25519", v->name);
        test_check(axl_pk_key_alg(k) == AXL_PK_ED25519, what);

        uint8_t raw[ED25519_RAW_PUB_LEN];
        size_t  rlen = sizeof(raw);
        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s exports its public key", v->name);
        test_check(axl_pk_key_get_raw_public(k, raw, &rlen) == AXL_OK
                       && rlen == ED25519_RAW_PUB_LEN
                       && test_hex_equals(raw, rlen, v->pub_raw),
                   what);

        uint8_t        msg_buf[8];
        const uint8_t *msg = vector_msg(v, msg_buf);

        uint8_t sig[ED25519_SIG_LEN];
        size_t  slen = sizeof(sig);
        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s signature", v->name);
        test_check(axl_pk_key_sign(k, msg, v->msg_len, AXL_PK_SIG_RAW,
                                   sig, &slen) == AXL_OK
                       && slen == ED25519_SIG_LEN
                       && test_hex_equals(sig, slen, v->sig),
                   what);

        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s verifies its own signature",
                     v->name);
        test_check(axl_pk_key_verify(k, msg, v->msg_len, AXL_PK_SIG_RAW,
                                     sig, slen) == AXL_OK,
                   what);

        /* A public-only handle built from the raw point must verify the
           same signature -- the path AxlSsh's authorized-keys check
           takes, which never sees a seed. */
        AxlPkKey *pub = axl_pk_key_from_raw_public(AXL_PK_ED25519, raw,
                                                   ED25519_RAW_PUB_LEN);
        axl_snprintf(what, sizeof(what),
                     "ed25519 rfc8032: %s verifies under a raw public key",
                     v->name);
        test_check(pub != NULL
                       && axl_pk_key_verify(pub, msg, v->msg_len,
                                            AXL_PK_SIG_RAW, sig, slen)
                              == AXL_OK,
                   what);
        axl_pk_key_free(pub);
        axl_pk_key_free(k);
    }
}

/* RFC 8032 section 7.1, "TEST 1024" -- the RFC's longest Ed25519 vector,
   copied out of the RFC text (rfc-editor.org/rfc/rfc8032.txt) by script,
   not retyped, and cross-checked before it went in: OpenSSL 3.5.5
   (`openssl pkeyutl -sign -rawin`) reproduces this public key and this
   signature from this seed and message. It is here because it is the
   only vector in the tree whose signing hashes span many SHA-512 blocks
   (the nonce hashes 32 + 1023 bytes, the challenge 64 + 1023): TEST 1-3
   hash at most 66 bytes, and SSH signs a 32-byte exchange hash, so
   without it nothing checks the multi-block path against another
   implementation. */
#define T1024_MSG_LEN  1023u
static const char T1024_SEED[] =
    "f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5";
static const char T1024_PUB[] =
    "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e";
static const char T1024_MSG[] =
    "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
    "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
    "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
    "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
    "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
    "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
    "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
    "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
    "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
    "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
    "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
    "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
    "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
    "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
    "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
    "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
    "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
    "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
    "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
    "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
    "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
    "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
    "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
    "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
    "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
    "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
    "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
    "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
    "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
    "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
    "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
    "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0";
static const char T1024_SIG[] =
    "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
    "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03";

static void
test_ed25519_rfc8032_test1024(void)
{
    static uint8_t msg[T1024_MSG_LEN];
    uint8_t        der[ED25519_PRIV_DER_LEN];
    char           der_hex[2 * ED25519_PRIV_DER_LEN + 1];
    axl_snprintf(der_hex, sizeof(der_hex), "%s%s",
                 "302e020100300506032b657004220420", T1024_SEED);
    test_check(axl_strlen(T1024_MSG) == 2 * T1024_MSG_LEN
                   && test_hex_bytes(T1024_MSG, msg, sizeof(msg))
                   && test_hex_bytes(der_hex, der, sizeof(der)),
               "ed25519 rfc8032: TEST 1024's vectors are well-formed");

    AxlPkKey *k = axl_pk_key_load_private(der, sizeof(der));
    test_check(k != NULL, "ed25519 rfc8032: TEST 1024 imports its seed");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. */
        test_skip_n(3, "TEST 1024's key did not import");
        return;
    }
    uint8_t raw[ED25519_RAW_PUB_LEN];
    size_t  rlen = sizeof(raw);
    test_check(axl_pk_key_get_raw_public(k, raw, &rlen) == AXL_OK
                   && rlen == sizeof(raw)
                   && test_hex_equals(raw, rlen, T1024_PUB),
               "ed25519 rfc8032: TEST 1024 derives the RFC's public key");

    uint8_t sig[ED25519_SIG_LEN];
    size_t  slen = sizeof(sig);
    test_check(axl_pk_key_sign(k, msg, sizeof(msg), AXL_PK_SIG_RAW, sig,
                               &slen) == AXL_OK
                   && slen == sizeof(sig)
                   && test_hex_equals(sig, slen, T1024_SIG),
               "ed25519 rfc8032: TEST 1024 signature (a 1023-byte message)");

    AxlPkKey *pub = axl_pk_key_from_raw_public(AXL_PK_ED25519, raw,
                                               sizeof(raw));
    test_check(pub != NULL
                   && axl_pk_key_verify(pub, msg, sizeof(msg),
                                        AXL_PK_SIG_RAW, sig, slen) == AXL_OK,
               "ed25519 rfc8032: TEST 1024 verifies under a raw public key");
    axl_pk_key_free(pub);
    axl_pk_key_free(k);
}

// ---------------------------------------------------------------------------
// RFC 8410 DER, both directions.
// ---------------------------------------------------------------------------

static void
test_ed25519_der_roundtrip(void)
{
    const Ed25519Vector *v = &VECTORS[0];

    uint8_t priv[ED25519_PRIV_DER_LEN];
    uint8_t pub[ED25519_PUB_DER_LEN];
    test_check(test_hex_bytes(v->priv_der, priv, sizeof(priv))
                   && test_hex_bytes(v->pub_der, pub, sizeof(pub)),
               "ed25519 der: both DER vectors are well-formed");

    AxlPkKey *k = axl_pk_key_load_private(priv, sizeof(priv));
    test_check(k != NULL, "ed25519 der: PKCS#8 import");
    if (k == NULL) {
        return;
    }

    /* Size query first -- the output-buffer protocol the whole AxlPk
       API shares. Both encodings are fixed-length, so unlike ECDSA DER
       these are exact sizes rather than upper bounds. */
    size_t need = 0;
    test_check(axl_pk_key_get_private_der(k, NULL, &need) == AXL_OK
                   && need == ED25519_PRIV_DER_LEN,
               "ed25519 der: private size query reports exactly 48");

    uint8_t out[ED25519_PRIV_DER_LEN];
    size_t  olen = sizeof(out);
    test_check(axl_pk_key_get_private_der(k, out, &olen) == AXL_OK
                   && olen == ED25519_PRIV_DER_LEN
                   && test_hex_equals(out, olen, v->priv_der),
               "ed25519 der: private export reproduces the vector byte for byte");

    need = 0;
    test_check(axl_pk_key_get_public_der(k, NULL, &need) == AXL_OK
                   && need == ED25519_PUB_DER_LEN,
               "ed25519 der: public size query reports exactly 44");

    uint8_t pout[ED25519_PUB_DER_LEN];
    size_t  plen = sizeof(pout);
    test_check(axl_pk_key_get_public_der(k, pout, &plen) == AXL_OK
                   && plen == ED25519_PUB_DER_LEN
                   && test_hex_equals(pout, plen, v->pub_der),
               "ed25519 der: public export reproduces the vector byte for byte");

    /* A one-byte-short buffer must report the size it needs and leave
       the caller's bytes alone, not write a truncated encoding. */
    uint8_t small[ED25519_PUB_DER_LEN];
    axl_memset(small, 0xAA, sizeof(small));
    size_t slen = ED25519_PUB_DER_LEN - 1;
    test_check(axl_pk_key_get_public_der(k, small, &slen) == AXL_ERR
                   && slen == ED25519_PUB_DER_LEN
                   && small[0] == 0xAA,
               "ed25519 der: a short buffer reports the size and writes nothing");

    axl_pk_key_free(k);

    /* SPKI import, and the resulting handle verifies but cannot sign. */
    AxlPkKey *pk = axl_pk_key_load_public(pub, sizeof(pub));
    test_check(pk != NULL, "ed25519 der: SPKI import");
    if (pk == NULL) {
        return;
    }
    test_check(axl_pk_key_alg(pk) == AXL_PK_ED25519,
               "ed25519 der: SPKI classifies as Ed25519");

    uint8_t raw[ED25519_RAW_PUB_LEN];
    size_t  rlen = sizeof(raw);
    test_check(axl_pk_key_get_raw_public(pk, raw, &rlen) == AXL_OK
                   && test_hex_equals(raw, rlen, v->pub_raw),
               "ed25519 der: SPKI carries the vector's public key");

    size_t need_priv = 0;
    test_check(axl_pk_key_get_private_der(pk, NULL, &need_priv) == AXL_ERR,
               "ed25519 der: a public-only handle has no private DER");

    uint8_t sig[ED25519_SIG_LEN];
    size_t  siglen = sizeof(sig);
    test_check(axl_pk_key_sign(pk, NULL, 0, AXL_PK_SIG_RAW, sig, &siglen)
                   == AXL_ERR,
               "ed25519 der: a public-only handle cannot sign");

    axl_pk_key_free(pk);
}

// ---------------------------------------------------------------------------
// DER rejections. Every case is a DISTINCT malformation -- five variants
// that the same length check catches would be one assertion wearing five
// names.
// ---------------------------------------------------------------------------

static void
test_ed25519_der_rejects(void)
{
    const Ed25519Vector *v = &VECTORS[0];
    uint8_t              good_pub[ED25519_PUB_DER_LEN];
    uint8_t              good_priv[ED25519_PRIV_DER_LEN];
    test_check(test_hex_bytes(v->pub_der, good_pub, sizeof(good_pub))
                   && test_hex_bytes(v->priv_der, good_priv,
                                     sizeof(good_priv)),
               "ed25519 der reject: the control vectors are well-formed");

    /* Control: unmodified, both import. Without this every rejection
       below could be passing because the whole path is broken. */
    AxlPkKey *ok = axl_pk_key_load_public(good_pub, sizeof(good_pub));
    test_check(ok != NULL, "ed25519 der reject: control -- the good SPKI imports");
    axl_pk_key_free(ok);

    uint8_t bad[ED25519_PUB_DER_LEN];

    /* 1. Wrong OID: 1.3.101.113 (Ed448) instead of 1.3.101.112. Same
          length, same structure -- only the algorithm differs, which is
          the case der_import_any must treat as "not mine" rather than
          as an error. */
    axl_memcpy(bad, good_pub, sizeof(bad));
    bad[8] = 0x71;
    test_check(axl_pk_key_load_public(bad, sizeof(bad)) == NULL,
               "ed25519 der reject: Ed448's OID is not ours");

    /* 2. Wrong total length: 43 valid bytes. Structure intact, one byte
          short. */
    test_check(axl_pk_key_load_public(good_pub, sizeof(good_pub) - 1)
                   == NULL,
               "ed25519 der reject: a 43-byte SPKI");

    /* 3. Wrong outer tag: SET (0x31) where SEQUENCE (0x30) belongs. */
    axl_memcpy(bad, good_pub, sizeof(bad));
    bad[0] = 0x31;
    test_check(axl_pk_key_load_public(bad, sizeof(bad)) == NULL,
               "ed25519 der reject: a SET where a SEQUENCE belongs");

    /* 4. Non-zero BIT STRING unused-bits byte. The one field a template
          matcher is most likely to skip over, because nothing downstream
          reads it -- and a 33rd byte of key material is exactly what an
          attacker would put there. */
    axl_memcpy(bad, good_pub, sizeof(bad));
    bad[11] = 0x03;
    test_check(axl_pk_key_load_public(bad, sizeof(bad)) == NULL,
               "ed25519 der reject: a non-zero BIT STRING unused-bits byte");

    /* 5. A private key offered to the PUBLIC loader. Well-formed DER of
          the right algorithm, wrong structure -- so this is not caught
          by any OID or length check the public template applies. */
    test_check(axl_pk_key_load_public(good_priv, sizeof(good_priv)) == NULL,
               "ed25519 der reject: PKCS#8 is not a SubjectPublicKeyInfo");

    /* 6. And the inverse: SPKI offered to the private loader. */
    test_check(axl_pk_key_load_private(good_pub, sizeof(good_pub)) == NULL,
               "ed25519 der reject: SPKI is not a PKCS#8 PrivateKeyInfo");

    /* 7. PKCS#8 with a non-zero version. The RFC 8410 form pins version
          0; a v1 PrivateKeyInfo carries an attributes field this
          template does not parse. */
    uint8_t bad_priv[ED25519_PRIV_DER_LEN];
    axl_memcpy(bad_priv, good_priv, sizeof(bad_priv));
    bad_priv[4] = 0x01;
    test_check(axl_pk_key_load_private(bad_priv, sizeof(bad_priv)) == NULL,
               "ed25519 der reject: a non-zero PKCS#8 version");

    /* 8. The inner CurvePrivateKey OCTET STRING mis-tagged. The outer
          04 22 wrapper is intact, so only a matcher that checks BOTH
          layers catches it. */
    axl_memcpy(bad_priv, good_priv, sizeof(bad_priv));
    bad_priv[14] = 0x05;
    test_check(axl_pk_key_load_private(bad_priv, sizeof(bad_priv)) == NULL,
               "ed25519 der reject: a mis-tagged inner CurvePrivateKey");
}

// ---------------------------------------------------------------------------
// Raw public keys.
// ---------------------------------------------------------------------------

static void
test_ed25519_raw_public(void)
{
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              raw[ED25519_RAW_PUB_LEN];
    test_check(test_hex_bytes(v->pub_raw, raw, sizeof(raw)),
               "ed25519 raw: the vector is well-formed");

    AxlPkKey *k = axl_pk_key_from_raw_public(AXL_PK_ED25519, raw,
                                             sizeof(raw));
    test_check(k != NULL, "ed25519 raw: 32-byte import");
    if (k == NULL) {
        return;
    }

    uint8_t out[ED25519_RAW_PUB_LEN];
    size_t  olen = sizeof(out);
    test_check(axl_pk_key_get_raw_public(k, out, &olen) == AXL_OK
                   && olen == ED25519_RAW_PUB_LEN
                   && test_hex_equals(out, olen, v->pub_raw),
               "ed25519 raw: export round-trips the vector");

    size_t need = 0;
    test_check(axl_pk_key_get_raw_public(k, NULL, &need) == AXL_OK
                   && need == ED25519_RAW_PUB_LEN,
               "ed25519 raw: size query reports exactly 32");
    axl_pk_key_free(k);

    /* Wrong lengths. 31 and 33 bracket the only legal size; 65 is
       P-256's SEC1 length, the mistake a caller is most likely to
       actually make. */
    test_check(axl_pk_key_from_raw_public(AXL_PK_ED25519, raw, 31) == NULL,
               "ed25519 raw: refuses 31 bytes");
    test_check(axl_pk_key_from_raw_public(AXL_PK_ED25519, raw, 33) == NULL,
               "ed25519 raw: refuses 33 bytes");
    test_check(axl_pk_key_from_raw_public(AXL_PK_ED25519, raw, 65) == NULL,
               "ed25519 raw: refuses a SEC1-length input");
}

// ---------------------------------------------------------------------------
// Key generation.
// ---------------------------------------------------------------------------

static void
test_ed25519_keygen(void)
{
    AxlPkKey *a = axl_pk_key_new(AXL_PK_ED25519);
    AxlPkKey *b = axl_pk_key_new(AXL_PK_ED25519);
    test_check(a != NULL && b != NULL, "ed25519 keygen: two keys generate");
    if (a == NULL || b == NULL) {
        axl_pk_key_free(a);
        axl_pk_key_free(b);
        return;
    }

    uint8_t pa[ED25519_RAW_PUB_LEN];
    uint8_t pb[ED25519_RAW_PUB_LEN];
    size_t  la = sizeof(pa);
    size_t  lb = sizeof(pb);
    test_check(axl_pk_key_get_raw_public(a, pa, &la) == AXL_OK
                   && axl_pk_key_get_raw_public(b, pb, &lb) == AXL_OK,
               "ed25519 keygen: both export a public key");
    /* Two draws from the same RNG colliding would mean the RNG is
       returning a constant -- which is exactly what the glue's
       failure path deliberately produces, so this is the assertion
       that would catch it. */
    test_check(axl_memcmp(pa, pb, sizeof(pa)) != 0,
               "ed25519 keygen: two keys differ");

    static const uint8_t msg[5] = { 'h', 'e', 'l', 'l', 'o' };
    uint8_t              sig[ED25519_SIG_LEN];
    size_t               slen = sizeof(sig);
    test_check(axl_pk_key_sign(a, msg, sizeof(msg), AXL_PK_SIG_RAW,
                               sig, &slen) == AXL_OK
                   && slen == ED25519_SIG_LEN,
               "ed25519 keygen: a generated key signs");
    test_check(axl_pk_key_verify(a, msg, sizeof(msg), AXL_PK_SIG_RAW,
                                 sig, slen) == AXL_OK,
               "ed25519 keygen: and verifies its own signature");
    test_check(axl_pk_key_verify(b, msg, sizeof(msg), AXL_PK_SIG_RAW,
                                 sig, slen) == AXL_ERR,
               "ed25519 keygen: the other key does not verify it");

    /* A DER round-trip through a generated key, which is the shape
       AxlSsh's host-key persistence takes: generate once, store the
       PKCS#8, load it back on the next boot. */
    uint8_t der[ED25519_PRIV_DER_LEN];
    size_t  dlen = sizeof(der);
    test_check(axl_pk_key_get_private_der(a, der, &dlen) == AXL_OK
                   && dlen == ED25519_PRIV_DER_LEN,
               "ed25519 keygen: exports PKCS#8");
    AxlPkKey *reloaded = axl_pk_key_load_private(der, dlen);
    test_check(reloaded != NULL, "ed25519 keygen: the PKCS#8 loads back");
    if (reloaded != NULL) {
        test_check(axl_pk_key_verify(reloaded, msg, sizeof(msg),
                                     AXL_PK_SIG_RAW, sig, slen) == AXL_OK,
                   "ed25519 keygen: the reloaded key verifies the original signature");
    }
    axl_pk_key_free(reloaded);
    axl_pk_key_free(b);
    axl_pk_key_free(a);
}

// ---------------------------------------------------------------------------
// The refusals.
//
// A verifier that passes every RFC 8032 positive vector can still be
// broken. The vendored ref10 already implements each negative check --
// canonical S, the high bits of S, a non-canonical or small-order public
// key -- so what these tests prove is that each is REACHABLE through our
// wrapper, which is a different claim and the one that matters.
//
// crypto_sign_ed25519_open opens with, in order:
//     if (smlen < 64)                       goto badsig;
//     if (sm[63] & 224)                     goto badsig;   high bits of S
//     if (sc25519_inrange(sm+32))           goto badsig;   S >= L
//     if (ge25519_unpackneg_vartime(&get1,pk)) goto badsig; non-canonical pk
//     if (ge25519_isneutral_vartime(&get1)) goto badsig;   small-order pk
// ---------------------------------------------------------------------------

static void
test_ed25519_refusals(void)
{
    /* RFC 8032 TEST 2's key and message throughout. */
    const Ed25519Vector *v = &VECTORS[1];

    /* Same R; S replaced by S + L. Verified independently: S + L is
       7259498856289251509293018044029001091963178266749712260501067738332268015093,
       it still fits in 32 bytes, and byte 63 is 0x10 -- so its high bits
       are CLEAR and it does not trip the `sm[63] & 224` guard. Only
       sc25519_inrange can reject it, which is what makes it a clean
       probe for that one check rather than an input two guards catch. */
    static const char *MAL =
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "f52db7415978abc61b2c2eb6aeebfca0387b2eaeb4302aeeb00d291612bb0c10";
    /* The good signature with the low bit of byte 0 -- inside R -- flipped. */
    static const char *FLIP =
        "93a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
    /* The good signature with the top three bits of S set.
       ref10 guards this with `sm[63] & 224` -- but that guard is DEAD
       CODE behind OpenSSH's later sc25519_inrange: L's top byte is
       0x10, so any S with bits 0xE0 set is necessarily >= L and
       sc25519_inrange rejects it first. Verified by sabotage: deleting
       `sm[63] & 224` leaves the whole suite green. So this vector pins
       that such a signature is refused, NOT which guard refuses it. */
    static const char *HIGHBITS =
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0ce0";

    uint8_t pub[ED25519_RAW_PUB_LEN];
    uint8_t good[ED25519_SIG_LEN];
    uint8_t mal[ED25519_SIG_LEN];
    uint8_t flip[ED25519_SIG_LEN];
    uint8_t high[ED25519_SIG_LEN];
    test_check(test_hex_bytes(v->pub_raw, pub, sizeof(pub))
                   && test_hex_bytes(v->sig, good, sizeof(good))
                   && test_hex_bytes(MAL, mal, sizeof(mal))
                   && test_hex_bytes(FLIP, flip, sizeof(flip))
                   && test_hex_bytes(HIGHBITS, high, sizeof(high)),
               "ed25519 refuse: every vector is well-formed");

    static const uint8_t msg2[1]  = { 0x72 };
    static const uint8_t other[1] = { 0x73 };

    AxlPkKey *k = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                             sizeof(pub));
    test_check(k != NULL, "ed25519 refuse: import TEST 2's public key");
    if (k == NULL) {
        return;
    }

    /* CONTROL FIRST. Without it every refusal below could be passing
       because the verify path is broken outright rather than because
       the check fired. */
    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, good,
                                 ED25519_SIG_LEN) == AXL_OK,
               "ed25519 refuse: control -- the good signature verifies");

    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, mal,
                                 ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 refuse: S >= L");
    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, high,
                                 ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 refuse: a signature with the top bits of S set");
    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, flip,
                                 ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 refuse: a flipped bit in R");
    test_check(axl_pk_key_verify(k, other, 1, AXL_PK_SIG_RAW, good,
                                 ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 refuse: a good signature over the wrong message");

    /* Wrong signature lengths. These are refused by THIS provider before
       the vendored code sees them, and deliberately so: 63 bytes with a
       1-byte message would otherwise build a 64-byte sm whose last byte
       is message data reinterpreted as the top of S -- an input ref10's
       own `smlen < 64` guard cannot catch, because smlen would be 64. */
    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, good, 63)
                   == AXL_ERR,
               "ed25519 refuse: a 63-byte signature");
    test_check(axl_pk_key_verify(k, msg2, 1, AXL_PK_SIG_RAW, good, 65)
                   == AXL_ERR,
               "ed25519 refuse: a 65-byte signature");
    axl_pk_key_free(k);
}

/* Small-order public keys, and the forgeries they enable.

   THE FORGERY. Under a public key A whose order divides 2, verification
   reduces to [S]B == R + [h]A with [h]A the identity, so choosing S = 1
   and R = encode(B) yields a signature that verifies with NO private
   key: for every message when A is the identity, and for every message
   whose challenge h is even when A has order 2. Its S is 1 -- in range,
   top bits clear -- so neither sc25519_inrange nor `sm[63] & 224` can
   catch it. Constructed and checked against an independent Python
   implementation of RFC 8032's verification equation before being
   pinned here.

   BOTH CASES WERE MEASURED AGAINST THIS PROVIDER BEFORE THE FIX, and
   they answered differently: the vendored ref10 refused the identity
   forgery (its ge25519_isneutral_vartime) and ACCEPTED the order-2 one,
   because the identity is the only small-order point it checks for.
   That measurement is why ed25519_is_small_order() exists.

   Sabotage now moves these as ONE group, not two, and that is the
   expected result rather than a gap: our table contains the identity
   as well, so it is strictly outside ref10's check. Deleting
   ED25519_SMALL_ORDER's use fails the order-2 and torsion assertions;
   deleting ge25519_isneutral_vartime's branch fails NOTHING, because
   our layer already refused the key before ref10 saw it. Ours is the
   outer door, and the vendored guard is now defence in depth behind it.

   The order-8 keys get the same treatment; no forgery is constructed
   for them, because [h]A is the identity only when h is a multiple of
   8 and the search is 8x longer for no additional claim -- the
   assertion that matters, that a small-order key never verifies
   anything, is the same one. */
/* The sm/m sizing guard.

   64 + msg_len is computed in size_t at both SM-adapter sites, and
   msg_len is caller-controlled with nothing between the public API and
   there to bound it. The top of size_t is exactly where a
   `end - start` pointer or length underflow lands, which matters
   because the first consumer parses length-prefixed network fields.

   THE FIRST VERSION OF THIS TEST HUNG THE BINARY, and that is worth
   recording because the reasoning behind it was the confident kind. It
   asserted that msg_len == SIZE_MAX - 64 was "safe to run" since
   n == SIZE_MAX exactly does not wrap and the allocator would refuse
   it. The allocator does not refuse it: axl_malloc's align_up() rounds
   to a size_t boundary and itself wraps to 0 above SIZE_MAX - 7, so
   axl_malloc(SIZE_MAX) returns a HEADER-SIZED buffer and reports
   success -- after which the copy corrupted the heap and the binary
   stopped printing, taking the run from 50s to 122s. The guard is now
   SIZE_MAX/2, well below anything either layer can mishandle.

   Every value below is refused BEFORE any allocation, so nothing here
   attempts a large malloc and the message pointer is never
   dereferenced. Deliberately no case on the accepted side of the
   boundary: proving the guard lets SIZE_MAX/2 through would mean
   actually asking for that allocation. */
static void
test_ed25519_length_overflow(void)
{
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              pub[ED25519_RAW_PUB_LEN];
    uint8_t              sig[ED25519_SIG_LEN];
    test_check(test_hex_bytes(v->pub_raw, pub, sizeof(pub))
                   && test_hex_bytes(v->sig, sig, sizeof(sig)),
               "ed25519 overflow: the control vectors are well-formed");

    AxlPkKey *k = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                             sizeof(pub));
    test_check(k != NULL, "ed25519 overflow: import the control key");
    if (k == NULL) {
        return;
    }
    /* A valid, non-NULL pointer, so the dispatcher's NULL-with-nonzero-
       length check cannot be what refuses these. */
    static const uint8_t dummy[1] = { 0x72 };

    test_check(axl_pk_key_verify(k, dummy, SIZE_MAX / 2 + 1,
                                 AXL_PK_SIG_RAW, sig, ED25519_SIG_LEN)
                   == AXL_ERR,
               "ed25519 overflow: verify refuses the smallest over-bound length");
    test_check(axl_pk_key_verify(k, dummy, SIZE_MAX - ED25519_SIG_LEN,
                                 AXL_PK_SIG_RAW, sig, ED25519_SIG_LEN)
                   == AXL_ERR,
               "ed25519 overflow: verify refuses the length that wraps axl_malloc");
    test_check(axl_pk_key_verify(k, dummy, SIZE_MAX, AXL_PK_SIG_RAW,
                                 sig, ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 overflow: verify refuses SIZE_MAX");
    /* The control: a normal length still verifies, so the guard is not
       simply refusing everything. */
    test_check(axl_pk_key_verify(k, dummy, 1, AXL_PK_SIG_RAW, sig,
                                 ED25519_SIG_LEN) == AXL_OK,
               "ed25519 overflow: control -- a 1-byte message still verifies");
    axl_pk_key_free(k);

    /* And the signing side, which has the same arithmetic. */
    uint8_t der[ED25519_PRIV_DER_LEN];
    test_check(test_hex_bytes(VECTORS[0].priv_der, der, sizeof(der)),
               "ed25519 overflow: the private DER vector is well-formed");
    AxlPkKey *priv = axl_pk_key_load_private(der, sizeof(der));
    test_check(priv != NULL, "ed25519 overflow: import a signing key");
    if (priv != NULL) {
        uint8_t out[ED25519_SIG_LEN];
        size_t  olen = sizeof(out);
        test_check(axl_pk_key_sign(priv, dummy, SIZE_MAX, AXL_PK_SIG_RAW,
                                   out, &olen) == AXL_ERR,
                   "ed25519 overflow: sign refuses SIZE_MAX");
        olen = sizeof(out);
        test_check(axl_pk_key_sign(priv, dummy, SIZE_MAX / 2 + 1,
                                   AXL_PK_SIG_RAW, out, &olen) == AXL_ERR,
                   "ed25519 overflow: sign refuses the smallest over-bound length");
    }
    axl_pk_key_free(priv);
}

/* An allocation that fails INSIDE verify is not a verdict on the signature.
   ed25519_verify builds ref10's signature-then-message buffer and an output
   buffer the same size, one axl_malloc each, before the signature is
   examined at all -- and it used to report either failing as AXL_ERR, the
   same value as a forgery. A caller that treats a bad signature as an
   attack (AxlSsh charges a forgery penalty and writes it to the audit log)
   then accused its peer of this machine's memory. Both allocations are
   pinned, so a fix that covers only the first cannot pass. */
static void
test_ed25519_verify_oom(void)
{
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              pub[ED25519_RAW_PUB_LEN];
    uint8_t              sig[ED25519_SIG_LEN];
    uint8_t              msg[1];
    test_check(test_hex_bytes(v->pub_raw, pub, sizeof(pub))
                   && test_hex_bytes(v->sig, sig, sizeof(sig))
                   && v->msg_len == sizeof(msg)
                   && test_hex_bytes(v->msg, msg, sizeof(msg)),
               "ed25519 oom: the control vectors are well-formed");

    AxlPkKey *k = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                             sizeof(pub));
    test_check(k != NULL, "ed25519 oom: import the control key");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. */
        test_skip_n(4, "no Ed25519 key to verify with");
        return;
    }

    axl_mem_fail_next_alloc(1);
    int rc = axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW, sig,
                               sizeof(sig));
    axl_mem_fail_next_alloc(0);
    test_check(rc == AXL_NO_RESOURCES,
               "ed25519 oom: the first buffer failing is AXL_NO_RESOURCES, not a verdict");

    axl_mem_fail_next_alloc(2);
    rc = axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW, sig,
                           sizeof(sig));
    axl_mem_fail_next_alloc(0);
    test_check(rc == AXL_NO_RESOURCES,
               "ed25519 oom: the second buffer failing is AXL_NO_RESOURCES too");

    /* The controls: with memory, the same call verifies, and a bad
       signature is still the verdict AXL_ERR -- so the code above is the
       allocation's, not something every call now returns. */
    test_check(axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW, sig,
                                 sizeof(sig)) == AXL_OK,
               "ed25519 oom: control -- with memory the signature verifies");
    uint8_t bad[ED25519_SIG_LEN];
    axl_memcpy(bad, sig, sizeof(bad));
    bad[0] ^= 0x01;
    test_check(axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW, bad,
                                 sizeof(bad)) == AXL_ERR,
               "ed25519 oom: control -- a bad signature is still AXL_ERR");
    axl_pk_key_free(k);
}

/* THE WHOLE of each operation, not only the buffers ed25519_verify and
   ed25519_sign allocate themselves: fail the 1st, 2nd, ... allocation the
   call makes until it makes none that fails. ref10 hashes through
   crypto_hash_sha512 (axl-ed25519-glue.c) and IGNORES the return at every
   call site, and the glue's hash used to allocate -- so an allocation
   failing in there reached ref10 as an all-zero digest. That is far worse
   than a refused check: at the nonce it made a signature that VERIFIES
   and discloses the private scalar (a = S * h^-1 mod L), and at the
   challenge it let R = encode(B), S = 1 verify under any key; see
   test_ed25519_hash_failure_refused, which pins both consequences
   directly. An allocation sweep of AxlSsh's signature check, one layer up,
   found the allocation. These sweeps now hold that the hash allocates
   nothing at all. */
static void
test_ed25519_oom_sweeps(void)
{
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              der[ED25519_PRIV_DER_LEN];
    uint8_t              sig[ED25519_SIG_LEN];
    uint8_t              msg[1];
    test_check(test_hex_bytes(v->priv_der, der, sizeof(der))
                   && test_hex_bytes(v->sig, sig, sizeof(sig))
                   && v->msg_len == sizeof(msg)
                   && test_hex_bytes(v->msg, msg, sizeof(msg)),
               "ed25519 oom sweep: the control vectors are well-formed");
    AxlPkKey *k = axl_pk_key_load_private(der, sizeof(der));
    test_check(k != NULL, "ed25519 oom sweep: import the signing key");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. */
        test_skip_n(2, "no Ed25519 key to sweep with");
        return;
    }

    /* Every result short of AXL_OK must be AXL_NO_RESOURCES -- "could not
       check" -- never the verdict AXL_ERR. */
    bool     verified = false;
    unsigned verdicts = 0;
    for (size_t n = 1; n <= 32; n++) {
        axl_mem_fail_next_alloc(n);
        int rc = axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW, sig,
                                   sizeof(sig));
        axl_mem_fail_next_alloc(0);
        if (rc == AXL_OK) {
            verified = true;
            break;
        }
        if (rc != AXL_NO_RESOURCES) {
            verdicts++;
        }
    }
    test_check(verified && verdicts == 0,
               "ed25519 oom sweep: no allocation failure inside verify reads as a bad signature");

    /* Ed25519 is deterministic, so every AXL_OK must carry exactly RFC
       8032's signature for this key and message. A failure may refuse; it
       may not answer wrong. */
    bool     signed_ok = false;
    unsigned wrong     = 0;
    for (size_t n = 1; n <= 32; n++) {
        uint8_t out[ED25519_SIG_LEN];
        size_t  olen = sizeof(out);
        axl_mem_fail_next_alloc(n);
        int rc = axl_pk_key_sign(k, msg, sizeof(msg), AXL_PK_SIG_RAW, out,
                                 &olen);
        axl_mem_fail_next_alloc(0);
        if (rc != AXL_OK) {
            continue;
        }
        if (olen == sizeof(out) && axl_memcmp(out, sig, sizeof(out)) == 0) {
            signed_ok = true;
            break;
        }
        wrong++;
    }
    test_check(signed_ok && wrong == 0,
               "ed25519 oom sweep: no allocation failure inside sign returns a wrong signature as AXL_OK");
    axl_pk_key_free(k);
}

/* R = encode(B), S = 1: the forgery an all-zero challenge hash admits.
   With h = 0, ref10's check R == [S]B - [h]A reduces to R == [S]B, so this
   signature verifies under EVERY public key and every message. B's
   encoding is y = 4/5 mod p, little-endian, x even (RFC 8032 section
   5.1). */
static const uint8_t H0_FORGERY[ED25519_SIG_LEN] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x01
};

/* A SHA-512 that fails must refuse the operation it happened in. ref10
   carries on with an all-zero digest, and there is no safe digest to
   carry on with:
     - sign's NONCE hash zeroed gives r = 0, so R = identity and S = h*a --
       a signature that VERIFIES and hands anyone the private scalar;
     - verify's CHALLENGE hash zeroed gives h = 0, and H0_FORGERY verifies
       under any key.
   The shipped hash cannot fail (the glue #errors on the mbedTLS options
   that would let it), so the glue's test hook stands in for the day it
   can. Each case asserts the refusal AND that the hook really fired. */
static void
test_ed25519_hash_failure_refused(void)
{
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              der[ED25519_PRIV_DER_LEN];
    uint8_t              msg[1];
    test_check(test_hex_bytes(v->priv_der, der, sizeof(der))
                   && v->msg_len == sizeof(msg)
                   && test_hex_bytes(v->msg, msg, sizeof(msg)),
               "ed25519 hash failure: the control vectors are well-formed");
    AxlPkKey *k = axl_pk_key_load_private(der, sizeof(der));
    test_check(k != NULL, "ed25519 hash failure: import the signing key");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. */
        test_skip_n(8, "no Ed25519 key to sign with");
        return;
    }

    /* sign hashes three times, in this order: the secret key (az), the
       nonce, the challenge. Whichever fails, nothing may come back as
       AXL_OK, and nothing may be written to the caller's buffer. */
    static const char *const WHICH[3] = {
        "ed25519 hash failure: sign refuses when its key hash fails, and writes nothing",
        "ed25519 hash failure: sign refuses when its NONCE hash fails -- no key-disclosing signature",
        "ed25519 hash failure: sign refuses when its challenge hash fails, and writes nothing"
    };
    for (uint32_t n = 1; n <= 3; n++) {
        uint8_t  out[ED25519_SIG_LEN];
        size_t   olen   = sizeof(out);
        uint32_t before = _axl_ed25519_glue_failures();
        axl_memset(out, 0xA5, sizeof(out));
        _axl_ed25519_glue_fail_hash(n);
        int rc = axl_pk_key_sign(k, msg, sizeof(msg), AXL_PK_SIG_RAW, out,
                                 &olen);
        _axl_ed25519_glue_fail_hash(0);
        bool untouched = true;
        for (size_t i = 0; i < sizeof(out); i++) {
            untouched = untouched && out[i] == 0xA5;
        }
        test_check(_axl_ed25519_glue_failures() == before + 1
                       && rc != AXL_OK && untouched,
                   WHICH[n - 1]);
    }

    /* verify hashes once: the challenge. The h = 0 forgery must not pass
       with that hash failing -- the case the old glue let through. */
    uint32_t before = _axl_ed25519_glue_failures();
    _axl_ed25519_glue_fail_hash(1);
    int rc = axl_pk_key_verify(k, msg, sizeof(msg), AXL_PK_SIG_RAW,
                               H0_FORGERY, sizeof(H0_FORGERY));
    _axl_ed25519_glue_fail_hash(0);
    test_check(_axl_ed25519_glue_failures() == before + 1 && rc != AXL_OK,
               "ed25519 hash failure: R = encode(B), S = 1 is refused when the challenge hash fails");
    axl_pk_key_free(k);

    /* Keygen and private import hash the seed once each, and a zeroed
       digest there is a key whose public half does not match the seed
       sign re-derives from. Neither may hand back a key. */
    before = _axl_ed25519_glue_failures();
    _axl_ed25519_glue_fail_hash(1);
    AxlPkKey *gen = axl_pk_key_new(AXL_PK_ED25519);
    _axl_ed25519_glue_fail_hash(0);
    test_check(_axl_ed25519_glue_failures() == before + 1 && gen == NULL,
               "ed25519 hash failure: keygen refuses when its hash fails");
    axl_pk_key_free(gen);

    before = _axl_ed25519_glue_failures();
    _axl_ed25519_glue_fail_hash(1);
    AxlPkKey *imp = axl_pk_key_load_private(der, sizeof(der));
    _axl_ed25519_glue_fail_hash(0);
    test_check(_axl_ed25519_glue_failures() == before + 1 && imp == NULL,
               "ed25519 hash failure: private import refuses when its hash fails");
    axl_pk_key_free(imp);

    /* ref10's randombytes is never AXL's entropy: keygen draws its own
       seed where a failure is checkable. A key built from whatever
       randombytes left -- zeros, on failure -- is a key anyone can
       derive. */
    uint32_t calls = _axl_ed25519_glue_randombytes_calls();
    AxlPkKey *fresh = axl_pk_key_new(AXL_PK_ED25519);
    test_check(fresh != NULL, "ed25519 keygen: control -- a key is generated");
    test_check(_axl_ed25519_glue_randombytes_calls() == calls,
               "ed25519 keygen: never reaches ref10's randombytes");
    axl_pk_key_free(fresh);
}

/* H0_FORGERY with a WORKING hash, under every RFC vector's key and
   message: the h = 0 shape must never verify for any key, which is the
   consequence the hash-failure test above guards from the other side. */
static void
test_ed25519_h0_forgery_refused(void)
{
    static const char *const NAMES[3] = {
        "ed25519 forgery: R = encode(B), S = 1 is refused under TEST 1's key",
        "ed25519 forgery: R = encode(B), S = 1 is refused under TEST 2's key",
        "ed25519 forgery: R = encode(B), S = 1 is refused under TEST 3's key"
    };
    for (size_t i = 0; i < 3; i++) {
        const Ed25519Vector *v = &VECTORS[i];
        uint8_t              pub[ED25519_RAW_PUB_LEN];
        uint8_t              buf[16];
        const uint8_t       *msg = NULL;
        AxlPkKey            *k   = NULL;
        if (v->msg_len <= sizeof(buf)
            && test_hex_bytes(v->pub_raw, pub, sizeof(pub))) {
            msg = vector_msg(v, buf);
            k   = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                             sizeof(pub));
        }
        int rc = k != NULL && (msg != NULL || v->msg_len == 0)
                 ? axl_pk_key_verify(k, msg, v->msg_len, AXL_PK_SIG_RAW,
                                     H0_FORGERY, sizeof(H0_FORGERY))
                 : AXL_OK;
        test_check(k != NULL && rc == AXL_ERR, NAMES[i]);
        axl_pk_key_free(k);
    }
}

static void
test_ed25519_bad_public_keys(void)
{
    /* R = encode(B), S = 1. */
    static const char *FORGED =
        "5866666666666666666666666666666666666666666666666666666666666666"
        "0100000000000000000000000000000000000000000000000000000000000000";
    /* The identity. */
    static const char *ID_PUB =
        "0100000000000000000000000000000000000000000000000000000000000000";
    /* The order-2 point (0, -1), and a message whose challenge h is
       even -- which is what makes [h]A the identity and the forgery
       valid against a verifier that does not refuse the key. */
    static const char *O2_PUB =
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
    static const char  O2_MSG[] = "axl-order2-probe-0";

    uint8_t forged[ED25519_SIG_LEN];
    uint8_t id_pub[ED25519_RAW_PUB_LEN];
    uint8_t o2_pub[ED25519_RAW_PUB_LEN];
    test_check(test_hex_bytes(FORGED, forged, sizeof(forged))
                   && test_hex_bytes(ID_PUB, id_pub, sizeof(id_pub))
                   && test_hex_bytes(O2_PUB, o2_pub, sizeof(o2_pub)),
               "ed25519 smallorder: the forgery vectors are well-formed");

    AxlPkKey *id = axl_pk_key_from_raw_public(AXL_PK_ED25519, id_pub,
                                              sizeof(id_pub));
    test_check(id != NULL,
               "ed25519 smallorder: the identity key imports (refused at verify, not import)");
    if (id != NULL) {
        /* Two unrelated messages, because the forgery's whole property
           is that it does not depend on the message -- one could pass
           by coincidence, two cannot. */
        static const uint8_t m1[1] = { 0x72 };
        static const uint8_t m2[4] = { 'a', 'x', 'l', '!' };
        test_check(axl_pk_key_verify(id, m1, sizeof(m1), AXL_PK_SIG_RAW,
                                     forged, ED25519_SIG_LEN) == AXL_ERR,
                   "ed25519 smallorder: the identity-key forgery is refused");
        test_check(axl_pk_key_verify(id, m2, sizeof(m2), AXL_PK_SIG_RAW,
                                     forged, ED25519_SIG_LEN) == AXL_ERR,
                   "ed25519 smallorder: refused for a second message too");
    }
    axl_pk_key_free(id);

    AxlPkKey *o2 = axl_pk_key_from_raw_public(AXL_PK_ED25519, o2_pub,
                                              sizeof(o2_pub));
    test_check(o2 != NULL, "ed25519 smallorder: the order-2 key imports");
    if (o2 != NULL) {
        test_check(axl_pk_key_verify(o2, (const uint8_t *)O2_MSG,
                                     sizeof(O2_MSG) - 1, AXL_PK_SIG_RAW,
                                     forged, ED25519_SIG_LEN) == AXL_ERR,
                   "ed25519 smallorder: the order-2 forgery is refused");
    }
    axl_pk_key_free(o2);

    /* EVERY torsion encoding, each with a forgery that DISCRIMINATES
       IT. The first version of this test reused the order-2 forgery
       against all twelve remaining encodings and asserted one aggregate
       flag -- which passed, and proved almost nothing: that signature
       only discriminates keys of order dividing 2, so for the order-4
       and order-8 rows the assertion was satisfied by ordinary
       cryptographic failure rather than by the table. Neutralising those
       rows left the suite green while a real no-private-key forgery
       became verifiable. An independent review demonstrated exactly
       that, and it is the sharpest "passes for the wrong reason" this
       phase produced.

       So each row gets its own ground message: with A of order k,
       [h]A is the identity only when h is a multiple of k, and h is
       SHA-512(R || A || msg), so the message is ground per ENCODING,
       not per order. Every pair below was checked against an
       independent implementation of RFC 8032's verification equation
       -- a checkless verifier ACCEPTS all fourteen. Sabotaging any
       single row now fails that row's own assertion. */
    static const struct {
        const char *pub;  /* the torsion encoding */
        const char *msg;  /* a message whose h annihilates it */
    } TORSION[] = {
        { "0000000000000000000000000000000000000000000000000000000000000000", "f0" },
        { "0000000000000000000000000000000000000000000000000000000000000080", "f4" },
        { "0100000000000000000000000000000000000000000000000000000000000000", "f0" },
        { "0100000000000000000000000000000000000000000000000000000000000080", "f0" },
        { "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05", "f1" },
        { "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85", "f0" },
        { "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a", "f3" },
        { "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa", "f2" },
        { "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "f0" },
        { "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "f0" },
        /* y = p, order 4: ref10 refuses this NOWHERE. fe25519_unpack
           masks bit 255 and does not range-check y, and the point is
           not the identity, so ge25519_isneutral_vartime does not fire
           either. These two rows are its only refusal in the tree. */
        { "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "f8" },
        { "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "f1" },
        { "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "f0" },
        { "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "f0" },
    };
    for (size_t i = 0; i < sizeof(TORSION) / sizeof(TORSION[0]); i++) {
        uint8_t t[ED25519_RAW_PUB_LEN];
        char    what[80];
        if (!test_hex_bytes(TORSION[i].pub, t, sizeof(t))) {
            test_check(false,
                       "ed25519 smallorder: a torsion vector is malformed");
            continue;
        }
        AxlPkKey *k = axl_pk_key_from_raw_public(AXL_PK_ED25519, t,
                                                 sizeof(t));
        axl_snprintf(what, sizeof(what),
                     "ed25519 smallorder: torsion key %u refuses its own forgery",
                     (unsigned)i);
        test_check(k != NULL
                       && axl_pk_key_verify(k,
                                            (const uint8_t *)TORSION[i].msg,
                                            axl_strlen(TORSION[i].msg),
                                            AXL_PK_SIG_RAW, forged,
                                            ED25519_SIG_LEN) == AXL_ERR,
                   what);
        axl_pk_key_free(k);
    }

    /* The check lives at verify, not at import, precisely so it covers
       construction paths other than from_raw_public(). Pin that with
       the SPKI path rather than leaving it as a claim in a comment:
       same identity key, wrapped in RFC 8410 DER. */
    static const char *ID_SPKI =
        "302a300506032b6570032100"
        "0100000000000000000000000000000000000000000000000000000000000000";
    uint8_t id_spki[44];
    test_check(test_hex_bytes(ID_SPKI, id_spki, sizeof(id_spki)),
               "ed25519 smallorder: the identity SPKI is well-formed");
    AxlPkKey *spki = axl_pk_key_load_public(id_spki, sizeof(id_spki));
    test_check(spki != NULL
                   && axl_pk_key_verify(spki, (const uint8_t *)"x", 1,
                                        AXL_PK_SIG_RAW, forged,
                                        ED25519_SIG_LEN) == AXL_ERR,
               "ed25519 smallorder: refused via the DER path too");
    axl_pk_key_free(spki);

    /* A NON-small-order key must still work, or the check above could
       be refusing everything. */
    const Ed25519Vector *v = &VECTORS[1];
    uint8_t              pub[ED25519_RAW_PUB_LEN];
    uint8_t              good[ED25519_SIG_LEN];
    test_check(test_hex_bytes(v->pub_raw, pub, sizeof(pub))
                   && test_hex_bytes(v->sig, good, sizeof(good)),
               "ed25519 smallorder: the control vectors are well-formed");
    AxlPkKey *real = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                                sizeof(pub));
    static const uint8_t msg2[1] = { 0x72 };
    test_check(real != NULL
                   && axl_pk_key_verify(real, msg2, 1, AXL_PK_SIG_RAW,
                                        good, ED25519_SIG_LEN) == AXL_OK,
               "ed25519 smallorder: control -- an honest key still verifies");
    axl_pk_key_free(real);
}

static int
test_ed25519_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlEd25519");

    /* The opt-in this binary exists to exercise -- and deliberately NOT
       axl_pk_enable_mbedtls(): see test_ed25519_mbedtls_absent(). */
    axl_pk_enable_ed25519();

    test_ed25519_available();
    test_ed25519_mbedtls_absent();
    test_ed25519_rfc8032_vectors();
    test_ed25519_rfc8032_test1024();
    test_ed25519_der_roundtrip();
    test_ed25519_der_rejects();
    test_ed25519_raw_public();
    test_ed25519_keygen();
    test_ed25519_refusals();
    test_ed25519_bad_public_keys();
    test_ed25519_length_overflow();
    test_ed25519_verify_oom();
    test_ed25519_oom_sweeps();
    test_ed25519_hash_failure_refused();
    test_ed25519_h0_forgery_refused();

    return test_print_results();
}

AXL_APP(test_ed25519_main)
