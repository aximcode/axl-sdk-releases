/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-auth.c
    Unit tests for the auth-hardening primitives: PBKDF2-HMAC-SHA256
    (axl-digest.h), constant-time compare (axl-crypto.h), and the
    server-side SCRAM-SHA-256 engine (axl-scram.h).

    All three are dependency-free (no AXL_TLS), so they run in the normal
    unit suite. PBKDF2 is pinned to RFC 7914 §11 vectors; SCRAM to the
    RFC 7677 `user`/`pencil` exchange via an internal server-nonce seam.
**/

#include <axl.h>
#include <axl/axl-scram.h>
#include "axl-test.h"
#include "axl-scram-internal.h"   /* _axl_scram_server_first_nonce test seam */

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Decode @p hex into @p out; returns the byte count (asserts on bad input). */
static size_t
unhex(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t n = axl_strlen(hex) / 2;
    if (n > out_cap) {
        n = out_cap;
    }
    for (size_t i = 0; i < n; i++) {
        char hi = hex[2 * i];
        char lo = hex[2 * i + 1];
        int h = (hi >= 'a') ? hi - 'a' + 10 : hi - '0';
        int l = (lo >= 'a') ? lo - 'a' + 10 : lo - '0';
        out[i] = (uint8_t)((h << 4) | l);
    }
    return n;
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256 — RFC 7914 §11 vectors (P="passwd", S="salt", c=1).
// dkLen=32 is the first block of the dkLen=64 output, so both pin the same
// construction and the 64-byte case also exercises multi-block concatenation.
// ---------------------------------------------------------------------------

static void
test_pbkdf2(void)
{
    static const char *V64 =
        "55ac046e56e3089fec1691c22544b605"
        "f94185216dde0465e68b9d57c20dacbc"
        "49ca9cccf179b645991664b39d77ef31"
        "7c71b845b1e30bd509112041d3a19783";

    uint8_t expected[64];
    unhex(V64, expected, sizeof expected);

    uint8_t out32[32];
    int rc = axl_pbkdf2_hmac_sha256((const uint8_t *)"passwd", 6,
                                    (const uint8_t *)"salt", 4,
                                    1, out32, sizeof out32);
    test_check(rc == AXL_OK && axl_memcmp(out32, expected, 32) == 0,
               "pbkdf2: RFC 7914 passwd/salt/c=1, dkLen=32");

    uint8_t out64[64];
    rc = axl_pbkdf2_hmac_sha256((const uint8_t *)"passwd", 6,
                                (const uint8_t *)"salt", 4,
                                1, out64, sizeof out64);
    test_check(rc == AXL_OK && axl_memcmp(out64, expected, 64) == 0,
               "pbkdf2: RFC 7914 passwd/salt/c=1, dkLen=64 (multi-block)");

    /* A higher iteration count changes the output (exercises U_k chaining). */
    uint8_t out_c2[32];
    rc = axl_pbkdf2_hmac_sha256((const uint8_t *)"passwd", 6,
                                (const uint8_t *)"salt", 4,
                                4096, out_c2, sizeof out_c2);
    test_check(rc == AXL_OK && axl_memcmp(out_c2, expected, 32) != 0,
               "pbkdf2: c=4096 differs from c=1");

    /* Argument validation. */
    test_check(axl_pbkdf2_hmac_sha256((const uint8_t *)"p", 1,
                                      (const uint8_t *)"s", 1,
                                      0, out32, sizeof out32) == AXL_INVALID,
               "pbkdf2: iterations==0 -> AXL_INVALID");
    test_check(axl_pbkdf2_hmac_sha256((const uint8_t *)"p", 1,
                                      (const uint8_t *)"s", 1,
                                      1, NULL, 0) == AXL_INVALID,
               "pbkdf2: NULL out / out_len==0 -> AXL_INVALID");
}

// ---------------------------------------------------------------------------
// Constant-time equality (correctness; the timing property is by construction).
// ---------------------------------------------------------------------------

static void
test_consttime(void)
{
    uint8_t a[32], b[32];
    for (size_t i = 0; i < 32; i++) {
        a[i] = (uint8_t)i;
        b[i] = (uint8_t)i;
    }

    test_check(axl_consttime_equal(a, b, 32),
               "consttime: equal buffers -> true");

    b[0] ^= 0x01;
    test_check(!axl_consttime_equal(a, b, 32),
               "consttime: differ in first byte -> false");
    b[0] = a[0];

    b[31] ^= 0x80;
    test_check(!axl_consttime_equal(a, b, 32),
               "consttime: differ in last byte -> false");
    b[31] = a[31];

    test_check(axl_consttime_equal(a, b, 0),
               "consttime: len==0 -> true");
    test_check(!axl_consttime_equal(NULL, b, 32),
               "consttime: NULL buffer with len>0 -> false");
}

// ---------------------------------------------------------------------------
// SCRAM-SHA-256 server engine — RFC 7677 §3 test vector (user "user",
// password "pencil"), driven through the server-nonce seam so the wire
// bytes match the RFC exactly, plus a random-nonce round-trip and tamper.
// ---------------------------------------------------------------------------

#define RFC_PASSWORD     "pencil"
#define RFC_SALT_B64     "W22ZaJ0SNY7soEsUEjb6gQ=="
#define RFC_ITERATIONS   4096
#define RFC_CLIENT_NONCE "rOprNGfwEbeRWgbNEkqO"
#define RFC_SERVER_NONCE "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0"
#define RFC_CLIENT_FIRST "n,,n=user,r=" RFC_CLIENT_NONCE
#define RFC_SERVER_FIRST \
    "r=" RFC_CLIENT_NONCE RFC_SERVER_NONCE \
    ",s=" RFC_SALT_B64 ",i=4096"
#define RFC_CLIENT_FINAL \
    "c=biws,r=" RFC_CLIENT_NONCE RFC_SERVER_NONCE \
    ",p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="
#define RFC_SERVER_FINAL "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="

/* Build the RFC credential into @p cred (salt kept in @p salt_buf). */
static void
rfc_credential(AxlScramCredential *cred, uint8_t *salt_buf, size_t salt_cap)
{
    void *salt = NULL;
    size_t salt_len = 0;
    axl_base64_decode(RFC_SALT_B64, &salt, &salt_len);
    if (salt_len > salt_cap) {
        salt_len = salt_cap;
    }
    axl_memcpy(salt_buf, salt, salt_len);
    axl_free(salt);

    cred->salt = salt_buf;
    cred->salt_len = salt_len;
    cred->iterations = RFC_ITERATIONS;
    (void)axl_scram_sha256_derive(RFC_PASSWORD, salt_buf, salt_len,
                                  RFC_ITERATIONS,
                                  cred->stored_key, cred->server_key);
}

static void
test_scram(void)
{
    uint8_t salt_buf[32];
    AxlScramCredential cred;
    rfc_credential(&cred, salt_buf, sizeof salt_buf);

    /* Step 1 with the RFC server nonce -> exact RFC server-first. */
    char server_first[AXL_SCRAM_MAX_MESSAGE];
    AxlScramState st;
    int rc = _axl_scram_server_first_nonce(
        &cred, RFC_CLIENT_FIRST, axl_strlen(RFC_CLIENT_FIRST),
        RFC_SERVER_NONCE, axl_strlen(RFC_SERVER_NONCE),
        server_first, sizeof server_first, &st);
    test_check(rc == AXL_OK,
               "scram: server_first (RFC nonce) -> AXL_OK");
    test_check(rc == AXL_OK && axl_strcmp(server_first, RFC_SERVER_FIRST) == 0,
               "scram: server-first matches RFC 7677 bytes");

    /* Step 2 with the RFC client-final -> auth OK, exact RFC server-final. */
    char server_final[AXL_SCRAM_SERVER_FINAL_MAX];
    rc = axl_scram_server_final(&st, RFC_CLIENT_FINAL, axl_strlen(RFC_CLIENT_FINAL),
                                server_final, sizeof server_final);
    test_check(rc == AXL_OK, "scram: server_final accepts RFC proof -> AXL_OK");
    test_check(rc == AXL_OK && axl_strcmp(server_final, RFC_SERVER_FINAL) == 0,
               "scram: server-final matches RFC 7677 bytes");

    /* Tampered proof -> AXL_DENIED, no oracle. Flip the FIRST proof char
       (an unconstrained base64 position) so the bytes still decode cleanly
       to 32 — a different but valid proof, exercising the verify, not the
       base64 parser. */
    char tampered[256];
    axl_strncpy(tampered, RFC_CLIENT_FINAL, sizeof tampered);
    char *pp = axl_strstr(tampered, ",p=");
    pp[3] = (pp[3] == 'A') ? 'B' : 'A';
    rc = axl_scram_server_final(&st, tampered, axl_strlen(tampered),
                                server_final, sizeof server_final);
    test_check(rc == AXL_DENIED, "scram: tampered proof -> AXL_DENIED");

    /* Wrong nonce in client-final -> AXL_DENIED. */
    rc = axl_scram_server_final(&st,
        "c=biws,r=wrongnonce,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
        62, server_final, sizeof server_final);
    test_check(rc == AXL_DENIED, "scram: wrong nonce -> AXL_DENIED");

    /* Reject a channel-binding gs2 header (no downgrade). */
    char sf2[AXL_SCRAM_MAX_MESSAGE];
    AxlScramState st2;
    rc = _axl_scram_server_first_nonce(&cred, "y,,n=user,r=abc", 15,
                                       RFC_SERVER_NONCE,
                                       axl_strlen(RFC_SERVER_NONCE),
                                       sf2, sizeof sf2, &st2);
    test_check(rc == AXL_INVALID, "scram: gs2 'y,,' (channel binding) -> AXL_INVALID");

    /* Public server_first (random nonce). Exercises axl_rng + the
       nonce-append path the seam bypasses. SKIP-balanced when the firmware
       has no EFI_RNG_PROTOCOL. */
    char sf3[AXL_SCRAM_MAX_MESSAGE];
    AxlScramState st3;
    int rrc = axl_scram_server_first(&cred, RFC_CLIENT_FIRST,
                                     axl_strlen(RFC_CLIENT_FIRST),
                                     sf3, sizeof sf3, &st3);
    if (rrc != AXL_OK) {
        test_skip_n(2, "axl_scram_server_first (no EFI_RNG_PROTOCOL)");
    } else {
        /* Client nonce preserved, then a fresh 24-char server nonce. */
        test_check(axl_strncmp(sf3, "r=" RFC_CLIENT_NONCE, 2 + 20) == 0,
                   "scram: public server-first preserves the client nonce");
        test_check(st3.combined_nonce_len
                       == axl_strlen(RFC_CLIENT_NONCE) + AXL_SCRAM_SERVER_NONCE_LEN,
                   "scram: public server-first appends a 24-char server nonce");
    }
}

// ---------------------------------------------------------------------------
// SCRAM-SHA-256 client engine — pinned to the RFC 7677 vector via the client
// nonce seam, plus a full client<->server round-trip with random nonces.
// ---------------------------------------------------------------------------

static void
test_scram_client(void)
{
    /* RFC 7677 client side, byte-exact (fixed client nonce via the seam). */
    AxlScramClientState cs;
    char client_first[AXL_SCRAM_MAX_MESSAGE];
    int rc = _axl_scram_client_first_nonce(
        "user", RFC_CLIENT_NONCE, axl_strlen(RFC_CLIENT_NONCE),
        client_first, sizeof client_first, &cs);
    test_check(rc == AXL_OK && axl_strcmp(client_first, RFC_CLIENT_FIRST) == 0,
               "scram client: client-first matches RFC 7677 bytes");

    char client_final[AXL_SCRAM_MAX_MESSAGE];
    rc = axl_scram_client_final(&cs, RFC_PASSWORD,
                                RFC_SERVER_FIRST, axl_strlen(RFC_SERVER_FIRST),
                                client_final, sizeof client_final);
    test_check(rc == AXL_OK && axl_strcmp(client_final, RFC_CLIENT_FINAL) == 0,
               "scram client: client-final (incl. proof) matches RFC 7677 bytes");

    test_check(axl_scram_client_verify(&cs, RFC_SERVER_FINAL,
                                       axl_strlen(RFC_SERVER_FINAL)) == AXL_OK,
               "scram client: verifies the RFC server-final");

    /* A tampered server-final is rejected (mutual-auth failure). Flip the
       first base64 char of v= so it still decodes to 32 bytes (a valid but
       wrong signature), exercising the compare, not the parser. */
    char tsf[64];
    axl_strncpy(tsf, RFC_SERVER_FINAL, sizeof tsf);
    tsf[2] = (tsf[2] == 'A') ? 'B' : 'A';
    test_check(axl_scram_client_verify(&cs, tsf, axl_strlen(tsf)) == AXL_DENIED,
               "scram client: tampered server-final -> AXL_DENIED");

    /* Full client<->server round-trip with random nonces, end to end. */
    uint8_t salt_buf[32];
    AxlScramCredential cred;
    rfc_credential(&cred, salt_buf, sizeof salt_buf);

    AxlScramClientState rc_cs;
    char cf[AXL_SCRAM_MAX_MESSAGE];
    int r1 = axl_scram_client_first("user", cf, sizeof cf, &rc_cs);
    if (r1 != AXL_OK) {
        test_skip_n(2, "scram round-trip (no EFI_RNG_PROTOCOL)");
        return;
    }

    AxlScramState ss;
    char sf[AXL_SCRAM_MAX_MESSAGE];
    int r2 = axl_scram_server_first(&cred, cf, axl_strlen(cf),
                                    sf, sizeof sf, &ss);
    char cfin[AXL_SCRAM_MAX_MESSAGE];
    int r3 = (r2 == AXL_OK)
        ? axl_scram_client_final(&rc_cs, RFC_PASSWORD, sf, axl_strlen(sf),
                                 cfin, sizeof cfin) : AXL_ERR;
    char sfin[AXL_SCRAM_SERVER_FINAL_MAX];
    int r4 = (r3 == AXL_OK)
        ? axl_scram_server_final(&ss, cfin, axl_strlen(cfin),
                                 sfin, sizeof sfin) : AXL_ERR;
    test_check(r4 == AXL_OK,
               "scram client: server accepts our proof in a live exchange");
    test_check(r4 == AXL_OK
                   && axl_scram_client_verify(&rc_cs, sfin, axl_strlen(sfin))
                          == AXL_OK,
               "scram client: we verify the server's signature in turn");
}

static int
test_auth_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlAuth");

    test_pbkdf2();
    test_consttime();
    test_scram();
    test_scram_client();

    return test_print_results();
}

AXL_APP(test_auth_main)
