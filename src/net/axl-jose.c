/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-jose.c
    JOSE — JWS Compact (RFC 7515), JWT (RFC 7519), JWK/JWK-Set (RFC 7517).

    Built on the AxlCrypto key-handle API (ES256/ES384/RS256/PS256),
    AxlHmac (HS256), AxlStr base64url, and the AxlJson reader/writer.
    mbedTLS is an unconditional dependency, so the module is in every
    build and axl_jose_available() is a guaranteed true.

    All five algorithms are implemented: ES256, ES384, RS256, PS256 and
    HS256. An algorithm outside the caller's allow-list is rejected
    (fail closed), never silently downgraded.

    Security model lives in the header: verification is allow-list-driven
    (never header-`alg`-driven), `none` is unrepresentable, and the key is
    bound to the algorithm family (HMAC secret vs public key are separate
    fields), so the RS256<->HS256 confusion is structurally impossible.
**/

#include <axl/axl-jose.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-hmac.h>
#include <axl/axl-json.h>
#include "axl-crypto-internal.h"   /* raw EC/RSA key import/export (JWK) */

#define JOSE_SHA256_LEN   32u   /* HS256 digest / HMAC tag size. */
#define JOSE_EC_COORD_MAX 48u   /* widest EC affine coordinate (P-384). */
#define JOSE_RSA_N_MAX    512u  /* modulus cap: 4096-bit RSA = 512 bytes. */

bool
axl_jose_available(void)
{
    return true;
}

// -------------------------------------------------------------------
// JSON intake — strict, and always a JSON object.
// -------------------------------------------------------------------

/* Parse a JOSE structure: a JWS protected header, a JWT claims set, a JWK,
 * or a JWK Set.
 *
 * AXL_JSON_STRICT, deliberately, and not AXL_JSON_RELAXED -- the dialect the
 * SDK reserves for sidecars and config files it did not write. Every
 * document reaching this function is the opposite case: base64url-decoded
 * bytes from a token or a JWKS endpoint, i.e. attacker-influenced, and
 * RFC 7515/7517/7519 all define these structures as ordinary JSON. A JOSE
 * parser that also accepted comments, unquoted keys, single-quoted strings
 * and hex literals would be a parser-differential hazard: the JWS signature
 * covers the base64url TEXT, not our token tree, so any other component
 * validating the same authenticated bytes with a conforming parser could read
 * a different document out of them. AXL_JSON_STRICT is exactly the
 * "attacker-influenced input" case axl-json.h tells callers to use it for.
 *
 * The object requirement is the second half, and it is not redundant. A JSON
 * text is any value (RFC 8259 §2), so `42` and `"x"` parse -- but RFC 7519
 * §7.2 requires a JWT claims set to be a JSON OBJECT, and RFC 7517 the same
 * for a JWK. Without this check a validly-signed token whose payload decodes
 * to `42` verifies: every axl_json_get_* against a non-object root returns
 * false, which a policy requiring no claims cannot distinguish from "claim
 * absent". Under a container-root-only reader that was impossible; once the
 * reader started accepting a bare primitive it had to be said out loud.
 *
 * Spelled as a byte test rather than a reader predicate because AxlJson has
 * no root-type accessor. Safe and exact: under AXL_JSON_STRICT the only bytes
 * that may precede the root are RFC 8259 whitespace, so the first byte that
 * is not one of those four IS the root's first byte.
 *
 * @return true if @a json is a strict-JSON object, with @a r filled.
 */
static bool
jose_parse_object(const char *json, size_t len, AxlJsonReader *r)
{
    if (!axl_json_parse(json, len, AXL_JSON_STRICT, r)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = json[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c == '{') {
            return true;
        }
        break;
    }
    axl_json_free(r);
    return false;
}

// -------------------------------------------------------------------
// Algorithm names + family.
// -------------------------------------------------------------------

static const char *
alg_name(AxlJoseAlg a)
{
    switch (a) {
    case AXL_JOSE_ES256: return "ES256";
    case AXL_JOSE_ES384: return "ES384";
    case AXL_JOSE_RS256: return "RS256";
    case AXL_JOSE_PS256: return "PS256";
    case AXL_JOSE_HS256: return "HS256";
    }
    return NULL;
}

static bool
alg_from_name(const char *s, AxlJoseAlg *out)
{
    if (axl_strcmp(s, "ES256") == 0)      { *out = AXL_JOSE_ES256; }
    else if (axl_strcmp(s, "ES384") == 0) { *out = AXL_JOSE_ES384; }
    else if (axl_strcmp(s, "RS256") == 0) { *out = AXL_JOSE_RS256; }
    else if (axl_strcmp(s, "PS256") == 0) { *out = AXL_JOSE_PS256; }
    else if (axl_strcmp(s, "HS256") == 0) { *out = AXL_JOSE_HS256; }
    else { return false; }
    return true;
}

static bool
alg_is_symmetric(AxlJoseAlg a)
{
    return a == AXL_JOSE_HS256;
}

static bool
alg_allowed(AxlJoseAlg a, const AxlJoseAlg *allowed, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (allowed[i] == a) {
            return true;
        }
    }
    return false;
}

/* An allow-list mixing a symmetric algorithm with an asymmetric one would
   let the token's own `alg` choose the key family — exactly what the list
   exists to prevent. Reject it. */
static bool
allow_list_mixed(const AxlJoseAlg *allowed, size_t n)
{
    bool sym = false, asym = false;
    for (size_t i = 0; i < n; i++) {
        if (alg_is_symmetric(allowed[i])) {
            sym = true;
        } else {
            asym = true;
        }
    }
    return sym && asym;
}

/* Constant-time equality over @p n bytes — no early-out on the first
   differing byte, so an attacker cannot time their way to a valid tag. */
static bool
ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* HMAC-SHA-256(@p key, @p data) -> @p out (JOSE_SHA256_LEN bytes). The
   raw-digest peer of axl_compute_hmac (which returns hex). */
static bool
jose_hmac_sha256(const uint8_t *key, size_t key_len,
                 const char *data, size_t len, uint8_t out[JOSE_SHA256_LEN])
{
    AxlHmac *h = axl_hmac_new(AXL_CHECKSUM_SHA256, key, key_len);
    if (h == NULL) {
        return false;
    }
    axl_hmac_update(h, data, len);
    size_t out_len = JOSE_SHA256_LEN;
    axl_hmac_get_digest(h, out, &out_len);
    axl_hmac_free(h);
    return out_len == JOSE_SHA256_LEN;
}

// -------------------------------------------------------------------
// JWS compact framing.
// -------------------------------------------------------------------

/* Locate the two '.' separators of a JWS compact token. On success
   seg0=[0,*dot1), seg1=[*dot1+1,*dot2), seg2=[*dot2+1,len). Requires
   exactly two dots and non-empty header / payload / signature segments
   (an empty signature segment is the alg:none form, rejected here). */
static bool
jws_split(const char *t, size_t len, size_t *dot1, size_t *dot2)
{
    size_t d1 = 0, d2 = 0;
    int    dots = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '.') {
            dots++;
            if (dots == 1)      { d1 = i; }
            else if (dots == 2) { d2 = i; }
            else                { return false; }  /* a third dot */
        }
    }
    if (dots != 2 || d1 == 0 || d2 == d1 + 1 || d2 + 1 >= len) {
        return false;
    }
    *dot1 = d1;
    *dot2 = d2;
    return true;
}

/* Decode a JWS protected header (base64url JSON) and extract its `alg`
   into @p alg_out. Returns false on a decode/parse error or an unknown
   `alg`. */
static bool
header_alg(const char *seg, size_t seg_len, AxlJoseAlg *alg_out)
{
    void  *hdr = NULL;
    size_t hdr_len = 0;
    if (axl_base64url_decode(seg, seg_len, &hdr, &hdr_len) != AXL_OK) {
        return false;
    }

    AxlJsonReader r = { 0 };
    bool          ok = false;
    if (jose_parse_object((const char *)hdr, hdr_len, &r)) {
        char alg[16] = { 0 };
        if (axl_json_get_string(&r, "alg", alg, sizeof(alg))) {
            ok = alg_from_name(alg, alg_out);
        }
        axl_json_free(&r);
    }
    axl_free(hdr);
    return ok;
}

/* Expected AxlPkKey algorithm for an asymmetric JOSE algorithm whose
   signature goes through the public axl_pk_key_sign/_verify path. PS256
   also uses an RSA key but a distinct PSS scheme, so it is handled
   separately and is intentionally absent here. */
static bool
pk_alg_for(AxlJoseAlg a, AxlPkAlg *out)
{
    switch (a) {
    case AXL_JOSE_ES256: *out = AXL_PK_ECDSA_P256; return true;
    case AXL_JOSE_ES384: *out = AXL_PK_ECDSA_P384; return true;
    case AXL_JOSE_RS256: *out = AXL_PK_RSA;        return true;
    default:             return false;  /* HS256/PS256 handled elsewhere */
    }
}

// ===================================================================
// JWS sign.
// ===================================================================

/* Compute the signature segment (base64url) over @p signing_input for the
   selected @p alg. Returns the encoded segment (caller frees) or NULL. */
static char *
jws_make_sig(const AxlJoseKey *key, AxlJoseAlg alg,
             const char *signing_input, size_t si_len)
{
    if (alg == AXL_JOSE_HS256) {
        uint8_t mac[JOSE_SHA256_LEN];
        if (key->hmac_key == NULL
            || !jose_hmac_sha256(key->hmac_key, key->hmac_key_len,
                                 signing_input, si_len, mac)) {
            return NULL;
        }
        return axl_base64url_encode(mac, JOSE_SHA256_LEN);
    }

    /* PS256 is RSA-PSS — a distinct scheme over an RSA key, so it binds to
       an RSA key but signs through the dedicated PSS helper. */
    bool pss = (alg == AXL_JOSE_PS256);
    if (pss) {
        if (key->pk == NULL || axl_pk_key_alg(key->pk) != AXL_PK_RSA) {
            return NULL;
        }
    } else {
        AxlPkAlg pk_alg;
        if (!pk_alg_for(alg, &pk_alg) || key->pk == NULL
            || axl_pk_key_alg(key->pk) != pk_alg) {
            return NULL;
        }
    }

    AxlPkSigFormat fmt = (alg == AXL_JOSE_ES256 || alg == AXL_JOSE_ES384)
                             ? AXL_PK_SIG_RAW : AXL_PK_SIG_DER;
    const uint8_t *si = (const uint8_t *)signing_input;
    size_t         sig_len = 0;
    int            rc = pss
        ? axl_pk_rsa_pss_sha256_sign(key->pk, si, si_len, NULL, &sig_len)
        : axl_pk_key_sign(key->pk, si, si_len, fmt, NULL, &sig_len);
    if (rc != AXL_OK || sig_len == 0) {
        return NULL;
    }
    uint8_t *sig = axl_malloc(sig_len);
    if (sig == NULL) {
        return NULL;
    }
    rc = pss
        ? axl_pk_rsa_pss_sha256_sign(key->pk, si, si_len, sig, &sig_len)
        : axl_pk_key_sign(key->pk, si, si_len, fmt, sig, &sig_len);
    char *enc = (rc == AXL_OK) ? axl_base64url_encode(sig, sig_len) : NULL;
    axl_free(sig);
    return enc;
}

int
axl_jws_sign(const AxlJoseKey *key, AxlJoseAlg alg,
             const uint8_t *payload, size_t payload_len, char **token_out)
{
    const char *name = alg_name(alg);
    if (key == NULL || token_out == NULL || name == NULL
        || (payload == NULL && payload_len != 0)) {
        return AXL_ERR;
    }

    int rc = AXL_ERR;

    /* Minimal protected header: {"alg":"<name>","typ":"JWT"}. */
    AxlString *hdr = axl_string_new("{\"alg\":\"");
    axl_string_append(hdr, name);
    axl_string_append(hdr, "\",\"typ\":\"JWT\"}");

    char *hdr_b64 = axl_base64url_encode(axl_string_str(hdr),
                                         axl_string_len(hdr));
    char *pl_b64  = axl_base64url_encode(payload, payload_len);
    axl_string_free(hdr);
    if (hdr_b64 == NULL || pl_b64 == NULL) {
        axl_free(hdr_b64);
        axl_free(pl_b64);
        return AXL_ERR;
    }

    /* signing input = BASE64URL(header) "." BASE64URL(payload). */
    AxlString *tok = axl_string_new(hdr_b64);
    axl_string_append_c(tok, '.');
    axl_string_append(tok, pl_b64);
    axl_free(hdr_b64);
    axl_free(pl_b64);

    char *sig_b64 = jws_make_sig(key, alg, axl_string_str(tok),
                                 axl_string_len(tok));
    if (sig_b64 != NULL) {
        axl_string_append_c(tok, '.');
        axl_string_append(tok, sig_b64);
        axl_free(sig_b64);
        *token_out = axl_strdup(axl_string_str(tok));
        rc = (*token_out != NULL) ? AXL_OK : AXL_ERR;
    }
    axl_string_free(tok);
    return rc;
}

// ===================================================================
// JWS verify.
// ===================================================================

/* Verify the signature segment @p sig (raw bytes) over @p signing_input
   for @p alg under @p key. Returns AXL_OK only on a valid signature. */
static int
jws_check_sig(const AxlJoseKey *key, AxlJoseAlg alg,
              const char *signing_input, size_t si_len,
              const uint8_t *sig, size_t sig_len)
{
    if (alg == AXL_JOSE_HS256) {
        uint8_t mac[JOSE_SHA256_LEN];
        if (key->hmac_key == NULL || sig_len != JOSE_SHA256_LEN
            || !jose_hmac_sha256(key->hmac_key, key->hmac_key_len,
                                 signing_input, si_len, mac)) {
            return AXL_ERR;
        }
        return ct_equal(mac, sig, JOSE_SHA256_LEN) ? AXL_OK : AXL_ERR;
    }

    /* PS256: RSA-PSS over an RSA key (a distinct scheme from RS256's
       PKCS#1 v1.5, so a PS256 token never verifies under an RS256
       allow-list and vice versa). */
    if (alg == AXL_JOSE_PS256) {
        if (key->pk == NULL || axl_pk_key_alg(key->pk) != AXL_PK_RSA) {
            return AXL_ERR;
        }
        return axl_pk_rsa_pss_sha256_verify(
            key->pk, (const uint8_t *)signing_input, si_len, sig, sig_len);
    }

    AxlPkAlg pk_alg;
    if (!pk_alg_for(alg, &pk_alg) || key->pk == NULL
        || axl_pk_key_alg(key->pk) != pk_alg) {
        return AXL_ERR;
    }
    AxlPkSigFormat fmt = (alg == AXL_JOSE_ES256 || alg == AXL_JOSE_ES384)
                             ? AXL_PK_SIG_RAW : AXL_PK_SIG_DER;
    return axl_pk_key_verify(key->pk, (const uint8_t *)signing_input, si_len,
                             fmt, sig, sig_len);
}

int
axl_jws_verify(const char *token, size_t token_len, const AxlJoseKey *key,
               const AxlJoseAlg *allowed, size_t n_allowed,
               uint8_t **payload_out, size_t *payload_len_out)
{
    if (token == NULL || token_len == 0 || key == NULL || allowed == NULL
        || n_allowed == 0 || payload_out == NULL || payload_len_out == NULL
        || allow_list_mixed(allowed, n_allowed)) {
        return AXL_ERR;
    }

    size_t d1, d2;
    if (!jws_split(token, token_len, &d1, &d2)) {
        return AXL_ERR;
    }

    AxlJoseAlg alg;
    if (!header_alg(token, d1, &alg) || !alg_allowed(alg, allowed, n_allowed)) {
        return AXL_ERR;
    }

    /* Decode the signature segment. */
    void  *sig = NULL;
    size_t sig_len = 0;
    if (axl_base64url_decode(token + d2 + 1, token_len - d2 - 1,
                             &sig, &sig_len) != AXL_OK) {
        return AXL_ERR;
    }

    /* signing input is the raw header.payload bytes (offsets [0, d2)). */
    int rc = jws_check_sig(key, alg, token, d2, sig, sig_len);
    axl_free(sig);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }

    /* Authentic — now decode the payload for the caller. */
    void  *payload = NULL;
    size_t payload_len = 0;
    if (axl_base64url_decode(token + d1 + 1, d2 - d1 - 1,
                             &payload, &payload_len) != AXL_OK) {
        return AXL_ERR;
    }
    *payload_out = (uint8_t *)payload;
    *payload_len_out = payload_len;
    return AXL_OK;
}

// ===================================================================
// JWT — registered-claim validation atop JWS.
// ===================================================================

/* Check the `aud` claim against @p expect_aud: a string value must equal
   it, or, if `aud` is an array, one member must. A missing or non-string
   `aud` fails. */
static bool
jwt_check_aud(const AxlJsonReader *claims, const char *expect_aud)
{
    char one[256];
    if (axl_json_get_string(claims, "aud", one, sizeof(one))) {
        return axl_strcmp(one, expect_aud) == 0;
    }

    AxlJsonArrayIter it;
    if (axl_json_array_begin(claims, "aud", &it)) {
        AxlJsonReader el;
        while (axl_json_array_next(&it, &el)) {
            char v[256];
            if (axl_json_value_string(&el, v, sizeof(v))
                && axl_strcmp(v, expect_aud) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Validate the registered claims in @p claims against @p policy. */
static bool
jwt_check_claims(const AxlJsonReader *claims, const AxlJwtPolicy *policy)
{
    int64_t v;

    /* exp: token is valid while now <= exp + leeway. */
    bool have_exp = axl_json_get_int(claims, "exp", &v);
    if (policy->require_exp && !have_exp) {
        return false;
    }
    if (have_exp && policy->now > v + policy->leeway_s) {
        return false;
    }

    /* nbf: token is valid while now >= nbf - leeway. */
    bool have_nbf = axl_json_get_int(claims, "nbf", &v);
    if (policy->require_nbf && !have_nbf) {
        return false;
    }
    if (have_nbf && policy->now + policy->leeway_s < v) {
        return false;
    }

    if (policy->expect_iss != NULL) {
        char iss[256];
        if (!axl_json_get_string(claims, "iss", iss, sizeof(iss))
            || axl_strcmp(iss, policy->expect_iss) != 0) {
            return false;
        }
    }

    if (policy->expect_aud != NULL && !jwt_check_aud(claims, policy->expect_aud)) {
        return false;
    }
    return true;
}

int
axl_jwt_verify(const char *token, size_t token_len, const AxlJoseKey *key,
               const AxlJoseAlg *allowed, size_t n_allowed,
               const AxlJwtPolicy *policy,
               uint8_t **payload_out, size_t *payload_len_out,
               AxlJsonReader *claims_out)
{
    if (policy == NULL || payload_out == NULL || payload_len_out == NULL) {
        return AXL_ERR;
    }

    uint8_t *payload = NULL;
    size_t   payload_len = 0;
    if (axl_jws_verify(token, token_len, key, allowed, n_allowed,
                       &payload, &payload_len) != AXL_OK) {
        return AXL_ERR;
    }

    AxlJsonReader claims = { 0 };
    if (!jose_parse_object((const char *)payload, payload_len, &claims)
        || !jwt_check_claims(&claims, policy)) {
        axl_json_free(&claims);
        axl_free(payload);
        return AXL_ERR;
    }

    *payload_out = payload;
    *payload_len_out = payload_len;
    if (claims_out != NULL) {
        *claims_out = claims;   /* aliases payload — caller keeps it alive */
    } else {
        axl_json_free(&claims);
    }
    return AXL_OK;
}

// ===================================================================
// JWK / JWK Set.
// ===================================================================

struct AxlJwks {
    struct {
        char     *kid;   /* may be NULL for a key with no `kid` */
        AxlPkKey *key;
    }     *entries;
    size_t count;
};

/* Decode a base64url JWK member (e.g. `x`, `n`) into @p buf. Returns the
   decoded length, or 0 on a missing member / decode error / overflow. */
static size_t
jwk_decode_member(const AxlJsonReader *r, const char *key,
                  uint8_t *buf, size_t cap)
{
    char enc[1024];
    if (!axl_json_get_string(r, key, enc, sizeof(enc))) {
        return 0;
    }
    void  *raw = NULL;
    size_t raw_len = 0;
    if (axl_base64url_decode(enc, axl_strlen(enc), &raw, &raw_len) != AXL_OK) {
        return 0;
    }
    size_t out = 0;
    if (raw_len > 0 && raw_len <= cap) {
        axl_memcpy(buf, raw, raw_len);
        out = raw_len;
    }
    axl_free(raw);
    return out;
}

/* Build a public key from a JWK already parsed into @p r. Optionally
   returns the `kid` and implied algorithm. Returns NULL on an
   unsupported/malformed key. */
static AxlPkKey *
jwk_from_reader(const AxlJsonReader *r, char **kid_out, AxlJoseAlg *alg_out)
{
    char kty[8];
    if (!axl_json_get_string(r, "kty", kty, sizeof(kty))) {
        return NULL;
    }

    AxlPkKey  *key = NULL;
    AxlJoseAlg inferred;

    if (axl_strcmp(kty, "EC") == 0) {
        char crv[16];
        if (!axl_json_get_string(r, "crv", crv, sizeof(crv))) {
            return NULL;
        }
        AxlPkAlg curve_alg;
        if (axl_strcmp(crv, "P-256") == 0) {
            curve_alg = AXL_PK_ECDSA_P256;
            inferred  = AXL_JOSE_ES256;
        } else if (axl_strcmp(crv, "P-384") == 0) {
            curve_alg = AXL_PK_ECDSA_P384;
            inferred  = AXL_JOSE_ES384;
        } else {
            return NULL;   /* only P-256 / P-384 are exposed */
        }
        uint8_t x[JOSE_EC_COORD_MAX], y[JOSE_EC_COORD_MAX];
        size_t  xl = jwk_decode_member(r, "x", x, sizeof(x));
        size_t  yl = jwk_decode_member(r, "y", y, sizeof(y));
        if (xl == 0 || yl == 0) {
            return NULL;
        }
        key = axl_pk_key_from_ec_xy(curve_alg, x, xl, y, yl);
    } else if (axl_strcmp(kty, "RSA") == 0) {
        uint8_t n[JOSE_RSA_N_MAX], e[64];
        size_t  nl = jwk_decode_member(r, "n", n, sizeof(n));
        size_t  el = jwk_decode_member(r, "e", e, sizeof(e));
        if (nl == 0 || el == 0) {
            return NULL;
        }
        key = axl_pk_key_from_rsa_ne(n, nl, e, el);
        inferred = AXL_JOSE_RS256;
    } else {
        return NULL;   /* oct and other key types are out of scope */
    }

    if (key == NULL) {
        return NULL;
    }

    if (alg_out != NULL) {
        char alg[16];
        AxlJoseAlg from_jwk;
        *alg_out = (axl_json_get_string(r, "alg", alg, sizeof(alg))
                    && alg_from_name(alg, &from_jwk))
                       ? from_jwk : inferred;
    }
    if (kid_out != NULL) {
        char kid[256];
        *kid_out = axl_json_get_string(r, "kid", kid, sizeof(kid))
                       ? axl_strdup(kid) : NULL;
    }
    return key;
}

AxlPkKey *
axl_jwk_parse(const char *json, size_t len, char **kid_out, AxlJoseAlg *alg_out)
{
    if (json == NULL || len == 0) {
        return NULL;
    }
    AxlJsonReader r = { 0 };
    if (!jose_parse_object(json, len, &r)) {
        return NULL;
    }
    AxlPkKey *key = jwk_from_reader(&r, kid_out, alg_out);
    axl_json_free(&r);
    return key;
}

AxlJwks *
axl_jwks_parse(const char *json, size_t len)
{
    if (json == NULL || len == 0) {
        return NULL;
    }
    AxlJsonReader r = { 0 };
    if (!jose_parse_object(json, len, &r)) {
        return NULL;
    }

    AxlJwks *set = axl_calloc(1, sizeof(*set));
    if (set == NULL) {
        axl_json_free(&r);
        return NULL;
    }

    AxlJsonArrayIter it;
    if (axl_json_array_begin(&r, "keys", &it)) {
        AxlJsonReader el;
        while (axl_json_array_next(&it, &el)) {
            char     *kid = NULL;
            AxlPkKey *key = jwk_from_reader(&el, &kid, NULL);
            if (key == NULL) {
                continue;   /* skip an unsupported key, keep the rest */
            }
            void *grown = axl_realloc(set->entries,
                                      (set->count + 1) * sizeof(*set->entries));
            if (grown == NULL) {
                axl_free(kid);
                axl_pk_key_free(key);
                axl_jwks_free(set);
                axl_json_free(&r);
                return NULL;
            }
            set->entries = grown;
            set->entries[set->count].kid = kid;
            set->entries[set->count].key = key;
            set->count++;
        }
    }

    axl_json_free(&r);
    return set;
}

const AxlPkKey *
axl_jwks_find(const AxlJwks *set, const char *kid)
{
    if (set == NULL || kid == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < set->count; i++) {
        if (set->entries[i].kid != NULL
            && axl_strcmp(set->entries[i].kid, kid) == 0) {
            return set->entries[i].key;
        }
    }
    return NULL;
}

void
axl_jwks_free(AxlJwks *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        axl_free(set->entries[i].kid);
        axl_pk_key_free(set->entries[i].key);
    }
    axl_free(set->entries);
    axl_free(set);
}

char *
axl_jwk_export_public(const AxlPkKey *key, const char *kid)
{
    if (key == NULL) {
        return NULL;
    }
    AxlPkAlg alg = axl_pk_key_alg(key);

    AxlString    *out = axl_string_new(NULL);
    AxlJsonWriter w;
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);

    if (alg == AXL_PK_ECDSA_P256 || alg == AXL_PK_ECDSA_P384) {
        uint8_t x[JOSE_EC_COORD_MAX], y[JOSE_EC_COORD_MAX];
        size_t  xl = sizeof(x), yl = sizeof(y);
        if (axl_pk_key_get_ec_xy(key, x, &xl, y, &yl) != AXL_OK) {
            axl_string_free(out);
            return NULL;
        }
        char *xb = axl_base64url_encode(x, xl);
        char *yb = axl_base64url_encode(y, yl);
        if (xb != NULL && yb != NULL) {
            axl_json_kv_str(&w, "kty", "EC");
            axl_json_kv_str(&w, "crv",
                            (alg == AXL_PK_ECDSA_P384) ? "P-384" : "P-256");
            axl_json_kv_str(&w, "x", xb);
            axl_json_kv_str(&w, "y", yb);
        }
        bool ok = (xb != NULL && yb != NULL);
        axl_free(xb);
        axl_free(yb);
        if (!ok) {
            axl_string_free(out);
            return NULL;
        }
    } else if (alg == AXL_PK_RSA) {
        uint8_t n[JOSE_RSA_N_MAX], e[64];
        size_t  nl = sizeof(n), el = sizeof(e);
        if (axl_pk_key_get_rsa_ne(key, n, &nl, e, &el) != AXL_OK) {
            axl_string_free(out);
            return NULL;
        }
        char *nb = axl_base64url_encode(n, nl);
        char *eb = axl_base64url_encode(e, el);
        if (nb != NULL && eb != NULL) {
            axl_json_kv_str(&w, "kty", "RSA");
            axl_json_kv_str(&w, "n", nb);
            axl_json_kv_str(&w, "e", eb);
        }
        bool ok = (nb != NULL && eb != NULL);
        axl_free(nb);
        axl_free(eb);
        if (!ok) {
            axl_string_free(out);
            return NULL;
        }
    } else {
        axl_string_free(out);
        return NULL;
    }

    if (kid != NULL) {
        axl_json_kv_str(&w, "kid", kid);
    }
    axl_json_obj_end(&w);

    char *json = NULL;
    if (!axl_json_writer_error(&w)) {
        json = axl_strdup(axl_string_str(out));
    }
    axl_string_free(out);
    return json;
}

