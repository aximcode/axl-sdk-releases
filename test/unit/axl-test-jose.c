/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-jose.c
    Unit tests for axl-jose (JWS / JWT / JWK).

    Allow-list / argument validation and the real signing and
    verification outcomes (KATs, round-trips, the rejection matrix, JWT
    claims, JWK parse/export) all run here.

    They used to be two layers, mirroring axl-test-crypto.c: the real
    outcomes needed an AXL_TLS=1 build and sat behind AXL_HAVE_TLS.
    mbedTLS is unconditional now, so there is one layer.

    The ES256 (RFC 7515 A.3) and HS256 (RFC 7515 A.1) verification KATs are
    cross-checked against an independent implementation before embedding,
    so a failure here is an axl-jose bug, not a bad constant.
**/

#include <axl.h>
#include "axl-test.h"

// ---------------------------------------------------------------------------
// RFC 7515 known-answer vectors.
// ---------------------------------------------------------------------------

/* A.1 / A.3 share this payload (base64url decodes to the bytes below,
   CRLF and all). Only the TLS build verifies against it. */
static const char RFC7515_PAYLOAD[] =
    "{\"iss\":\"joe\",\r\n"
    " \"exp\":1300819380,\r\n"
    " \"http://example.com/is_root\":true}";

/* A.1 — HS256. token = header.payload.sig; key is the base64url `k`. */
static const char A1_HS256_TOKEN[] =
    "eyJ0eXAiOiJKV1QiLA0KICJhbGciOiJIUzI1NiJ9"
    ".eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
    "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ"
    ".dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
static const char A1_HS256_KEY_B64URL[] =
    "AyM1SysPpbyDfgZld3umj1qzKObwVMkoqQ-EstJQLr_T-1qS0gZH75aKtMN3Yj0i"
    "PS4hcgUuTwjAzZr1Z9CAow";

/* A.3 — ES256 (P-256). Referenced in both builds (the TLS build verifies
   it; the non-TLS build asserts the call fails closed). */
static const char A3_ES256_TOKEN[] =
    "eyJhbGciOiJFUzI1NiJ9"
    ".eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
    "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ"
    ".DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSA"
    "pmWQxfKTUJqPP3-Kg6NU1Q";
/* The A.3 public key as a JWK (no `alg`/`kid`, so ES256 is inferred).
   This is also the canonical export form (kty, crv, x, y in this order),
   so it doubles as the exact-string KAT for axl_jwk_export_public. */
static const char A3_ES256_JWK[] =
    "{\"kty\":\"EC\",\"crv\":\"P-256\","
    "\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
    "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\"}";

/* A.3 `x` with a `y` of 32 zero bytes — a valid 43-char base64url
   coordinate, but (x, 0) is not on P-256, so curve-membership validation
   must reject it. */
static const char OFFCURVE_EC_JWK[] =
    "{\"kty\":\"EC\",\"crv\":\"P-256\","
    "\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
    "\"y\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";

/* A 17-bit RSA modulus (n = 0x010001) — well below the 2048-bit floor. */
static const char TINY_RSA_JWK[] =
    "{\"kty\":\"RSA\",\"n\":\"AQAB\",\"e\":\"AQAB\"}";

/* ES384 (P-384 + SHA-384) KAT — token + its public JWK, generated and
   cross-checked against an independent implementation. */
static const char ES384_TOKEN[] =
    "eyJhbGciOiJFUzM4NCJ9.eyJzdWIiOiJlczM4NC1rYXQifQ."
    "Hbc4LUhz9yXDCuldHSR6M4PCqvjlOhJGWNzjNuT24D4VGFhK4HmkbNCk9dnNeeIF"
    "DqnXb3A2VvoMILfT2-PPBE72q188LZsNZFCXHR4TmxCeZrNI99PMNBofhWe5pTqT";
static const char ES384_JWK[] =
    "{\"kty\":\"EC\",\"crv\":\"P-384\","
    "\"x\":\"XIakf0D27c1oyh49ESUi3IrJHOTBC3Q60-jiU48wvkiNUAN6AblA3cbfjXRlrhYm\","
    "\"y\":\"eJ5c1iRMeag7t9Oj5xOOudHyFqr9IVud68iVDYbMQBoR68kp0D46rjIfN1YIqjFD\"}";

/* PS256 (RSA-PSS + SHA-256, salt=32) KAT — token + its public JWK. */
static const char PS256_TOKEN[] =
    "eyJhbGciOiJQUzI1NiJ9.eyJzdWIiOiJwczI1Ni1rYXQifQ."
    "PmyRTIgJ0g61qHDaOgXDWj7r5NPckjRykGtCN6VZqem4ztHoe6dHBxwjQDL1Qb8V"
    "5ZsKQlQBlcnw-cga2nr34eereN2jMOJ4NOmnabUZ6hMw5Fdw_R5sHXd8uS9_QSUm"
    "D8yxcepi8J4zEy6DT5kWrtDtsPjS7iplu1xWcwg-pVhOoVo35lV90fSVx_iRMtPN"
    "D22-DSb3EOiTg_zWBjzzZT3M3wA8mLGRrRKFs0zcyKUXd76WK1msizDVLhM3or01"
    "iu_uTPy3TRS1hTBS5wue9FOZeRJ-Sjlhs7cEm_UJaXwMqcTwPCPKb6Oy_wMHquFy"
    "JQ3HirOFJR6NKbdzuUY5EQ";
static const char PS256_JWK[] =
    "{\"kty\":\"RSA\",\"e\":\"AQAB\",\"n\":\""
    "qP5Eao56ETZcneM-_4UfISA12I8zxkj3jgibpAJWFYd63rdy-vy4qFmnRPQWkf7k"
    "Fy9X7d4bTz2t66HGuG8H4lfJtXRF5niAukNgmly2YvJEYDX-SKa3gBS3Byd8S6YS"
    "tmhRU2xBzCLDD5Unkl5WygRguzjtKmV6fQgMgDBRdzuudsrkRLOksP-gGeaVL9MP"
    "Y84Bngf6nmTxi9SwVj21fTkyD1uXQH-pwWiV31fbUYunQdBCTk-wXiM_ecPOL2E1"
    "n0pgnjGmTIo6CxYAn6_VQju6H1s57OFqdLdlh2ud-Zq4Z7_B3laa_707bzQhYf8b"
    "bcDrzpek0dBFPUrg7aya4Q\"}";

// ---------------------------------------------------------------------------
// Allow-list / argument validation — holds in every build.
// ---------------------------------------------------------------------------

static void
test_jose_argval(void)
{
    AxlJoseKey k = { 0 };
    uint8_t   *payload = NULL;
    size_t     plen = 0;
    const AxlJoseAlg es256[] = { AXL_JOSE_ES256 };

    /* A NULL or empty allow-list is rejected with no signature check. */
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &k, NULL, 1, &payload, &plen) == AXL_ERR,
               "jws_verify: NULL allow-list -> AXL_ERR");
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &k, es256, 0, &payload, &plen) == AXL_ERR,
               "jws_verify: empty allow-list -> AXL_ERR");

    /* NULL token / out-params. */
    test_check(axl_jws_verify(NULL, 0, &k, es256, 1, &payload, &plen)
                   == AXL_ERR,
               "jws_verify: NULL token -> AXL_ERR");
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &k, es256, 1, NULL, &plen) == AXL_ERR,
               "jws_verify: NULL payload_out -> AXL_ERR");

    /* Sign with a NULL key / token_out. */
    char *tok = NULL;
    test_check(axl_jws_sign(NULL, AXL_JOSE_HS256, (const uint8_t *)"x", 1,
                            &tok) == AXL_ERR,
               "jws_sign: NULL key -> AXL_ERR");
    test_check(axl_jws_sign(&k, AXL_JOSE_HS256, (const uint8_t *)"x", 1,
                            NULL) == AXL_ERR,
               "jws_sign: NULL token_out -> AXL_ERR");
}


// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

/* Sign @p payload with HS256 under @p key/@p key_len, returning the token
   (caller frees). Used to fabricate JWTs with arbitrary claims. */
static char *
hs256_sign(const uint8_t *key, size_t key_len,
           const char *payload)
{
    AxlJoseKey k = { .hmac_key = key, .hmac_key_len = key_len };
    char      *tok = NULL;
    if (axl_jws_sign(&k, AXL_JOSE_HS256,
                     (const uint8_t *)payload, axl_strlen(payload),
                     &tok) != AXL_OK) {
        return NULL;
    }
    return tok;
}

// ---------------------------------------------------------------------------
// JWS verification KATs (RFC 7515 A.1 / A.3).
// ---------------------------------------------------------------------------

static void
test_jws_kat(void)
{
    test_check(axl_jose_available() == true,
               "jose_available: true");

    /* ---- A.3 ES256: verify against the JWK-parsed public key. ---- */
    AxlJoseAlg alg_out = (AxlJoseAlg)0;
    AxlPkKey  *pub = axl_jwk_parse(A3_ES256_JWK, axl_strlen(A3_ES256_JWK),
                                   NULL, &alg_out);
    test_check(pub != NULL, "jwk_parse: A.3 EC JWK -> key");
    test_check(alg_out == AXL_JOSE_ES256, "jwk_parse: EC P-256 infers ES256");

    AxlJoseKey      eckey = { .pk = pub };
    const AxlJoseAlg es256[] = { AXL_JOSE_ES256 };
    uint8_t        *payload = NULL;
    size_t          plen = 0;
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &eckey, es256, 1, &payload, &plen) == AXL_OK,
               "jws_verify: RFC 7515 A.3 ES256 KAT -> AXL_OK");
    test_check(payload != NULL
                   && plen == sizeof(RFC7515_PAYLOAD) - 1
                   && axl_memcmp(payload, RFC7515_PAYLOAD, plen) == 0,
               "jws_verify: A.3 payload decodes to the RFC bytes");
    axl_free(payload);
    payload = NULL;

    /* Tampered signature: flip the FIRST signature char, which carries
       significant bits (the final base64url char of a 64-byte signature
       only holds 2 significant bits, so flipping it can decode to the same
       bytes). -> AXL_ERR. */
    {
        char bad[sizeof(A3_ES256_TOKEN)];
        axl_memcpy(bad, A3_ES256_TOKEN, sizeof(A3_ES256_TOKEN));
        char *sig = axl_strchr(axl_strchr(bad, '.') + 1, '.') + 1;
        sig[0] = (sig[0] == 'D') ? 'E' : 'D';
        test_check(axl_jws_verify(bad, axl_strlen(bad), &eckey, es256, 1,
                                  &payload, &plen) == AXL_ERR,
                   "jws_verify: A.3 tampered signature -> AXL_ERR");
    }

    /* Tampered payload (flip a char in the middle segment) -> AXL_ERR. */
    {
        char bad[sizeof(A3_ES256_TOKEN)];
        axl_memcpy(bad, A3_ES256_TOKEN, sizeof(A3_ES256_TOKEN));
        bad[25] = (bad[25] == 'a') ? 'b' : 'a';
        test_check(axl_jws_verify(bad, axl_strlen(bad), &eckey, es256, 1,
                                  &payload, &plen) == AXL_ERR,
                   "jws_verify: A.3 tampered payload -> AXL_ERR");
    }

    axl_pk_key_free(pub);

    /* ---- A.1 HS256: verify with the decoded oct key. ---- */
    void  *raw_key = NULL;
    size_t raw_len = 0;
    test_check(axl_base64url_decode(A1_HS256_KEY_B64URL,
                                    axl_strlen(A1_HS256_KEY_B64URL),
                                    &raw_key, &raw_len) == AXL_OK
                   && raw_len == 64,
               "hs256: decode A.1 oct key (64 bytes)");

    AxlJoseKey      hmac = { .hmac_key = raw_key, .hmac_key_len = raw_len };
    const AxlJoseAlg hs256[] = { AXL_JOSE_HS256 };
    test_check(axl_jws_verify(A1_HS256_TOKEN, axl_strlen(A1_HS256_TOKEN),
                              &hmac, hs256, 1, &payload, &plen) == AXL_OK,
               "jws_verify: RFC 7515 A.1 HS256 KAT -> AXL_OK");
    test_check(payload != NULL && plen == sizeof(RFC7515_PAYLOAD) - 1
                   && axl_memcmp(payload, RFC7515_PAYLOAD, plen) == 0,
               "jws_verify: A.1 payload decodes to the RFC bytes");
    axl_free(payload);
    payload = NULL;

    /* Wrong HMAC key -> AXL_ERR. */
    {
        uint8_t wrong[64];
        axl_memcpy(wrong, raw_key, 64);
        wrong[0] ^= 0x01;
        AxlJoseKey wk = { .hmac_key = wrong, .hmac_key_len = 64 };
        test_check(axl_jws_verify(A1_HS256_TOKEN, axl_strlen(A1_HS256_TOKEN),
                                  &wk, hs256, 1, &payload, &plen) == AXL_ERR,
                   "jws_verify: A.1 wrong HMAC key -> AXL_ERR");
    }
    axl_free(raw_key);
}

// ---------------------------------------------------------------------------
// ES384 / PS256 verification KATs (independently cross-checked).
// ---------------------------------------------------------------------------

static void
test_jws_kat_es384_ps256(void)
{
    uint8_t *payload = NULL;
    size_t   plen = 0;

    /* ES384: P-384 + SHA-384. */
    AxlJoseAlg alg_out = (AxlJoseAlg)0;
    AxlPkKey  *ec = axl_jwk_parse(ES384_JWK, axl_strlen(ES384_JWK),
                                  NULL, &alg_out);
    test_check(ec != NULL && alg_out == AXL_JOSE_ES384,
               "jwk_parse: P-384 JWK infers ES384");
    AxlJoseKey       eckey = { .pk = ec };
    const AxlJoseAlg es384[] = { AXL_JOSE_ES384 };
    const AxlJoseAlg es256[] = { AXL_JOSE_ES256 };
    test_check(ec != NULL
                   && axl_jws_verify(ES384_TOKEN, axl_strlen(ES384_TOKEN),
                          &eckey, es384, 1, &payload, &plen) == AXL_OK,
               "jws_verify: ES384 KAT -> AXL_OK");
    axl_free(payload); payload = NULL;
    /* An ES384 token must not verify when only ES256 is allowed. */
    test_check(axl_jws_verify(ES384_TOKEN, axl_strlen(ES384_TOKEN),
                              &eckey, es256, 1, &payload, &plen) == AXL_ERR,
               "jws_verify: ES384 token not on ES256 allow-list -> AXL_ERR");
    axl_pk_key_free(ec);

    /* PS256: RSA-PSS + SHA-256. */
    AxlPkKey  *rsa = axl_jwk_parse(PS256_JWK, axl_strlen(PS256_JWK),
                                   NULL, &alg_out);
    test_check(rsa != NULL, "jwk_parse: PS256 JWK -> key");
    AxlJoseKey       rkey = { .pk = rsa };
    const AxlJoseAlg ps256[] = { AXL_JOSE_PS256 };
    const AxlJoseAlg rs256[] = { AXL_JOSE_RS256 };
    test_check(rsa != NULL
                   && axl_jws_verify(PS256_TOKEN, axl_strlen(PS256_TOKEN),
                          &rkey, ps256, 1, &payload, &plen) == AXL_OK,
               "jws_verify: PS256 KAT -> AXL_OK");
    axl_free(payload); payload = NULL;
    /* A PS256 token must not verify under RS256 (PSS vs PKCS#1 v1.5): the
       padding scheme is selected by the allow-listed alg, not the key. */
    test_check(axl_jws_verify(PS256_TOKEN, axl_strlen(PS256_TOKEN),
                              &rkey, rs256, 1, &payload, &plen) == AXL_ERR,
               "jws_verify: PS256 token not on RS256 allow-list -> AXL_ERR");
    axl_pk_key_free(rsa);
}

// ---------------------------------------------------------------------------
// Rejection matrix.
// ---------------------------------------------------------------------------

static void
test_jws_rejections(void)
{
    void  *raw_key = NULL;
    size_t raw_len = 0;
    axl_base64url_decode(A1_HS256_KEY_B64URL,
                         axl_strlen(A1_HS256_KEY_B64URL),
                         &raw_key, &raw_len);
    AxlJoseKey hmac = { .hmac_key = raw_key, .hmac_key_len = raw_len };
    AxlPkKey  *pub = axl_jwk_parse(A3_ES256_JWK, axl_strlen(A3_ES256_JWK),
                                   NULL, NULL);
    AxlJoseKey eckey = { .pk = pub };
    uint8_t   *payload = NULL;
    size_t     plen = 0;

    const AxlJoseAlg hs256[]      = { AXL_JOSE_HS256 };
    const AxlJoseAlg es256[]      = { AXL_JOSE_ES256 };
    const AxlJoseAlg mixed[]      = { AXL_JOSE_ES256, AXL_JOSE_HS256 };

    /* alg not on the allow-list: an ES256 token, an HS256-only list. */
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &eckey, hs256, 1, &payload, &plen) == AXL_ERR,
               "jws_verify: token alg not in allow-list -> AXL_ERR");

    /* Mixed-family allow-list is rejected outright. */
    test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                              &eckey, mixed, 2, &payload, &plen) == AXL_ERR,
               "jws_verify: mixed symmetric+asymmetric allow-list -> AXL_ERR");

    /* alg:none — header {"alg":"none"} + empty signature segment. */
    {
        /* base64url({"alg":"none"}) = eyJhbGciOiJub25lIn0 */
        const char *none_tok =
            "eyJhbGciOiJub25lIn0"
            ".eyJpc3MiOiJqb2UifQ"
            ".";
        test_check(axl_jws_verify(none_tok, axl_strlen(none_tok),
                                  &hmac, hs256, 1, &payload, &plen) == AXL_ERR,
                   "jws_verify: alg:none (empty sig) -> AXL_ERR");
    }

    /* Malformed: two segments, then four. */
    {
        const char *two = "aaa.bbb";
        const char *four = "aaa.bbb.ccc.ddd";
        test_check(axl_jws_verify(two, axl_strlen(two), &hmac, hs256, 1,
                                  &payload, &plen) == AXL_ERR,
                   "jws_verify: two-segment token -> AXL_ERR");
        test_check(axl_jws_verify(four, axl_strlen(four), &hmac, hs256, 1,
                                  &payload, &plen) == AXL_ERR,
                   "jws_verify: four-segment token -> AXL_ERR");
    }

    /* RS256<->HS256 confusion: an HS256-only list cannot select the EC key
       and an ES256 token is not on it — rejected before any HMAC runs. The
       structural defense is family binding, exercised by the mixed-list and
       alg-not-allowed cases above; here we confirm a missing key field for
       the selected family also fails closed (ES256 token, list allows
       ES256, but the key carries only an HMAC secret, no pk). */
    {
        AxlJoseKey hmac_only = { .hmac_key = raw_key, .hmac_key_len = raw_len };
        test_check(axl_jws_verify(A3_ES256_TOKEN, axl_strlen(A3_ES256_TOKEN),
                                  &hmac_only, es256, 1, &payload, &plen)
                       == AXL_ERR,
                   "jws_verify: ES256 selected but no pk in key -> AXL_ERR");
    }

    axl_pk_key_free(pub);
    axl_free(raw_key);
}

// ---------------------------------------------------------------------------
// Sign + round-trip (HS256, ES256, RS256).
// ---------------------------------------------------------------------------

static void
test_jws_roundtrip(void)
{
    const uint8_t  secret[] = "super-secret-hmac-key-0123456789";
    const char     msg[]    = "{\"hello\":\"world\"}";
    uint8_t       *payload = NULL;
    size_t         plen = 0;

    /* HS256. */
    {
        char *tok = hs256_sign(secret, sizeof(secret) - 1, msg);
        test_check(tok != NULL, "jws_sign: HS256 -> token");
        AxlJoseKey k = { .hmac_key = secret, .hmac_key_len = sizeof(secret) - 1 };
        const AxlJoseAlg allow[] = { AXL_JOSE_HS256 };
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &k, allow, 1,
                                         &payload, &plen) == AXL_OK
                       && plen == axl_strlen(msg)
                       && axl_memcmp(payload, msg, plen) == 0,
                   "jws round-trip: HS256 sign->verify recovers payload");
        axl_free(payload); payload = NULL;
        axl_free(tok);
    }

    /* ES256 (generated key). */
    {
        AxlPkKey *key = axl_pk_key_new(AXL_PK_ECDSA_P256);
        test_check(key != NULL, "keygen: ECDSA P-256 -> key");
        AxlJoseKey jk = { .pk = key };
        char *tok = NULL;
        test_check(key != NULL
                       && axl_jws_sign(&jk, AXL_JOSE_ES256,
                                       (const uint8_t *)msg, axl_strlen(msg),
                                       &tok) == AXL_OK,
                   "jws_sign: ES256 -> token");
        const AxlJoseAlg allow[] = { AXL_JOSE_ES256 };
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &jk, allow, 1,
                                         &payload, &plen) == AXL_OK
                       && plen == axl_strlen(msg)
                       && axl_memcmp(payload, msg, plen) == 0,
                   "jws round-trip: ES256 sign->verify recovers payload");
        axl_free(payload); payload = NULL;
        axl_free(tok);

        /* Public-only key cannot sign. */
        uint8_t der[256]; size_t dl = sizeof(der);
        char   *tok2 = NULL;
        if (axl_pk_key_get_public_der(key, der, &dl) == AXL_OK) {
            AxlPkKey  *pubonly = axl_pk_key_load_public(der, dl);
            AxlJoseKey pj = { .pk = pubonly };
            test_check(axl_jws_sign(&pj, AXL_JOSE_ES256,
                                    (const uint8_t *)msg, axl_strlen(msg),
                                    &tok2) == AXL_ERR,
                       "jws_sign: ES256 with public-only key -> AXL_ERR");
            axl_pk_key_free(pubonly);
        }
        axl_pk_key_free(key);
    }

    /* RS256 (generated key — slow keygen, but proves the RSA path). */
    {
        AxlPkKey *key = axl_pk_key_new(AXL_PK_RSA);
        test_check(key != NULL, "keygen: RSA -> key");
        AxlJoseKey jk = { .pk = key };
        char *tok = NULL;
        test_check(key != NULL
                       && axl_jws_sign(&jk, AXL_JOSE_RS256,
                                       (const uint8_t *)msg, axl_strlen(msg),
                                       &tok) == AXL_OK,
                   "jws_sign: RS256 -> token");
        const AxlJoseAlg allow[] = { AXL_JOSE_RS256 };
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &jk, allow, 1,
                                         &payload, &plen) == AXL_OK
                       && plen == axl_strlen(msg)
                       && axl_memcmp(payload, msg, plen) == 0,
                   "jws round-trip: RS256 sign->verify recovers payload");
        axl_free(payload); payload = NULL;
        axl_free(tok);
        axl_pk_key_free(key);
    }

    /* HS256 sign needs an hmac_key; a pk-only key is a mismatch. */
    {
        AxlJoseKey bad = { .pk = NULL, .hmac_key = NULL };
        char *tok = NULL;
        test_check(axl_jws_sign(&bad, AXL_JOSE_HS256,
                                (const uint8_t *)msg, axl_strlen(msg),
                                &tok) == AXL_ERR,
                   "jws_sign: HS256 without hmac_key -> AXL_ERR");
    }

    /* ES384 (generated P-384 key). */
    {
        AxlPkKey *key = axl_pk_key_new(AXL_PK_ECDSA_P384);
        test_check(key != NULL, "keygen: ECDSA P-384 -> key");
        AxlJoseKey jk = { .pk = key };
        char *tok = NULL;
        test_check(key != NULL
                       && axl_jws_sign(&jk, AXL_JOSE_ES384,
                                       (const uint8_t *)msg, axl_strlen(msg),
                                       &tok) == AXL_OK,
                   "jws_sign: ES384 -> token");
        const AxlJoseAlg allow[] = { AXL_JOSE_ES384 };
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &jk, allow, 1,
                                         &payload, &plen) == AXL_OK
                       && plen == axl_strlen(msg)
                       && axl_memcmp(payload, msg, plen) == 0,
                   "jws round-trip: ES384 sign->verify recovers payload");
        axl_free(payload); payload = NULL;
        axl_free(tok);
        axl_pk_key_free(key);
    }

    /* PS256 (RSA-PSS, generated key). */
    {
        AxlPkKey *key = axl_pk_key_new(AXL_PK_RSA);
        AxlJoseKey jk = { .pk = key };
        char *tok = NULL;
        test_check(key != NULL
                       && axl_jws_sign(&jk, AXL_JOSE_PS256,
                                       (const uint8_t *)msg, axl_strlen(msg),
                                       &tok) == AXL_OK,
                   "jws_sign: PS256 -> token");
        const AxlJoseAlg allow[] = { AXL_JOSE_PS256 };
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &jk, allow, 1,
                                         &payload, &plen) == AXL_OK
                       && plen == axl_strlen(msg)
                       && axl_memcmp(payload, msg, plen) == 0,
                   "jws round-trip: PS256 sign->verify recovers payload");
        axl_free(payload); payload = NULL;
        axl_free(tok);

        /* The same key signs distinct PS256 signatures (random salt), yet
           both verify — and a PS256 signature does not verify as RS256. */
        const AxlJoseAlg rs256[] = { AXL_JOSE_RS256 };
        char *ps = NULL;
        test_check(axl_jws_sign(&jk, AXL_JOSE_PS256, (const uint8_t *)msg,
                                axl_strlen(msg), &ps) == AXL_OK,
                   "ps256: sign ok");
        test_check(ps != NULL
                       && axl_jws_verify(ps, axl_strlen(ps), &jk, rs256, 1,
                                         &payload, &plen) == AXL_ERR,
                   "jws_verify: PS256 token under RS256 allow-list -> AXL_ERR");
        axl_free(ps);
        axl_pk_key_free(key);
    }
}

// ---------------------------------------------------------------------------
// JWT claim validation.
// ---------------------------------------------------------------------------

static void
test_jwt(void)
{
    const uint8_t secret[] = "jwt-test-secret-key-aaaaaaaaaaaa";
    const size_t  slen = sizeof(secret) - 1;
    const AxlJoseAlg hs256[] = { AXL_JOSE_HS256 };
    const int64_t NOW = 1000000;

    uint8_t *payload = NULL;
    size_t   plen = 0;

    /* Valid: iss/aud match, exp in the future, nbf in the past. */
    char *tok = hs256_sign(secret, slen,
        "{\"iss\":\"axl\",\"aud\":\"svc\",\"exp\":1000100,\"nbf\":999900}");
    AxlJwtPolicy pol = {
        .expect_iss = "axl", .expect_aud = "svc", .now = NOW,
        .leeway_s = 0, .require_exp = true, .require_nbf = true,
    };
    AxlJsonReader claims = { 0 };
    test_check(tok != NULL
                   && axl_jwt_verify(tok, axl_strlen(tok),
                          &(AxlJoseKey){ .hmac_key = secret, .hmac_key_len = slen },
                          hs256, 1, &pol, &payload, &plen, &claims) == AXL_OK,
               "jwt_verify: valid token + matching policy -> AXL_OK");
    {
        char iss[16] = { 0 };
        test_check(axl_json_get_string(&claims, "iss", iss, sizeof(iss))
                       && axl_strcmp(iss, "axl") == 0,
                   "jwt_verify: claims_out reads iss");
    }
    axl_json_free(&claims);
    axl_free(payload); payload = NULL;
    axl_free(tok);

    AxlJoseKey k = { .hmac_key = secret, .hmac_key_len = slen };

    /* Expired exp -> AXL_ERR; covered by leeway -> AXL_OK. */
    tok = hs256_sign(secret, slen, "{\"exp\":999990}");
    AxlJwtPolicy p_exp = { .now = NOW, .leeway_s = 0, .require_exp = true };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_exp,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: expired exp -> AXL_ERR");
    p_exp.leeway_s = 60;
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_exp,
                              &payload, &plen, NULL) == AXL_OK,
               "jwt_verify: expired exp within leeway -> AXL_OK");
    axl_free(payload); payload = NULL;
    axl_free(tok);

    /* nbf in the future -> AXL_ERR; leeway -> AXL_OK. */
    tok = hs256_sign(secret, slen, "{\"nbf\":1000010}");
    AxlJwtPolicy p_nbf = { .now = NOW, .leeway_s = 0, .require_nbf = true };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_nbf,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: nbf in the future -> AXL_ERR");
    p_nbf.leeway_s = 60;
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_nbf,
                              &payload, &plen, NULL) == AXL_OK,
               "jwt_verify: future nbf within leeway -> AXL_OK");
    axl_free(payload); payload = NULL;
    axl_free(tok);

    /* require_exp but exp absent -> AXL_ERR. */
    tok = hs256_sign(secret, slen, "{\"iss\":\"axl\"}");
    AxlJwtPolicy p_req = { .now = NOW, .require_exp = true };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_req,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: require_exp with no exp -> AXL_ERR");
    axl_free(tok);

    /* iss mismatch -> AXL_ERR. */
    tok = hs256_sign(secret, slen, "{\"iss\":\"evil\"}");
    AxlJwtPolicy p_iss = { .expect_iss = "axl", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_iss,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: iss mismatch -> AXL_ERR");
    axl_free(tok);

    /* aud as a string: match and mismatch. */
    tok = hs256_sign(secret, slen, "{\"aud\":\"svc\"}");
    AxlJwtPolicy p_aud = { .expect_aud = "svc", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_aud,
                              &payload, &plen, NULL) == AXL_OK,
               "jwt_verify: aud string match -> AXL_OK");
    axl_free(payload); payload = NULL;
    p_aud.expect_aud = "other";
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_aud,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: aud string mismatch -> AXL_ERR");
    axl_free(tok);

    /* aud as an array: membership match and miss. */
    tok = hs256_sign(secret, slen, "{\"aud\":[\"a\",\"svc\",\"b\"]}");
    AxlJwtPolicy p_arr = { .expect_aud = "svc", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_arr,
                              &payload, &plen, NULL) == AXL_OK,
               "jwt_verify: aud array membership match -> AXL_OK");
    axl_free(payload); payload = NULL;
    p_arr.expect_aud = "nope";
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_arr,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: aud array no member matches -> AXL_ERR");
    axl_free(tok);

    /* aud array with a non-string member alongside the match -> AXL_OK
       (the non-string element is skipped, not a parse failure). */
    tok = hs256_sign(secret, slen, "{\"aud\":[123,\"svc\"]}");
    AxlJwtPolicy p_mix = { .expect_aud = "svc", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_mix,
                              &payload, &plen, NULL) == AXL_OK,
               "jwt_verify: aud array skips non-string member -> AXL_OK");
    axl_free(payload); payload = NULL;
    axl_free(tok);

    /* A non-string, non-array aud (a bare number) with expect_aud set
       fails closed. */
    tok = hs256_sign(secret, slen, "{\"aud\":42}");
    AxlJwtPolicy p_num = { .expect_aud = "svc", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_num,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: numeric aud -> AXL_ERR");
    axl_free(tok);

    /* expect_aud set but claim absent -> AXL_ERR. */
    tok = hs256_sign(secret, slen, "{\"iss\":\"axl\"}");
    AxlJwtPolicy p_noaud = { .expect_aud = "svc", .now = NOW };
    test_check(axl_jwt_verify(tok, axl_strlen(tok), &k, hs256, 1, &p_noaud,
                              &payload, &plen, NULL) == AXL_ERR,
               "jwt_verify: expect_aud with no aud claim -> AXL_ERR");
    axl_free(tok);

    /* A payload that is not a JSON OBJECT -> AXL_ERR, even with a policy that
       requires no claims at all.
       RFC 7519 §7.2 requires a JWT claims set to be an object. This became
       reachable when the reader stopped requiring an object-or-array root: a
       bare `42` now PARSES, and every axl_json_get_* against a non-object root
       returns false, which an empty policy cannot tell from "claim absent" --
       so a validly-signed non-JWT verified. The empty policy is the whole
       point of the case; with any required claim it would fail anyway and the
       test would pass for the wrong reason. */
    {
        AxlJwtPolicy p_any = { .now = NOW };
        struct { const char *body; const char *what; } nonobj[] = {
            { "42",      "jwt_verify: bare-number payload -> AXL_ERR" },
            { "\"abc\"", "jwt_verify: bare-string payload -> AXL_ERR" },
            { "null",    "jwt_verify: bare-null payload -> AXL_ERR" },
            { "true",    "jwt_verify: bare-true payload -> AXL_ERR" },
            { "[1,2]",   "jwt_verify: array payload -> AXL_ERR" },
        };
        for (size_t i = 0; i < sizeof(nonobj) / sizeof(nonobj[0]); i++) {
            char *t = hs256_sign(secret, slen, nonobj[i].body);
            test_check(t != NULL &&
                       axl_jwt_verify(t, axl_strlen(t), &k, hs256, 1, &p_any,
                                      &payload, &plen, NULL) == AXL_ERR,
                       nonobj[i].what);
            axl_free(t);
        }
        /* The control: the SAME empty policy must still accept a real object
           payload. Without this the five above would pass against a
           jwt_verify that rejected everything. */
        char *t_ok = hs256_sign(secret, slen, "{\"sub\":\"s\"}");
        test_check(t_ok != NULL &&
                   axl_jwt_verify(t_ok, axl_strlen(t_ok), &k, hs256, 1, &p_any,
                                  &payload, &plen, NULL) == AXL_OK,
                   "jwt_verify: object payload + empty policy -> AXL_OK");
        axl_free(payload); payload = NULL;
        axl_free(t_ok);
    }

    /* JOSE intake is STRICT RFC 8259, never AXL_JSON_RELAXED.
       These structures are attacker-influenced and RFC 7515/7517/7519 define
       them as ordinary JSON; a JOSE parser that also took JSON5 would let a
       second component reading the same signed bytes with a conforming parser
       derive a different document. Signed correctly, so only the dialect is
       under test. */
    {
        struct { const char *body; const char *what; } j5[] = {
            { "{/* c */\"sub\":\"s\"}", "jwt_verify: comment in payload -> AXL_ERR" },
            { "{sub:\"s\"}",            "jwt_verify: unquoted key in payload -> AXL_ERR" },
            { "{'sub':'s'}",            "jwt_verify: single quotes in payload -> AXL_ERR" },
            { "{\"sub\":\"s\",}",       "jwt_verify: trailing comma in payload -> AXL_ERR" },
            { "{\"n\":0x10}",           "jwt_verify: hex literal in payload -> AXL_ERR" },
        };
        AxlJwtPolicy p_any = { .now = NOW };
        for (size_t i = 0; i < sizeof(j5) / sizeof(j5[0]); i++) {
            char *t = hs256_sign(secret, slen, j5[i].body);
            test_check(t != NULL &&
                       axl_jwt_verify(t, axl_strlen(t), &k, hs256, 1, &p_any,
                                      &payload, &plen, NULL) == AXL_ERR,
                       j5[i].what);
            axl_free(t);
        }
    }
}

// ---------------------------------------------------------------------------
// JWK parse / export / set.
// ---------------------------------------------------------------------------

static void
test_jwk(void)
{
    /* EC export round-trip: keygen -> export public JWK (with kid) ->
       re-parse -> verify a token the original key signed. */
    AxlPkKey *key = axl_pk_key_new(AXL_PK_ECDSA_P256);
    char     *jwk = axl_jwk_export_public(key, "k1");
    test_check(jwk != NULL, "jwk_export_public: EC -> JSON");
    test_check(jwk != NULL && axl_strstr(jwk, "\"kty\":\"EC\"") != NULL
                   && axl_strstr(jwk, "\"crv\":\"P-256\"") != NULL,
               "jwk_export_public: EC carries kty/crv");
    /* Private material must never be exported. */
    test_check(jwk != NULL && axl_strstr(jwk, "\"d\"") == NULL,
               "jwk_export_public: omits private `d`");

    /* A JWK must be strict RFC 8259, not JSON5. Same reasoning as the JWT
       payload: a JWKS is fetched over the network, so this is adversary-
       reachable input, and RFC 7517 defines a JWK as a JSON object.

       Built by MUTATING the valid exported JWK above rather than hand-writing
       a stub, because the obvious stubs do not discriminate. `{kty:"EC"}` and
       `42` are rejected whatever the dialect -- they carry no usable key
       material, so axl_jwk_parse returns NULL on the missing-member path and
       the assertion passes even against the liberal parser it is meant to
       catch. (Verified: with the strict parse sabotaged back to
       axl_json_parse, stub-based assertions still passed. These do not.)

       Each mutation below is valid JSON5 and invalid RFC 8259, and otherwise
       a complete, parseable EC public key. */
    if (jwk != NULL) {
        const size_t jwk_len = axl_strlen(jwk);

        /* Leading block comment: object and members untouched. */
        AxlString *commented = axl_string_new("/* c */");
        axl_string_append(commented, jwk);
        test_check(axl_jwk_parse(axl_string_str(commented),
                                 axl_string_len(commented), NULL, NULL) == NULL,
                   "jwk_parse: leading comment (JSON5) -> NULL");
        axl_string_free(commented);

        /* Trailing comma before the closing brace. */
        if (jwk_len > 0 && jwk[jwk_len - 1] == '}') {
            AxlString *trailing = axl_string_new(NULL);
            axl_string_append_len(trailing, jwk, jwk_len - 1);
            axl_string_append(trailing, ",}");
            test_check(axl_jwk_parse(axl_string_str(trailing),
                                     axl_string_len(trailing),
                                     NULL, NULL) == NULL,
                       "jwk_parse: trailing comma (JSON5) -> NULL");
            axl_string_free(trailing);
        }

        /* Wrapped in an array: valid key material, but RFC 7517 says a JWK is
           an object. NOTE this one does NOT discriminate the strict parse --
           an array root makes every axl_json_get_* miss anyway, so it is
           rejected either way (confirmed under the sabotage run). Kept as a
           behavioral pin, not as evidence for the dialect: it would catch a
           future change that started accepting an array here. */
        AxlString *wrapped = axl_string_new("[");
        axl_string_append(wrapped, jwk);
        axl_string_append(wrapped, "]");
        test_check(axl_jwk_parse(axl_string_str(wrapped),
                                 axl_string_len(wrapped), NULL, NULL) == NULL,
                   "jwk_parse: array-wrapped JWK -> NULL");
        axl_string_free(wrapped);

        /* The control: unmutated, it must still parse. Without this the three
           above would pass against a jwk_parse that rejected everything. */
        AxlPkKey *control = axl_jwk_parse(jwk, jwk_len, NULL, NULL);
        test_check(control != NULL,
                   "jwk_parse: the unmutated JWK still parses");
        axl_pk_key_free(control);

        /* Same for a JWK Set, whose own root must be an object too. */
        AxlString *set = axl_string_new("{\"keys\":[");
        axl_string_append(set, jwk);
        axl_string_append(set, "]}");
        AxlJwks *ok_set = axl_jwks_parse(axl_string_str(set),
                                         axl_string_len(set));
        test_check(ok_set != NULL, "jwks_parse: strict JWK Set parses");
        axl_jwks_free(ok_set);

        AxlString *set5 = axl_string_new("/* c */{\"keys\":[");
        axl_string_append(set5, jwk);
        axl_string_append(set5, "]}");
        test_check(axl_jwks_parse(axl_string_str(set5),
                                  axl_string_len(set5)) == NULL,
                   "jwks_parse: leading comment (JSON5) -> NULL");
        axl_string_free(set5);
        axl_string_free(set);
    }

    char       *kid = NULL;
    AxlJoseAlg  alg = (AxlJoseAlg)0;
    AxlPkKey   *reparsed = axl_jwk_parse(jwk, axl_strlen(jwk), &kid, &alg);
    test_check(reparsed != NULL, "jwk_parse: re-parse exported EC JWK");
    test_check(kid != NULL && axl_strcmp(kid, "k1") == 0,
               "jwk_parse: recovers kid");
    test_check(alg == AXL_JOSE_ES256, "jwk_parse: EC infers ES256");

    /* Sign with the original, verify against the re-parsed public key. */
    {
        const char  msg[] = "{\"rt\":1}";
        AxlJoseKey  sk = { .pk = key };
        char       *tok = NULL;
        test_check(axl_jws_sign(&sk, AXL_JOSE_ES256, (const uint8_t *)msg,
                                axl_strlen(msg), &tok) == AXL_OK,
                   "es256: sign ok");
        AxlJoseKey   vk = { .pk = reparsed };
        const AxlJoseAlg allow[] = { AXL_JOSE_ES256 };
        uint8_t     *pl = NULL; size_t pn = 0;
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &vk, allow, 1,
                                         &pl, &pn) == AXL_OK,
                   "jwk round-trip: export->parse->verify EC token");
        axl_free(pl);
        axl_free(tok);
    }
    axl_free(jwk);
    axl_free(kid);
    axl_pk_key_free(reparsed);
    axl_pk_key_free(key);

    /* RSA export round-trip. */
    {
        AxlPkKey *rsa = axl_pk_key_new(AXL_PK_RSA);
        char     *rjwk = axl_jwk_export_public(rsa, NULL);
        test_check(rjwk != NULL && axl_strstr(rjwk, "\"kty\":\"RSA\"") != NULL
                       && axl_strstr(rjwk, "\"n\"") != NULL
                       && axl_strstr(rjwk, "\"e\"") != NULL,
                   "jwk_export_public: RSA carries kty/n/e");
        AxlJoseAlg ralg = (AxlJoseAlg)0;
        AxlPkKey  *rp = axl_jwk_parse(rjwk, axl_strlen(rjwk), NULL, &ralg);
        test_check(rp != NULL && ralg == AXL_JOSE_RS256,
                   "jwk_parse: RSA infers RS256");
        /* Round-trip a signature through the re-parsed modulus/exponent. */
        const char  msg[] = "{\"rt\":2}";
        AxlJoseKey  sk = { .pk = rsa };
        char       *tok = NULL;
        test_check(axl_jws_sign(&sk, AXL_JOSE_RS256, (const uint8_t *)msg,
                                axl_strlen(msg), &tok) == AXL_OK,
                   "rs256: sign ok");
        AxlJoseKey   vk = { .pk = rp };
        const AxlJoseAlg allow[] = { AXL_JOSE_RS256 };
        uint8_t *pl = NULL; size_t pn = 0;
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &vk, allow, 1,
                                         &pl, &pn) == AXL_OK,
                   "jwk round-trip: export->parse->verify RSA token");
        axl_free(pl);
        axl_free(tok);
        axl_free(rjwk);
        axl_pk_key_free(rp);
        axl_pk_key_free(rsa);
    }

    /* JWK Set: parse + find by kid. */
    {
        AxlPkKey *k1 = axl_pk_key_new(AXL_PK_ECDSA_P256);
        AxlPkKey *k2 = axl_pk_key_new(AXL_PK_ECDSA_P256);
        char     *j1 = axl_jwk_export_public(k1, "key-1");
        char     *j2 = axl_jwk_export_public(k2, "key-2");
        AxlString *set = axl_string_new("{\"keys\":[");
        axl_string_append(set, j1);
        axl_string_append(set, ",");
        axl_string_append(set, j2);
        axl_string_append(set, "]}");
        const char *sjson = axl_string_str(set);

        AxlJwks *jwks = axl_jwks_parse(sjson, axl_strlen(sjson));
        test_check(jwks != NULL, "jwks_parse: two-key set -> AxlJwks");
        test_check(jwks != NULL && axl_jwks_find(jwks, "key-1") != NULL,
                   "jwks_find: key-1 present");
        test_check(jwks != NULL && axl_jwks_find(jwks, "key-2") != NULL,
                   "jwks_find: key-2 present");
        test_check(jwks == NULL || axl_jwks_find(jwks, "absent") == NULL,
                   "jwks_find: unknown kid -> NULL");
        axl_jwks_free(jwks);
        axl_string_free(set);
        axl_free(j1); axl_free(j2);
        axl_pk_key_free(k1); axl_pk_key_free(k2);
    }

    /* Exact-string export KAT: re-export the A.3 public key and pin the
       canonical JWK byte-for-byte (kty/crv/x/y order, no kid). */
    {
        AxlPkKey *a3 = axl_jwk_parse(A3_ES256_JWK, axl_strlen(A3_ES256_JWK),
                                     NULL, NULL);
        char     *exported = axl_jwk_export_public(a3, NULL);
        test_check(exported != NULL
                       && axl_strcmp(exported, A3_ES256_JWK) == 0,
                   "jwk_export_public: A.3 EC key exports the canonical JWK");
        axl_free(exported);
        axl_pk_key_free(a3);
    }

    /* P-384 EC export round-trip: keygen -> export JWK -> re-parse ->
       verify an ES384 token the original key signed. */
    {
        AxlPkKey *p384 = axl_pk_key_new(AXL_PK_ECDSA_P384);
        char     *jwk = axl_jwk_export_public(p384, NULL);
        test_check(jwk != NULL && axl_strstr(jwk, "\"crv\":\"P-384\"") != NULL,
                   "jwk_export_public: P-384 carries crv P-384");
        AxlJoseAlg alg = (AxlJoseAlg)0;
        AxlPkKey  *rp = axl_jwk_parse(jwk, axl_strlen(jwk), NULL, &alg);
        test_check(rp != NULL && alg == AXL_JOSE_ES384,
                   "jwk_parse: re-parsed P-384 infers ES384");
        const char  msg[] = "{\"rt\":384}";
        AxlJoseKey  sk = { .pk = p384 };
        char       *tok = NULL;
        test_check(axl_jws_sign(&sk, AXL_JOSE_ES384, (const uint8_t *)msg,
                                axl_strlen(msg), &tok) == AXL_OK,
                   "es384: sign ok");
        AxlJoseKey   vk = { .pk = rp };
        const AxlJoseAlg allow[] = { AXL_JOSE_ES384 };
        uint8_t *pl = NULL; size_t pn = 0;
        test_check(tok != NULL
                       && axl_jws_verify(tok, axl_strlen(tok), &vk, allow, 1,
                                         &pl, &pn) == AXL_OK,
                   "jwk round-trip: export->parse->verify P-384 ES384 token");
        axl_free(pl);
        axl_free(tok);
        axl_free(jwk);
        axl_pk_key_free(rp);
        axl_pk_key_free(p384);
    }

    /* An off-curve EC point must be rejected (curve-membership check). */
    test_check(axl_jwk_parse(OFFCURVE_EC_JWK, axl_strlen(OFFCURVE_EC_JWK),
                             NULL, NULL) == NULL,
               "jwk_parse: off-curve EC point -> NULL");

    /* A sub-2048-bit RSA modulus is below the RS256 floor. */
    test_check(axl_jwk_parse(TINY_RSA_JWK, axl_strlen(TINY_RSA_JWK),
                             NULL, NULL) == NULL,
               "jwk_parse: sub-2048-bit RSA modulus -> NULL");

    /* Symmetric (oct) and malformed JWKs are not handled. */
    {
        const char *oct = "{\"kty\":\"oct\",\"k\":\"AAAA\"}";
        const char *junk = "{not json";
        test_check(axl_jwk_parse(oct, axl_strlen(oct), NULL, NULL) == NULL,
                   "jwk_parse: oct JWK -> NULL");
        test_check(axl_jwk_parse(junk, axl_strlen(junk), NULL, NULL) == NULL,
                   "jwk_parse: malformed JWK -> NULL");
    }
}


static int
test_jose_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlJose");

    test_jose_argval();
    test_jws_kat();
    test_jws_kat_es384_ps256();
    test_jws_rejections();
    test_jws_roundtrip();
    test_jwt();
    test_jwk();

    return test_print_results();
}

AXL_APP(test_jose_main)
