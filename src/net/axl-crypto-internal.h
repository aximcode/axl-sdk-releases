/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-crypto-internal.h
    Internal shared crypto helpers (AXL_HAVE_TLS only).
**/

#ifndef AXL_CRYPTO_INTERNAL_H
#define AXL_CRYPTO_INTERNAL_H

#ifdef AXL_HAVE_TLS

#include <stddef.h>
#include <stdint.h>
#include <mbedtls/ctr_drbg.h>
#include <axl/axl-crypto.h>  /* AxlPkKey, AxlPkAlg */

/**
 * Lazy process-global DRBG shared by the mbedTLS-backed crypto TUs
 * (key generation, ECDSA signing, ECDH). Seeded once from the platform
 * entropy source and freed via axl_atexit so the leak checker stays
 * clean. Independent of axl_tls_init().
 *
 * @return the seeded DRBG, or NULL if seeding failed.
 */
mbedtls_ctr_drbg_context *
axl_crypto_rng(void);

// ===================================================================
// Raw public-key component import/export (JWK support).
// ===================================================================
//
// The public AxlPkKey API serializes whole keys as DER. JWK (RFC 7517)
// instead carries the bare key components — EC affine (x, y), RSA
// (n, e) — so axl-jose needs to construct and deconstruct a key from
// those raw integers. These internal helpers bridge that gap so the
// mbedTLS coordinate/bignum handling stays in one place (here, beside
// the rest of the key-handle code), not duplicated in the JOSE module.

/**
 * Build a public-key handle from raw EC affine coordinates @p x and @p y
 * (big-endian, each at most the curve's field size; shorter values are
 * left-padded). @p alg selects the curve (AXL_PK_ECDSA_P256 -> P-256).
 *
 * @return a public AxlPkKey (caller frees with axl_pk_key_free), or NULL
 *     on a point not on the curve, an unsupported @p alg, or OOM.
 */
AxlPkKey *
axl_pk_key_from_ec_xy(AxlPkAlg alg,
                      const uint8_t *x, size_t x_len,
                      const uint8_t *y, size_t y_len);

/**
 * Build a public-key handle from a raw RSA modulus @p n and public
 * exponent @p e (both big-endian).
 *
 * @return a public AxlPkKey (caller frees with axl_pk_key_free), or NULL
 *     on malformed components or OOM.
 */
AxlPkKey *
axl_pk_key_from_rsa_ne(const uint8_t *n, size_t n_len,
                       const uint8_t *e, size_t e_len);

/**
 * Export an EC public key's affine coordinates, each left-padded to the
 * curve's field size (32 bytes for P-256). @p x and @p y must each have
 * capacity for the field size; on entry @p *x_len / @p *y_len are the
 * buffer capacities and on success are set to the field size.
 *
 * @return AXL_OK on success; AXL_ERR on a non-EC key, a too-small buffer,
 *     or bad args.
 */
int
axl_pk_key_get_ec_xy(const AxlPkKey *key,
                     uint8_t *x, size_t *x_len,
                     uint8_t *y, size_t *y_len);

/**
 * Export an RSA public key's modulus @p n and exponent @p e as minimal
 * big-endian integers (no leading zero bytes). Uses the in/out length
 * protocol: on entry @p *n_len / @p *e_len are buffer capacities, on
 * success set to the bytes written.
 *
 * @return AXL_OK on success; AXL_ERR on a non-RSA key, a too-small
 *     buffer, or bad args.
 */
int
axl_pk_key_get_rsa_ne(const AxlPkKey *key,
                      uint8_t *n, size_t *n_len,
                      uint8_t *e, size_t *e_len);

/**
 * Sign / verify with RSASSA-PSS over SHA-256 (JWS PS256). The public
 * sign/verify do PKCS#1 v1.5 (RS256); PSS is a per-signature scheme, not a
 * key property, so axl-jose selects it through these. @p key must be an
 * RSA key (a private key to sign). Sign uses the size-query / output-buffer
 * protocol on @p sig / @p sig_len.
 *
 * @return AXL_OK on success / a valid signature; AXL_ERR otherwise.
 */
int
axl_pk_rsa_pss_sha256_sign(const AxlPkKey *key,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t *sig, size_t *sig_len);

int
axl_pk_rsa_pss_sha256_verify(const AxlPkKey *key,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *sig, size_t sig_len);

#endif /* AXL_HAVE_TLS */

#endif /* AXL_CRYPTO_INTERNAL_H */
