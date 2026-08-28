/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file jose-demo.c
    Standalone consumer of <axl/axl-jose.h> — the same way an application
    would use it. Exercises the full lifecycle end to end:

      1. mint an ES256 server key, sign a JWT, then verify it against an
         allow-list and a registered-claim policy (the API-token / OIDC
         shape the module is built for);
      2. publish the public key as a JWK and verify a token against the
         re-parsed key (a JWKS-style key-distribution round-trip);
      3. show the allow-list rejecting a token whose `alg` is not permitted
         (the defense that makes algorithm-confusion impossible);
      4. round-trip every supported algorithm (ES256, ES384, RS256, PS256,
         HS256) so the demo dogfoods each signer/verifier.

    Build:
        ./scripts/install.sh --arch x64
        ./out/bin/axl-cc sdk/examples/jose-demo.c -o jose-demo.efi
    then run jose-demo.efi under run-qemu.sh. JOSE is compiled into every
    build; the axl_jose_available() check below is kept because a consumer
    should still branch on it rather than assume.
**/

#include <axl.h>

/* A fixed trusted clock so the demo is deterministic (a real consumer
   feeds NTP + a monotonic high-water mark, never the bare RTC). */
#define DEMO_NOW    1700000000

static int demo_failures = 0;

static void
check(bool ok, const char *what)
{
    axl_printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        demo_failures++;
    }
}

/* Sign @p payload with @p alg under @p key, verify it straight back with
   an allow-list of just that algorithm, and confirm the payload survives.
   The round-trip every supported signer/verifier must pass. */
static void
roundtrip(const char *label, const AxlJoseKey *key, AxlJoseAlg alg,
          const char *payload)
{
    char    *token = NULL;
    uint8_t *out = NULL;
    size_t   out_len = 0;
    const AxlJoseAlg allow[] = { alg };

    bool ok = axl_jws_sign(key, alg, (const uint8_t *)payload,
                           axl_strlen(payload), &token) == AXL_OK
              && token != NULL
              && axl_jws_verify(token, axl_strlen(token), key, allow, 1,
                                &out, &out_len) == AXL_OK
              && out_len == axl_strlen(payload)
              && axl_memcmp(out, payload, out_len) == 0;
    check(ok, label);

    axl_free(out);
    axl_free(token);
}

int
main(void)
{
    axl_printf("axl-jose demo\n");

    if (!axl_jose_available()) {
        axl_printf("JOSE not available in this build.\n");
        return 1;
    }

    // -----------------------------------------------------------------
    // 1. Mint an ES256 key, sign a JWT, verify it against a claim policy.
    // -----------------------------------------------------------------
    AxlPkKey *server = axl_pk_key_new(AXL_PK_ECDSA_P256);
    check(server != NULL, "minted an ES256 server key");

    AxlJoseKey signer = { .pk = server };
    const char *claims =
        "{\"iss\":\"axl-demo\",\"aud\":\"bmc\",\"sub\":\"admin\","
        "\"nbf\":1699999940,\"exp\":1700003600}";

    char *jwt = NULL;
    check(axl_jws_sign(&signer, AXL_JOSE_ES256, (const uint8_t *)claims,
                       axl_strlen(claims), &jwt) == AXL_OK && jwt != NULL,
          "signed an ES256 JWT");

    const AxlJoseAlg allow_es256[] = { AXL_JOSE_ES256 };
    AxlJwtPolicy policy = {
        .expect_iss = "axl-demo",
        .expect_aud = "bmc",
        .now        = DEMO_NOW,
        .leeway_s   = 30,
        .require_exp = true,
        .require_nbf = true,
    };
    uint8_t      *payload = NULL;
    size_t        payload_len = 0;
    AxlJsonReader parsed = { 0 };
    int rc = jwt != NULL
        ? axl_jwt_verify(jwt, axl_strlen(jwt), &signer, allow_es256, 1,
                         &policy, &payload, &payload_len, &parsed)
        : AXL_ERR;
    check(rc == AXL_OK, "verified the JWT (signature + iss/aud/exp/nbf)");

    if (rc == AXL_OK) {
        char sub[64] = { 0 };
        check(axl_json_get_string(&parsed, "sub", sub, sizeof(sub))
                  && axl_strcmp(sub, "admin") == 0,
              "read the `sub` claim back from the verified token");
        axl_printf("       authenticated subject: %s\n", sub);
        axl_json_free(&parsed);
        axl_free(payload);
    }

    // -----------------------------------------------------------------
    // 2. Publish the public key as a JWK, verify against the re-parsed key.
    // -----------------------------------------------------------------
    char *jwk = axl_jwk_export_public(server, "server-1");
    check(jwk != NULL, "exported the public key as a JWK");
    if (jwk != NULL) {
        axl_printf("       jwk: %s\n", jwk);
    }

    char       *kid = NULL;
    AxlJoseAlg  jwk_alg = (AxlJoseAlg)0;
    AxlPkKey   *imported = jwk != NULL
        ? axl_jwk_parse(jwk, axl_strlen(jwk), &kid, &jwk_alg) : NULL;
    check(imported != NULL && kid != NULL && axl_strcmp(kid, "server-1") == 0
              && jwk_alg == AXL_JOSE_ES256,
          "re-parsed the JWK (recovered kid + inferred ES256)");

    if (imported != NULL && jwt != NULL) {
        AxlJoseKey verifier = { .pk = imported };
        uint8_t   *p = NULL;
        size_t     pl = 0;
        check(axl_jws_verify(jwt, axl_strlen(jwt), &verifier, allow_es256, 1,
                             &p, &pl) == AXL_OK,
              "verified the original JWT against the published JWK");
        axl_free(p);
    }

    // -----------------------------------------------------------------
    // 3. The allow-list rejects a token whose alg is not permitted.
    // -----------------------------------------------------------------
    if (jwt != NULL) {
        const AxlJoseAlg only_rs256[] = { AXL_JOSE_RS256 };
        uint8_t *p = NULL;
        size_t   pl = 0;
        check(axl_jws_verify(jwt, axl_strlen(jwt), &signer, only_rs256, 1,
                             &p, &pl) == AXL_ERR,
              "rejected the ES256 token under an RS256-only allow-list");
    }

    axl_free(kid);
    axl_free(jwk);
    axl_free(jwt);
    axl_pk_key_free(imported);
    axl_pk_key_free(server);

    // -----------------------------------------------------------------
    // 4. Round-trip every supported algorithm.
    // -----------------------------------------------------------------
    const char *msg = "{\"hello\":\"jose\"}";

    AxlPkKey *es256 = axl_pk_key_new(AXL_PK_ECDSA_P256);
    AxlPkKey *es384 = axl_pk_key_new(AXL_PK_ECDSA_P384);
    AxlPkKey *rsa   = axl_pk_key_new(AXL_PK_RSA);
    const uint8_t hmac_secret[] = "demo-hmac-secret-0123456789abcd";

    AxlJoseKey k_es256 = { .pk = es256 };
    AxlJoseKey k_es384 = { .pk = es384 };
    AxlJoseKey k_rsa   = { .pk = rsa };
    AxlJoseKey k_hmac  = { .hmac_key = hmac_secret,
                           .hmac_key_len = sizeof(hmac_secret) - 1 };

    roundtrip("ES256 round-trip", &k_es256, AXL_JOSE_ES256, msg);
    roundtrip("ES384 round-trip", &k_es384, AXL_JOSE_ES384, msg);
    roundtrip("RS256 round-trip", &k_rsa,   AXL_JOSE_RS256, msg);
    roundtrip("PS256 round-trip", &k_rsa,   AXL_JOSE_PS256, msg);
    roundtrip("HS256 round-trip", &k_hmac,  AXL_JOSE_HS256, msg);

    axl_pk_key_free(es256);
    axl_pk_key_free(es384);
    axl_pk_key_free(rsa);

    if (demo_failures == 0) {
        axl_printf("jose-demo: all checks passed\n");
        return 0;
    }
    axl_printf("jose-demo: %d check(s) FAILED\n", demo_failures);
    return 1;
}
