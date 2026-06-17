/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-jose.h:
 *
 * JOSE — JSON Object Signing and Encryption: JWS (RFC 7515), JWT
 * (RFC 7519), and JWK (RFC 7517). A generic standards module for
 * signed tokens — API-token auth, OIDC/identity, signed configuration —
 * built on AxlCrypto. It carries no application policy (a consumer's own
 * claims and their meaning stay with the consumer).
 *
 * v1 scope: JWS Compact (sign + verify), JWT registered-claim validation
 * with a caller-supplied clock, and JWK/JWK-Set parse/export/kid
 * resolution. Algorithms: ES256, ES384, RS256, PS256, HS256. JWE
 * (encryption) and the non-compact JWS JSON serialization are out of
 * scope for v1.
 *
 * SECURITY MODEL — allow-list, never header-driven. Verification never
 * trusts the token's own `alg` to pick the verification path:
 *   - the caller passes a MANDATORY @c allowed list; a token whose `alg`
 *     is not on it is rejected;
 *   - `none` is not representable (no enum value), so it can never be
 *     selected;
 *   - the key is bound to the algorithm family — an HS256 token verifies
 *     only against an HMAC key, an RS256/PS256/ES* token only against the
 *     matching public key — so the classic RS256<->HS256 confusion is
 *     structurally impossible.
 *
 * Optional — like axl-tls.h / axl-crypto.h, requires AXL_TLS=1 at build
 * time. Without it axl_jose_available() returns false and every call
 * fails closed.
 *
 * @code
 * // SoftBMC-style: verify an ES256 license token against a baked-in key.
 * AxlJoseKey k = { .pk = license_pubkey };
 * const AxlJoseAlg allow[] = { AXL_JOSE_ES256 };
 * uint8_t *payload; size_t plen;
 * if (axl_jws_verify(token, token_len, &k, allow, 1, &payload, &plen) == AXL_OK) {
 *     // payload is authentic; parse the consumer's own claims from it
 *     axl_free(payload);
 * }
 * @endcode
 */

#ifndef AXL_JOSE_H
#define AXL_JOSE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-crypto.h>   /* AxlPkKey */
#include <axl/axl-json.h>     /* AxlJsonReader (parsed JWT claims) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether the JOSE module is available (built with AXL_TLS=1).
 *
 * When false, every axl_jws_* / axl_jwt_* / axl_jwk_* call fails closed
 * (AXL_ERR / NULL).
 *
 * @return true if JOSE operations can run.
 */
bool
axl_jose_available(void);

/**
 * @brief JOSE signature algorithm. There is intentionally no `none`
 * value — it cannot be represented, so it can never be selected.
 *
 * Numeric values are part of the contract (a caller may persist an
 * allow-list); new algorithms only extend the range.
 */
typedef enum {
    AXL_JOSE_ES256 = 1,  /**< ECDSA P-256 + SHA-256 (raw r||s signature). */
    AXL_JOSE_ES384 = 2,  /**< ECDSA P-384 + SHA-384. */
    AXL_JOSE_RS256 = 3,  /**< RSASSA-PKCS1-v1_5 + SHA-256. */
    AXL_JOSE_PS256 = 4,  /**< RSASSA-PSS + SHA-256. */
    AXL_JOSE_HS256 = 5   /**< HMAC-SHA-256 (symmetric). */
} AxlJoseAlg;

/**
 * @brief A signing/verification key, bound to an algorithm family.
 *
 * Fill the field for the algorithm(s) in play; the selected `alg`
 * (constrained by the caller's allow-list) determines which field is
 * used and the other is ignored:
 *   - ES256/ES384/RS256/PS256 use @p pk (a public key for verify; a
 *     private key for sign);
 *   - HS256 uses @p hmac_key / @p hmac_key_len.
 *
 * The HMAC secret and the public key are separate fields, so a token's
 * `alg` can never cause a public key to be used as an HMAC secret (the
 * RS256<->HS256 confusion attack). A by-value descriptor — it borrows
 * its members; the caller owns their lifetime. Zero-init it (`= {0}`)
 * and set only the field for the algorithm(s) in play.
 *
 * Verify against ONE algorithm family per call: an @c allowed list that
 * mixes a symmetric algorithm (HS256) with an asymmetric one (ES*, RS*,
 * PS*) is rejected. Otherwise a caller who populated both fields would be
 * letting the token's own `alg` choose the family — header-driven key
 * selection, the very thing the allow-list exists to prevent.
 */
typedef struct {
    const AxlPkKey *pk;            ///< EC/RSA key (ES*, RS*, PS*); NULL for HS256
    const uint8_t  *hmac_key;      ///< HMAC secret (HS256); NULL otherwise
    size_t          hmac_key_len;  ///< HMAC secret length in bytes
} AxlJoseKey;

// ===================================================================
// JWS Compact (RFC 7515)
// ===================================================================

/**
 * @brief Sign a payload into a JWS Compact token.
 *
 * Produces `BASE64URL(header).BASE64URL(payload).BASE64URL(signature)`
 * with a minimal protected header (`{"alg":"<alg>","typ":"JWT"}`).
 * @p key must hold the material for @p alg (a private @p pk for ES*, RS*,
 * PS* algorithms; @p hmac_key for HS256).
 *
 * @return AXL_OK on success (a NUL-terminated token in @p *token_out,
 *     caller frees with axl_free); AXL_ERR on a key/alg mismatch, a
 *     @p pk that cannot sign (e.g. a public-only key), bad args, or TLS
 *     not compiled in.
 */
int
axl_jws_sign(
    const AxlJoseKey *key,          ///< signing key for @p alg
    AxlJoseAlg        alg,          ///< algorithm to sign with
    const uint8_t    *payload,      ///< payload bytes
    size_t            payload_len,  ///< payload length
    char            **token_out     ///< [out] compact token (caller frees)
);

/**
 * @brief Verify a JWS Compact token and return its payload.
 *
 * Verifies a three-segment compact token. The token's `alg` MUST appear
 * in @p allowed; @p allowed == NULL or @p n_allowed == 0 is an error and
 * the token is rejected with no signature check. A token whose `alg` is
 * not on the list is rejected, also without a signature check. An
 * unsigned token (the JWS Compact form with an empty signature segment,
 * used only by `alg:none`) is rejected. The key is bound to the
 * algorithm family per @ref AxlJoseKey, and an @p allowed list mixing
 * symmetric and asymmetric algorithms is rejected.
 *
 * Any non-AXL_OK result means "not verified, untrusted" — fail closed.
 * @p *payload_out / @p *payload_len_out are set only on AXL_OK (the
 * decoded payload, caller frees with axl_free); on AXL_ERR they are
 * untouched and there is nothing to free.
 *
 * @return AXL_OK if the signature is valid and `alg` is allowed; AXL_ERR
 *     otherwise (empty/NULL allow-list, alg not allowed, mixed-family
 *     allow-list, bad signature, malformed token — not three segments —
 *     wrong key, or TLS not compiled in).
 */
int
axl_jws_verify(
    const char       *token,            ///< compact token
    size_t            token_len,        ///< token length in bytes
    const AxlJoseKey *key,              ///< verification key
    const AxlJoseAlg *allowed,          ///< allow-list (MANDATORY, non-empty)
    size_t            n_allowed,        ///< number of allowed algorithms
    uint8_t         **payload_out,      ///< [out] decoded payload (caller frees)
    size_t           *payload_len_out   ///< [out] payload length
);

// ===================================================================
// JWT (RFC 7519) — registered-claim validation atop JWS
// ===================================================================

/**
 * @brief JWT validation policy.
 *
 * The clock is caller-supplied: the SDK never reads the RTC for policy,
 * so a consumer feeds its own trusted time (e.g. NTP + a monotonic
 * high-water mark). All checks a field enables are skipped when the
 * field is NULL/false.
 */
typedef struct {
    const char *expect_iss;   ///< required `iss`, or NULL to skip
    const char *expect_aud;   ///< required `aud` (membership), or NULL to skip
    int64_t     now;          ///< caller's trusted clock (epoch seconds)
    int64_t     leeway_s;     ///< clock-skew tolerance for exp/nbf (seconds)
    bool        require_exp;  ///< fail if `exp` is absent
    bool        require_nbf;  ///< fail if `nbf` is absent
} AxlJwtPolicy;

/**
 * @brief Verify a JWT: JWS signature, then registered-claim validation.
 *
 * Verifies the signature exactly as axl_jws_verify() (same allow-list
 * rules and fail-closed contract), then validates the registered claims
 * against @p policy: `exp` (with @p leeway_s), `nbf` (with @p leeway_s),
 * `iss` (== @p expect_iss), and `aud` (@p expect_aud present — a single
 * audience matched against the claim's string value or, if `aud` is an
 * array, its members; a missing `aud` with a non-NULL @p expect_aud
 * fails). Times are compared against @p policy->now, never the system
 * clock.
 *
 * The verified claims JSON is returned in @p *payload_out (caller frees
 * with axl_free). If @p claims_out is non-NULL it is filled with a parsed
 * view of those bytes for the caller's own reads; **@p claims_out aliases
 * @p *payload_out — keep the payload alive while reading claims, then
 * release the reader with axl_json_free() and the buffer with axl_free().**
 * Both out-params are set only on AXL_OK; on AXL_ERR neither is touched
 * (zero-init @p claims_out so the error path never frees a stale reader).
 *
 * @return AXL_OK if the signature is valid and all enabled claim checks
 *     pass; AXL_ERR otherwise (any signature or claim failure, or TLS
 *     not compiled in).
 */
int
axl_jwt_verify(
    const char         *token,            ///< compact JWT
    size_t              token_len,        ///< token length in bytes
    const AxlJoseKey   *key,              ///< verification key
    const AxlJoseAlg   *allowed,          ///< allow-list (MANDATORY, non-empty)
    size_t              n_allowed,        ///< number of allowed algorithms
    const AxlJwtPolicy *policy,           ///< claim-validation policy
    uint8_t           **payload_out,      ///< [out] claims JSON bytes (caller frees)
    size_t             *payload_len_out,  ///< [out] claims length
    AxlJsonReader      *claims_out        ///< [out] parsed claims view, or NULL
);

// ===================================================================
// JWK / JWK Set (RFC 7517)
// ===================================================================

/**
 * @brief Parse a single JWK into a public key.
 *
 * Handles asymmetric keys: `kty` `EC` (P-256 / P-384) and `RSA`. On
 * success returns a public AxlPkKey (caller frees with axl_pk_key_free)
 * and, if the pointers are non-NULL, the key's `kid` (caller frees with
 * axl_free) and the implied signature algorithm in @p alg_out. The
 * algorithm comes from the JWK's `alg` member when present; otherwise it
 * is inferred from the key — EC P-256 -> ES256, EC P-384 -> ES384, and
 * RSA -> RS256 (never PS256: RSA padding is not derivable from a bare
 * key, so a caller wanting PS256 must allow it explicitly). Symmetric
 * (`oct`) JWKs are not handled — an HS256 key is raw bytes the caller
 * supplies directly.
 *
 * @return the parsed public key, or NULL on a malformed/unsupported JWK
 *     or TLS not compiled in.
 */
AxlPkKey *
axl_jwk_parse(
    const char *json,     ///< JWK JSON
    size_t      len,      ///< length in bytes
    char      **kid_out,  ///< [out] key id, or NULL to skip (caller frees)
    AxlJoseAlg *alg_out   ///< [out] implied algorithm, or NULL to skip
);

/**
 * @brief A parsed JWK Set (collection of keys keyed by `kid`).
 */
typedef struct AxlJwks AxlJwks;

/**
 * @brief Parse a JWK Set (`{"keys":[...]}`).
 *
 * @return a key set (caller frees with axl_jwks_free), or NULL on a
 *     malformed document or TLS not compiled in.
 */
AxlJwks *
axl_jwks_parse(
    const char *json,  ///< JWK Set JSON
    size_t      len    ///< length in bytes
);

/**
 * @brief Find a key in a set by `kid`.
 *
 * @return the matching public key (owned by @p set, valid until
 *     axl_jwks_free), or NULL if no key has that `kid`.
 */
const AxlPkKey *
axl_jwks_find(
    const AxlJwks *set,  ///< key set
    const char    *kid   ///< key id to look up
);

/**
 * @brief Free a JWK Set and all its keys. NULL-safe.
 */
void
axl_jwks_free(
    AxlJwks *set  ///< key set to free
);

/**
 * @brief Export a public key as a JWK (JSON).
 *
 * Emits the public JWK for @p key (EC: `kty`/`crv`/`x`/`y`; RSA:
 * `kty`/`n`/`e`), including `kid` when @p kid is non-NULL. Only public
 * fields are emitted — never the private key material (`d`), even when
 * @p key holds a private key.
 *
 * @return a NUL-terminated JSON string (caller frees with axl_free), or
 *     NULL on failure / TLS not compiled in.
 */
char *
axl_jwk_export_public(
    const AxlPkKey *key,  ///< public (or private) key to export the public half of
    const char     *kid   ///< key id to embed, or NULL to omit
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlJwks, axl_jwks_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_JOSE_H */
