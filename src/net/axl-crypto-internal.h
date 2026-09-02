/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-crypto-internal.h
    Internal shared crypto helpers over the vendored mbedTLS.
**/

#ifndef AXL_CRYPTO_INTERNAL_H
#define AXL_CRYPTO_INTERNAL_H


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <axl/axl-crypto.h>  /* AxlPkKey, AxlPkAlg */
#include <axl/axl-mem.h>     /* axl_malloc, axl_free */
#include <axl/axl-str.h>     /* axl_memset */

/* ECDSA curve order sizes. A raw (P1363) signature is r||s, each padded
   to the order size: 64 bytes for P-256, 96 for P-384. */
#define AXL_P256_ORDER_BYTES  32u
#define AXL_P384_ORDER_BYTES  48u

/* The fail-closed sentinel key_alloc() leaves in `alg` before a real
   algorithm is known. Deliberately NOT a value of AxlPkAlg: the enum's
   zero value (AXL_PK_ED25519) was the original choice, on the theory
   that "reserved and unimplemented" made it a safe stand-in for
   "unclassified". E2b breaks that theory -- once an Ed25519 provider is
   linked, AXL_PK_ED25519 IS a valid, resolvable algorithm, and
   _axl_pk_provider_for() would happily resolve a half-built mbedTLS
   handle to the ref10 provider. Casting out of range instead makes the
   failure structural rather than a fact about which providers happen
   to exist: _axl_pk_provider_for()'s switch does not and cannot name
   this value, so it falls through to the function's trailing
   `return NULL` unconditionally, for as long as AxlPkAlg has no
   negative enumerator. */
#define AXL_PK_ALG_UNCLASSIFIED ((AxlPkAlg)-1)

/* AxlPkKey used to *be* an mbedtls_pk_context. Ed25519 cannot be one --
   mbedTLS 3.6.3 has no twisted-Edwards curve at all -- so the key
   became a tagged union: `alg` is the tag, and only the arm it names
   is live. Defined here, not in axl-pk-provider.h, because that header
   is the algorithm-neutral seam every provider (including a future
   ref10-backed one) includes, and it must stay mbedTLS-free; this
   header already carries mbedTLS types shared across the pk-verify
   dispatcher and the mbedTLS provider translation unit. A union with
   one arm reads oddly today and is deliberate: a later Ed25519
   provider adds its arm beside this one. */
struct AxlPkKey {
    AxlPkAlg alg;
    bool     has_private;
    union {
        mbedtls_pk_context mbedtls;
    } u;
};

/**
 * Allocate a key handle with no algorithm chosen and no union arm
 * initialised. Shared by every construction path in axl-pk-verify.c
 * and axl-pk-mbedtls.c so the one security-relevant default -- the
 * fail-closed sentinel below -- exists in exactly one place instead
 * of two copies that can drift.
 *
 * `alg` is left at #AXL_PK_ALG_UNCLASSIFIED, an out-of-range value
 * (see its own comment above) specifically so that a construction
 * which fails before a real algorithm is known fails CLOSED: the
 * half-built handle never reads as a valid, usable algorithm, no
 * matter which providers a given image happens to link. A caller that
 * already knows its target algorithm (axl_pk_key_new) overwrites
 * `alg` immediately; a caller importing DER (der_import_any() in
 * axl-pk-verify.c) leaves it as the sentinel until a provider's
 * der_private_import / der_public_import classifies the bytes and
 * overwrites it on success -- see axl-pk-provider.h's lifecycle note.
 *
 * No union arm is initialised here: this helper does not know which
 * provider the caller will use, so key_init happens through whichever
 * provider is chosen, immediately after this returns.
 *
 * @return a zeroed handle, or NULL on OOM.
 */
static inline AxlPkKey *
key_alloc(void)
{
    AxlPkKey *k = axl_malloc(sizeof(*k));
    if (k == NULL) {
        return NULL;
    }
    axl_memset(&k->u, 0, sizeof(k->u));
    /* Deliberately out of AxlPkAlg's range -- see AXL_PK_ALG_UNCLASSIFIED
       above for why. clang-analyzer's EnumCastOutOfRange assumes every
       value ever stored in an enum-typed field is one of its named
       enumerators; that assumption is exactly the failure mode this
       sentinel exists to avoid, so it is a false positive here, not a
       bug to work around by picking an in-range value. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    k->alg         = AXL_PK_ALG_UNCLASSIFIED;
    k->has_private = false;
    return k;
}

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

/* The ECDSA curve order size in bytes for a raw r||s signature. Zero for
   an algorithm whose signature is not r||s, which is every non-ECDSA one.
   static inline (not static) because both axl-pk-verify.c and
   axl-pk-mbedtls.c see this header. */
static inline size_t
ec_order_bytes(AxlPkAlg alg)
{
    switch (alg) {
    case AXL_PK_ECDSA_P256:   return AXL_P256_ORDER_BYTES;
    case AXL_PK_ECDSA_P384:   return AXL_P384_ORDER_BYTES;
    case AXL_PK_RSA:
    case AXL_PK_ED25519:      return 0;
    }
    return 0;
}

/* The message-digest a key-handle algorithm signs over: the ECDSA hash
   follows the curve (P-256 -> SHA-256, P-384 -> SHA-384); RSA PKCS#1 v1.5
   here is always SHA-256 (RS256). Returns MBEDTLS_MD_NONE for an algorithm
   that does not sign over a prehash at all -- Ed25519 hashes the message
   internally, twice -- so a caller that forgets to check gets a refusal
   from mbedtls rather than a signature over the wrong digest. */
static inline mbedtls_md_type_t
md_for_alg(AxlPkAlg alg)
{
    switch (alg) {
    case AXL_PK_ECDSA_P256:
    case AXL_PK_RSA:          return MBEDTLS_MD_SHA256;
    case AXL_PK_ECDSA_P384:   return MBEDTLS_MD_SHA384;
    case AXL_PK_ED25519:      return MBEDTLS_MD_NONE;
    }
    return MBEDTLS_MD_NONE;
}

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


#endif /* AXL_CRYPTO_INTERNAL_H */
