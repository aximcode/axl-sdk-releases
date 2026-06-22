/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-scram.c
    SCRAM-SHA-256 server and client engines (RFC 5802 / RFC 7677).

    Plain SCRAM (gs2 header "n,,", no channel binding). Built on the
    dependency-free digest/HMAC/PBKDF2/base64/RNG primitives plus the
    constant-time compare — works in every build (no AXL_TLS).
**/

#include <axl/axl-scram.h>
#include "axl-scram-internal.h"
#include <axl/axl-digest.h>   /* axl_pbkdf2_hmac_sha256, axl_compute_checksum_digest */
#include <axl/axl-hmac.h>
#include <axl/axl-crypto.h>   /* axl_consttime_equal */
#include <axl/axl-rng.h>      /* axl_rng_bytes */
#include <axl/axl-mem.h>      /* axl_free */
#include <axl/axl-str.h>      /* mem*, str*, axl_snprintf, axl_base64_* */

// ===================================================================
// Helpers
// ===================================================================

/* HMAC-SHA256(key, msg) -> out[32]. Returns true on success. */
static bool
hmac256(const uint8_t *key, size_t key_len,
        const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    AxlHmac *h = axl_hmac_new(AXL_CHECKSUM_SHA256, key, key_len);
    if (h == NULL) {
        return false;
    }
    axl_hmac_update(h, msg, msg_len);
    size_t n = 32;
    axl_hmac_get_digest(h, out, &n);
    axl_hmac_free(h);
    return n == 32;
}

/* Length of a SCRAM attribute value: bytes until ',' or NUL. */
static size_t
attr_len(const char *p)
{
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ',') {
        n++;
    }
    return n;
}

// ===================================================================
// Credential derivation
// ===================================================================

int
axl_scram_sha256_derive(
    const char    *password,
    const uint8_t *salt,
    size_t         salt_len,
    uint32_t       iterations,
    uint8_t        stored_key[32],
    uint8_t        server_key[32]
    )
{
    if (password == NULL || iterations == 0 ||
        stored_key == NULL || server_key == NULL) {
        return AXL_INVALID;
    }

    uint8_t salted[32];
    int rc = axl_pbkdf2_hmac_sha256(
        (const uint8_t *)password, axl_strlen(password),
        salt, salt_len, iterations, salted, sizeof salted);
    if (rc != AXL_OK) {
        return rc;
    }

    /* ClientKey = HMAC(SaltedPassword, "Client Key"); StoredKey = SHA256(ClientKey). */
    uint8_t client_key[32];
    if (!hmac256(salted, sizeof salted, (const uint8_t *)"Client Key", 10,
                 client_key)) {
        return AXL_ERR;
    }
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, client_key,
                                    sizeof client_key, stored_key, 32) != AXL_OK) {
        return AXL_ERR;
    }

    /* ServerKey = HMAC(SaltedPassword, "Server Key"). */
    if (!hmac256(salted, sizeof salted, (const uint8_t *)"Server Key", 10,
                 server_key)) {
        return AXL_ERR;
    }
    return AXL_OK;
}

// ===================================================================
// Step 1: client-first -> server-first
// ===================================================================

int
_axl_scram_server_first_nonce(
    const AxlScramCredential *cred,
    const char              *client_first,
    size_t                   client_first_len,
    const char              *server_nonce,
    size_t                   server_nonce_len,
    char                    *out_server_first,
    size_t                   out_server_first_size,
    AxlScramState           *out_state
    )
{
    if (cred == NULL || client_first == NULL || server_nonce == NULL ||
        out_server_first == NULL || out_state == NULL) {
        return AXL_INVALID;
    }

    char cf[AXL_SCRAM_MAX_MESSAGE];
    if (client_first_len >= sizeof cf) {
        return AXL_INVALID;
    }
    axl_memcpy(cf, client_first, client_first_len);
    cf[client_first_len] = '\0';

    /* gs2 header must be exactly "n,," — no channel binding, no authzid. */
    if (client_first_len < 3 || cf[0] != 'n' || cf[1] != ',' || cf[2] != ',') {
        return AXL_INVALID;
    }
    const char *bare = cf + 3;
    /* client-first-bare must start with "n=" (rejects an "m=" mandatory
       extension and any other leading field). */
    if (bare[0] != 'n' || bare[1] != '=') {
        return AXL_INVALID;
    }

    const char *rpos = axl_strstr(bare, ",r=");
    if (rpos == NULL) {
        return AXL_INVALID;
    }
    const char *cnonce = rpos + 3;
    size_t cnonce_len = attr_len(cnonce);
    if (cnonce_len == 0 || server_nonce_len == 0) {
        return AXL_INVALID;
    }
    if (cnonce_len + server_nonce_len > AXL_SCRAM_MAX_NONCE) {
        return AXL_INVALID;
    }
    size_t bare_len = axl_strlen(bare);
    if (bare_len >= AXL_SCRAM_MAX_MESSAGE) {
        return AXL_INVALID;
    }

    /* combined nonce = client nonce ‖ server nonce */
    char combined[AXL_SCRAM_MAX_NONCE + 1];
    axl_memcpy(combined, cnonce, cnonce_len);
    axl_memcpy(combined + cnonce_len, server_nonce, server_nonce_len);
    size_t combined_len = cnonce_len + server_nonce_len;
    combined[combined_len] = '\0';

    char *salt_b64 = axl_base64_encode(cred->salt, cred->salt_len);
    if (salt_b64 == NULL) {
        return AXL_ERR;
    }
    int sf_len = axl_snprintf(out_server_first, out_server_first_size,
                              "r=%s,s=%s,i=%u",
                              combined, salt_b64, (unsigned)cred->iterations);
    axl_free(salt_b64);
    if (sf_len < 0 || (size_t)sf_len >= out_server_first_size ||
        (size_t)sf_len >= AXL_SCRAM_MAX_MESSAGE) {
        return AXL_INVALID;
    }

    axl_memset(out_state, 0, sizeof *out_state);
    axl_memcpy(out_state->stored_key, cred->stored_key, 32);
    axl_memcpy(out_state->server_key, cred->server_key, 32);
    axl_memcpy(out_state->client_first_bare, bare, bare_len);
    out_state->client_first_bare_len = (uint16_t)bare_len;
    axl_memcpy(out_state->server_first, out_server_first, (size_t)sf_len);
    out_state->server_first_len = (uint16_t)sf_len;
    axl_memcpy(out_state->combined_nonce, combined, combined_len);
    out_state->combined_nonce_len = (uint16_t)combined_len;
    return AXL_OK;
}

int
axl_scram_server_first(
    const AxlScramCredential *cred,
    const char              *client_first,
    size_t                   client_first_len,
    char                    *out_server_first,
    size_t                   out_server_first_size,
    AxlScramState           *out_state
    )
{
    /* Fresh server nonce: base64 of 18 random bytes = 24 printable,
       comma-free chars (AXL_SCRAM_SERVER_NONCE_LEN). */
    uint8_t rnd[18];
    if (axl_rng_bytes(rnd, sizeof rnd) != AXL_OK) {
        return AXL_ERR;
    }
    char *nonce = axl_base64_encode(rnd, sizeof rnd);
    if (nonce == NULL) {
        return AXL_ERR;
    }
    int rc = _axl_scram_server_first_nonce(
        cred, client_first, client_first_len, nonce, axl_strlen(nonce),
        out_server_first, out_server_first_size, out_state);
    axl_free(nonce);
    return rc;
}

// ===================================================================
// Step 2: client-final -> verify proof, server-final
// ===================================================================

int
axl_scram_server_final(
    const AxlScramState *state,
    const char         *client_final,
    size_t              client_final_len,
    char               *out_server_final,
    size_t              out_server_final_size
    )
{
    if (state == NULL || client_final == NULL || out_server_final == NULL) {
        return AXL_INVALID;
    }

    char cf[AXL_SCRAM_MAX_MESSAGE];
    if (client_final_len >= sizeof cf) {
        return AXL_INVALID;
    }
    axl_memcpy(cf, client_final, client_final_len);
    cf[client_final_len] = '\0';

    /* "c=biws," — base64("n,,"); reject any other channel-binding token. */
    if (axl_strncmp(cf, "c=biws,", 7) != 0) {
        return AXL_INVALID;
    }
    if (axl_strncmp(cf + 7, "r=", 2) != 0) {
        return AXL_INVALID;
    }
    const char *rval = cf + 9;
    size_t rlen = attr_len(rval);

    const char *ppos = axl_strstr(cf, ",p=");
    if (ppos == NULL) {
        return AXL_INVALID;
    }
    size_t cfwp_len = (size_t)(ppos - cf);   /* client-final-without-proof */
    const char *proof_b64 = ppos + 3;

    /* Nonce must match step 1 (mismatch is an auth failure, not an oracle). */
    if (rlen != state->combined_nonce_len ||
        axl_memcmp(rval, state->combined_nonce, rlen) != 0) {
        return AXL_DENIED;
    }

    void *proof = NULL;
    size_t proof_len = 0;
    if (axl_base64_decode(proof_b64, &proof, &proof_len) != AXL_OK) {
        return AXL_INVALID;
    }
    if (proof_len != 32) {
        axl_free(proof);
        return AXL_INVALID;
    }

    /* AuthMessage = client-first-bare ‖ "," ‖ server-first ‖ ","
       ‖ client-final-without-proof. */
    char auth[3 * AXL_SCRAM_MAX_MESSAGE];
    int al = axl_snprintf(auth, sizeof auth, "%.*s,%.*s,%.*s",
                          (int)state->client_first_bare_len, state->client_first_bare,
                          (int)state->server_first_len, state->server_first,
                          (int)cfwp_len, cf);
    if (al < 0 || (size_t)al >= sizeof auth) {
        axl_free(proof);
        return AXL_INVALID;
    }

    uint8_t client_sig[32];
    if (!hmac256(state->stored_key, 32, (const uint8_t *)auth, (size_t)al,
                 client_sig)) {
        axl_free(proof);
        return AXL_ERR;
    }
    /* ClientKey' = ClientProof XOR ClientSignature. */
    uint8_t client_key[32];
    for (size_t i = 0; i < 32; i++) {
        client_key[i] = ((const uint8_t *)proof)[i] ^ client_sig[i];
    }
    axl_free(proof);

    uint8_t verify[32];
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, client_key,
                                    sizeof client_key, verify, 32) != AXL_OK) {
        return AXL_ERR;
    }
    if (!axl_consttime_equal(verify, state->stored_key, 32)) {
        return AXL_DENIED;
    }

    /* ServerSignature = HMAC(ServerKey, AuthMessage). */
    uint8_t server_sig[32];
    if (!hmac256(state->server_key, 32, (const uint8_t *)auth, (size_t)al,
                 server_sig)) {
        return AXL_ERR;
    }
    char *sig_b64 = axl_base64_encode(server_sig, sizeof server_sig);
    if (sig_b64 == NULL) {
        return AXL_ERR;
    }
    int sl = axl_snprintf(out_server_final, out_server_final_size, "v=%s", sig_b64);
    axl_free(sig_b64);
    if (sl < 0 || (size_t)sl >= out_server_final_size) {
        return AXL_INVALID;
    }
    return AXL_OK;
}

// ===================================================================
// Client engine (the peer of the server engine above)
// ===================================================================

/* Parse a base-10 uint32 of @p len digits. */
static bool
parse_u32(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 9) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

int
_axl_scram_client_first_nonce(
    const char          *username,
    const char          *client_nonce,
    size_t               client_nonce_len,
    char                *out_client_first,
    size_t               out_size,
    AxlScramClientState *out_state
    )
{
    if (username == NULL || client_nonce == NULL || out_client_first == NULL ||
        out_state == NULL) {
        return AXL_INVALID;
    }
    if (username[0] == '\0' || client_nonce_len == 0 ||
        client_nonce_len > AXL_SCRAM_MAX_NONCE) {
        return AXL_INVALID;
    }
    /* SCRAM reserves ',' and '='; reject rather than implement =2C/=3D. */
    for (const char *u = username; *u != '\0'; u++) {
        if (*u == ',' || *u == '=') {
            return AXL_INVALID;
        }
    }

    int n = axl_snprintf(out_client_first, out_size, "n,,n=%s,r=%.*s",
                         username, (int)client_nonce_len, client_nonce);
    if (n < 0 || (size_t)n >= out_size) {
        return AXL_INVALID;
    }

    axl_memset(out_state, 0, sizeof *out_state);
    /* client-first-bare = everything after the "n,," gs2 header. */
    const char *bare = out_client_first + 3;
    size_t bare_len = axl_strlen(bare);
    if (bare_len >= AXL_SCRAM_MAX_MESSAGE) {
        return AXL_INVALID;
    }
    axl_memcpy(out_state->client_first_bare, bare, bare_len);
    out_state->client_first_bare_len = (uint16_t)bare_len;
    axl_memcpy(out_state->client_nonce, client_nonce, client_nonce_len);
    out_state->client_nonce_len = (uint16_t)client_nonce_len;
    return AXL_OK;
}

int
axl_scram_client_first(
    const char          *username,
    char                *out_client_first,
    size_t               out_size,
    AxlScramClientState *out_state
    )
{
    uint8_t rnd[18];
    if (axl_rng_bytes(rnd, sizeof rnd) != AXL_OK) {
        return AXL_ERR;
    }
    char *nonce = axl_base64_encode(rnd, sizeof rnd);
    if (nonce == NULL) {
        return AXL_ERR;
    }
    int rc = _axl_scram_client_first_nonce(
        username, nonce, axl_strlen(nonce), out_client_first, out_size, out_state);
    axl_free(nonce);
    return rc;
}

int
axl_scram_client_final(
    AxlScramClientState *state,
    const char          *password,
    const char          *server_first,
    size_t               server_first_len,
    char                *out_client_final,
    size_t               out_size
    )
{
    if (state == NULL || password == NULL || server_first == NULL ||
        out_client_final == NULL) {
        return AXL_INVALID;
    }

    char sf[AXL_SCRAM_MAX_MESSAGE];
    if (server_first_len >= sizeof sf) {
        return AXL_INVALID;
    }
    axl_memcpy(sf, server_first, server_first_len);
    sf[server_first_len] = '\0';

    /* Parse "r=<combined>,s=<base64 salt>,i=<iterations>". */
    if (sf[0] != 'r' || sf[1] != '=') {
        return AXL_INVALID;
    }
    const char *combined = sf + 2;
    size_t combined_len = attr_len(combined);
    const char *spos = axl_strstr(sf, ",s=");
    const char *ipos = axl_strstr(sf, ",i=");
    if (spos == NULL || ipos == NULL) {
        return AXL_INVALID;
    }
    const char *salt_b64 = spos + 3;
    size_t salt_b64_len = attr_len(salt_b64);
    uint32_t iterations = 0;
    if (!parse_u32(ipos + 3, attr_len(ipos + 3), &iterations) ||
        iterations == 0) {
        return AXL_INVALID;
    }

    /* The server must echo our nonce as the prefix of the combined one. */
    if (combined_len < state->client_nonce_len ||
        axl_memcmp(combined, state->client_nonce, state->client_nonce_len) != 0) {
        return AXL_INVALID;
    }

    /* Decode the salt (copy the field out so it is NUL-terminated). */
    char salt_buf[128];
    if (salt_b64_len == 0 || salt_b64_len >= sizeof salt_buf) {
        return AXL_INVALID;
    }
    axl_memcpy(salt_buf, salt_b64, salt_b64_len);
    salt_buf[salt_b64_len] = '\0';
    void *salt = NULL;
    size_t salt_len = 0;
    if (axl_base64_decode(salt_buf, &salt, &salt_len) != AXL_OK) {
        return AXL_INVALID;
    }

    /* SaltedPassword -> ClientKey / StoredKey / ServerKey. */
    uint8_t salted[32], client_key[32], stored_key[32], server_key[32];
    int rc = axl_pbkdf2_hmac_sha256((const uint8_t *)password,
                                    axl_strlen(password), salt, salt_len,
                                    iterations, salted, sizeof salted);
    axl_free(salt);
    if (rc != AXL_OK ||
        !hmac256(salted, 32, (const uint8_t *)"Client Key", 10, client_key) ||
        axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, client_key, 32,
                                    stored_key, 32) != AXL_OK ||
        !hmac256(salted, 32, (const uint8_t *)"Server Key", 10, server_key)) {
        return AXL_ERR;
    }

    /* client-final-without-proof = "c=biws,r=<combined>". */
    char cfwp[AXL_SCRAM_MAX_MESSAGE];
    int cfwp_len = axl_snprintf(cfwp, sizeof cfwp, "c=biws,r=%.*s",
                                (int)combined_len, combined);
    if (cfwp_len < 0 || (size_t)cfwp_len >= sizeof cfwp) {
        return AXL_INVALID;
    }

    /* AuthMessage = client-first-bare ‖ "," ‖ server-first ‖ "," ‖ cfwp.
       Worst case = 3 fields each < AXL_SCRAM_MAX_MESSAGE + 2 commas + NUL,
       so 3*AXL_SCRAM_MAX_MESSAGE bytes covers it; the guard below is the
       real backstop if any field sizing ever changes. */
    char auth[3 * AXL_SCRAM_MAX_MESSAGE];
    int al = axl_snprintf(auth, sizeof auth, "%.*s,%s,%s",
                          (int)state->client_first_bare_len,
                          state->client_first_bare, sf, cfwp);
    if (al < 0 || (size_t)al >= sizeof auth) {
        return AXL_INVALID;
    }

    /* ClientProof = ClientKey XOR HMAC(StoredKey, AuthMessage). */
    uint8_t client_sig[32];
    if (!hmac256(stored_key, 32, (const uint8_t *)auth, (size_t)al, client_sig)) {
        return AXL_ERR;
    }
    uint8_t proof[32];
    for (size_t i = 0; i < 32; i++) {
        proof[i] = client_key[i] ^ client_sig[i];
    }
    char *proof_b64 = axl_base64_encode(proof, sizeof proof);
    if (proof_b64 == NULL) {
        return AXL_ERR;
    }
    int n = axl_snprintf(out_client_final, out_size, "%s,p=%s", cfwp, proof_b64);
    axl_free(proof_b64);
    if (n < 0 || (size_t)n >= out_size) {
        return AXL_INVALID;
    }

    /* Stash the expected ServerSignature for axl_scram_client_verify. */
    if (!hmac256(server_key, 32, (const uint8_t *)auth, (size_t)al,
                 state->server_signature)) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_scram_client_verify(
    const AxlScramClientState *state,
    const char               *server_final,
    size_t                    server_final_len
    )
{
    if (state == NULL || server_final == NULL) {
        return AXL_INVALID;
    }
    char sf[AXL_SCRAM_SERVER_FINAL_MAX];
    if (server_final_len >= sizeof sf) {
        return AXL_INVALID;
    }
    axl_memcpy(sf, server_final, server_final_len);
    sf[server_final_len] = '\0';

    if (sf[0] != 'v' || sf[1] != '=') {
        return AXL_INVALID;
    }
    void *sig = NULL;
    size_t sig_len = 0;
    if (axl_base64_decode(sf + 2, &sig, &sig_len) != AXL_OK) {
        return AXL_INVALID;
    }
    if (sig_len != 32) {
        axl_free(sig);
        return AXL_INVALID;
    }
    bool ok = axl_consttime_equal(sig, state->server_signature, 32);
    axl_free(sig);
    return ok ? AXL_OK : AXL_DENIED;
}
