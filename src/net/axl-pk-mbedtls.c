/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-mbedtls.c
    The mbedTLS AxlPkProvider: ECDSA P-256, ECDSA P-384, and RSA.

    One provider serves all three algorithms -- they differ only by
    curve/key type, which the key itself carries (see axl-pk-verify.c's
    `_axl_pk_provider_for`). This file holds the mbedTLS-specific
    bodies that axl-pk-verify.c dispatches into; argument validation
    that does not depend on mbedTLS stays in the dispatcher.
**/

#include <axl/axl-crypto.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-crypto-internal.h"
#include "axl-pk-provider.h"

#include <mbedtls/asn1.h>
#include <mbedtls/asn1write.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

/* P-256/P-384 field width covers every EC coordinate AXL emits. */
#define AXL_EC_FIELD_MAX  48u
/* Upper bound for a serialized key: a 3072-bit RSA PKCS#8 private key is
   ~1.9 KB; P-256 keys are far smaller. 4 KB covers every key AXL makes. */
#define AXL_PK_DER_MAX    4096u

// -------------------------------------------------------------------
// Shared helpers.
// -------------------------------------------------------------------

/* Every mbedTLS-arm site that already knows key->alg names this arm
   goes through this rather than reaching into the union, so the tag
   check lives in one place. Returns NULL when `alg` does not name the
   mbedTLS arm, which every caller treats as a refusal. The const_cast
   is confined here: mbedTLS's API takes a non-const context even for
   operations that do not mutate the key. */
static mbedtls_pk_context *
mbedtls_ctx(const AxlPkKey *key)
{
    if (key == NULL) {
        return NULL;
    }
    switch (key->alg) {
    case AXL_PK_ECDSA_P256:
    case AXL_PK_ECDSA_P384:
    case AXL_PK_RSA:
        return (mbedtls_pk_context *)&key->u.mbedtls;
    case AXL_PK_ED25519:
        return NULL;
    }
    return NULL;
}

/* Classify a parsed/generated pk context into an AxlPkAlg, rejecting
   anything AXL does not expose (e.g. EC on a non-P-256 curve). Returns
   false if unsupported. */
static bool
classify_pk(const mbedtls_pk_context *pk, AxlPkAlg *out)
{
    if (mbedtls_pk_can_do(pk, MBEDTLS_PK_RSA)
        && mbedtls_pk_get_type(pk) == MBEDTLS_PK_RSA) {
        *out = AXL_PK_RSA;
        return true;
    }
    if (mbedtls_pk_can_do(pk, MBEDTLS_PK_ECKEY)) {
        mbedtls_ecp_group_id gid =
            mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(*pk));
        if (gid == MBEDTLS_ECP_DP_SECP256R1) {
            *out = AXL_PK_ECDSA_P256;
            return true;
        }
        if (gid == MBEDTLS_ECP_DP_SECP384R1) {
            *out = AXL_PK_ECDSA_P384;
            return true;
        }
    }
    return false;
}

/* Hash @p msg with @p md into @p out (caller sizes for the largest digest
   in play — 48 bytes for SHA-384). Sets @p *out_len to the digest size. */
static bool
compute_hash(mbedtls_md_type_t md, const uint8_t *msg, size_t msg_len,
             uint8_t *out, size_t *out_len)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md);
    if (info == NULL) {
        return false;
    }
    *out_len = mbedtls_md_get_size(info);
    return mbedtls_md(info, msg, msg_len, out) == 0;
}

/* Apply the public size-query / output-buffer protocol around a mbedTLS
   DER writer (write_key_der / write_pubkey_der), which emit at the END of
   the buffer and return the length. */
static int
emit_der(int (*writer)(const mbedtls_pk_context *, unsigned char *, size_t),
         const mbedtls_pk_context *pk, uint8_t *out, size_t *len)
{
    if (len == NULL) {
        return AXL_ERR;
    }
    uint8_t tmp[AXL_PK_DER_MAX];
    int     n = writer(pk, tmp, sizeof(tmp));
    if (n < 0) {
        return AXL_ERR;
    }
    size_t need = (size_t)n;
    if (out == NULL) {
        *len = need;
        return AXL_OK;
    }
    if (*len < need) {
        *len = need;
        return AXL_ERR;
    }
    axl_memcpy(out, tmp + sizeof(tmp) - need, need);
    *len = need;
    return AXL_OK;
}

/* Convert a DER ECDSA signature (SEQUENCE{INTEGER r, INTEGER s}) to the
   fixed-width P1363 form r||s, each left-padded to @p order bytes. Public
   ASN.1/MPI APIs only — no mbedTLS struct internals. */
static int
ecdsa_der_to_raw(const uint8_t *der, size_t der_len,
                 uint8_t *out, size_t order)
{
    unsigned char       *p   = (unsigned char *)der;
    const unsigned char *end = der + der_len;
    size_t               seq_len;
    mbedtls_mpi          r, s;
    int                  rc = -1;

    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    if (mbedtls_asn1_get_tag(&p, end, &seq_len,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) == 0
        && mbedtls_asn1_get_mpi(&p, end, &r) == 0
        && mbedtls_asn1_get_mpi(&p, end, &s) == 0
        && mbedtls_mpi_write_binary(&r, out, order) == 0
        && mbedtls_mpi_write_binary(&s, out + order, order) == 0) {
        rc = 0;
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return (rc == 0) ? AXL_OK : AXL_ERR;
}

/* Convert a fixed-width P1363 signature r||s (@p order bytes each) to DER,
   written into @p buf (capacity @p cap). On success @p *out points at the
   DER within @p buf and @p *out_len is its length. */
static int
ecdsa_raw_to_der(const uint8_t *raw, size_t order,
                 uint8_t *buf, size_t cap,
                 uint8_t **out, size_t *out_len)
{
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int            rc  = AXL_ERR;
    unsigned char *p   = buf + cap;   /* asn1write fills back-to-front */
    int            len = 0, n;

    if (mbedtls_mpi_read_binary(&r, raw, order) != 0
        || mbedtls_mpi_read_binary(&s, raw + order, order) != 0) {
        goto out;
    }
    /* DER is built right-to-left: value s, then r, then the SEQUENCE
       length and tag wrapping both. */
    n = mbedtls_asn1_write_mpi(&p, buf, &s);
    if (n < 0) {
        goto out;
    }
    len += n;
    n = mbedtls_asn1_write_mpi(&p, buf, &r);
    if (n < 0) {
        goto out;
    }
    len += n;
    n = mbedtls_asn1_write_len(&p, buf, (size_t)len);
    if (n < 0) {
        goto out;
    }
    len += n;
    n = mbedtls_asn1_write_tag(&p, buf,
                               MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (n < 0) {
        goto out;
    }
    len += n;
    *out     = p;
    *out_len = (size_t)len;
    rc       = AXL_OK;

out:
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return rc;
}

/* Map an AxlPkAlg EC curve to its mbedTLS group id and field byte size.
   Returns false for non-EC / unsupported algorithms. */
static bool
ec_group_for_alg(AxlPkAlg alg, mbedtls_ecp_group_id *grp_id, size_t *flen)
{
    switch (alg) {
    case AXL_PK_ECDSA_P256:
        *grp_id = MBEDTLS_ECP_DP_SECP256R1;
        *flen   = 32u;
        return true;
    case AXL_PK_ECDSA_P384:
        *grp_id = MBEDTLS_ECP_DP_SECP384R1;
        *flen   = 48u;
        return true;
    default:
        return false;
    }
}

/* Copy a big-endian integer from @p src (length @p src_len) into @p dst
   with leading zero bytes stripped (the minimal unsigned encoding JWK
   requires for `n` and `e`). A wholly-zero input collapses to one 0x00
   byte. On entry @p *dst_len is the buffer capacity; on success it is the
   bytes written. Returns AXL_ERR on a too-small buffer. */
static int
copy_minimal_be(const uint8_t *src, size_t src_len,
                uint8_t *dst, size_t *dst_len)
{
    size_t lead = 0;
    while (lead + 1 < src_len && src[lead] == 0) {
        lead++;
    }
    size_t need = src_len - lead;
    if (*dst_len < need) {
        *dst_len = need;
        return AXL_ERR;
    }
    axl_memcpy(dst, src + lead, need);
    *dst_len = need;
    return AXL_OK;
}

// -------------------------------------------------------------------
// JWK component import/export -- mbedTLS-only, not vtable members
// (touch the arm directly; see axl-pk-provider.h's lifecycle note).
// -------------------------------------------------------------------

/* Populate an EC public key into @a k's (already key_init'd, empty)
   arm from SEC1 affine coordinates @a x / @a y, each at most @a flen
   bytes and left-padded on write. Does not touch @a k->alg or
   @a k->has_private -- classification (when the caller doesn't already
   know the algorithm) and private-key bookkeeping are the caller's
   job. Shared by axl_pk_key_from_ec_xy(), which owns allocation and
   classifies the result, and mbedtls_raw_public_import(), which is
   handed an already-allocated, already-classified key by the
   dispatcher -- factored out so the point math exists in exactly one
   place rather than reimplemented for the vtable member. */
static bool
ec_xy_populate(AxlPkKey *k, mbedtls_ecp_group_id grp_id, size_t flen,
              const uint8_t *x, size_t x_len,
              const uint8_t *y, size_t y_len)
{
    if (x == NULL || y == NULL || x_len == 0 || x_len > flen
        || y_len == 0 || y_len > flen) {
        return false;
    }
    mbedtls_pk_context *ctx = &k->u.mbedtls;

    /* SEC1 uncompressed point 0x04 || X || Y, each coordinate big-endian
       and left-padded to the field size. */
    uint8_t point[1u + 2u * AXL_EC_FIELD_MAX];
    size_t  plen = 1u + 2u * flen;
    axl_memset(point, 0, plen);
    point[0] = 0x04;
    axl_memcpy(point + 1u + (flen - x_len), x, x_len);
    axl_memcpy(point + 1u + flen + (flen - y_len), y, y_len);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);

    bool ok = mbedtls_pk_setup(ctx,
                  mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0
        && mbedtls_ecp_group_load(&grp, grp_id) == 0
        && mbedtls_ecp_point_read_binary(&grp, &Q, point, plen) == 0
        /* point_read_binary does NOT verify curve membership — a JWK is
           attacker-controlled, so reject an off-curve point explicitly. */
        && mbedtls_ecp_check_pubkey(&grp, &Q) == 0
        && mbedtls_ecp_set_public_key(grp_id, mbedtls_pk_ec(*ctx), &Q) == 0;

    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

AxlPkKey *
axl_pk_key_from_ec_xy(AxlPkAlg alg,
                      const uint8_t *x, size_t x_len,
                      const uint8_t *y, size_t y_len)
{
    mbedtls_ecp_group_id grp_id;
    size_t               flen;
    if (!ec_group_for_alg(alg, &grp_id, &flen)) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    /* Construction: address the arm directly -- the tag is not yet
       meaningful (classify_pk() below determines it), and this file
       IS the mbedTLS provider, so there is no seam to route the init
       through. */
    mbedtls_pk_init(&k->u.mbedtls);

    bool ok = ec_xy_populate(k, grp_id, flen, x, x_len, y, y_len)
        && classify_pk(&k->u.mbedtls, &k->alg);

    if (!ok) {
        /* Free the arm directly, not via axl_pk_key_free(): k->alg is
           still key_alloc()'s AXL_PK_ALG_UNCLASSIFIED sentinel here
           (classify_pk never ran, or ran and failed), so resolving a
           provider from the tag would find none at all. */
        mbedtls_pk_free(&k->u.mbedtls);
        axl_free(k);
        return NULL;
    }
    k->has_private = false;
    return k;
}

AxlPkKey *
axl_pk_key_from_rsa_ne(const uint8_t *n, size_t n_len,
                       const uint8_t *e, size_t e_len)
{
    if (n == NULL || e == NULL || n_len == 0 || e_len == 0) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    /* Construction: address the arm directly (see axl_pk_key_from_ec_xy()
       above). */
    mbedtls_pk_init(&k->u.mbedtls);
    mbedtls_pk_context *ctx = &k->u.mbedtls;

    int rc = -1;
    if (mbedtls_pk_setup(ctx,
            mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*ctx);
        /* A JWK is attacker-controlled. mbedtls_rsa_complete() accepts a
           degenerate modulus (e.g. a few bytes), so pin a 2048-bit floor —
           the RS256 security level — rather than minting a "valid" handle
           for a key that can never carry a real signature. */
        if (mbedtls_rsa_import_raw(rsa, n, n_len, NULL, 0, NULL, 0,
                                   NULL, 0, e, e_len) == 0
            && mbedtls_rsa_complete(rsa) == 0
            && mbedtls_rsa_get_len(rsa) >= 256u
            && classify_pk(ctx, &k->alg)) {
            rc = 0;
        }
    }

    if (rc != 0) {
        /* Free the arm directly -- see axl_pk_key_from_ec_xy() above. */
        mbedtls_pk_free(&k->u.mbedtls);
        axl_free(k);
        return NULL;
    }
    k->has_private = false;
    return k;
}

int
axl_pk_key_get_ec_xy(const AxlPkKey *key,
                     uint8_t *x, size_t *x_len,
                     uint8_t *y, size_t *y_len)
{
    mbedtls_ecp_group_id grp_id;
    size_t               flen;
    if (key == NULL || x == NULL || x_len == NULL || y == NULL
        || y_len == NULL || !ec_group_for_alg(key->alg, &grp_id, &flen)) {
        return AXL_ERR;
    }
    if (*x_len < flen || *y_len < flen) {
        *x_len = flen;
        *y_len = flen;
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);

    int     rc   = AXL_ERR;
    uint8_t point[1u + 2u * AXL_EC_FIELD_MAX];
    size_t  olen = 0;
    if (mbedtls_ecp_export(mbedtls_pk_ec(*pk), &grp, NULL, &Q) == 0
        && mbedtls_ecp_point_write_binary(&grp, &Q,
               MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, point, sizeof(point)) == 0
        && olen == 1u + 2u * flen) {
        axl_memcpy(x, point + 1u, flen);
        axl_memcpy(y, point + 1u + flen, flen);
        *x_len = flen;
        *y_len = flen;
        rc = AXL_OK;
    }

    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

int
axl_pk_key_get_rsa_ne(const AxlPkKey *key,
                      uint8_t *n, size_t *n_len,
                      uint8_t *e, size_t *e_len)
{
    if (key == NULL || n == NULL || n_len == NULL || e == NULL
        || e_len == NULL || key->alg != AXL_PK_RSA) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
    size_t klen = mbedtls_rsa_get_len(rsa);
    if (klen == 0 || klen > AXL_PK_DER_MAX) {
        return AXL_ERR;
    }

    /* Export both components modulus-width, then re-emit minimal. mbedtls
       writes each left-padded to the requested width, so the exponent's
       scratch is sized to the modulus length too. */
    uint8_t nbuf[AXL_PK_DER_MAX];
    uint8_t ebuf[AXL_PK_DER_MAX];
    if (mbedtls_rsa_export_raw(rsa, nbuf, klen, NULL, 0, NULL, 0,
                               NULL, 0, ebuf, klen) != 0) {
        return AXL_ERR;
    }
    if (copy_minimal_be(nbuf, klen, n, n_len) != AXL_OK
        || copy_minimal_be(ebuf, klen, e, e_len) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

// -------------------------------------------------------------------
// RSA-PSS (PS256) sign/verify — JOSE-internal, mbedTLS-only.
// -------------------------------------------------------------------
//
// The public sign/verify do RSA PKCS#1 v1.5 (RS256). PS256 is the same
// RSA key under PSS padding, which is a per-signature scheme, not a key
// property — so axl-jose selects it explicitly through these helpers
// rather than baking a padding mode into the key handle.

int
axl_pk_rsa_pss_sha256_sign(const AxlPkKey *key,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t *sig, size_t *sig_len)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (key == NULL || !key->has_private || key->alg != AXL_PK_RSA
        || sig_len == NULL || (msg == NULL && msg_len != 0) || rng == NULL) {
        return AXL_ERR;
    }

    if (sig == NULL) {
        *sig_len = (size_t)MBEDTLS_PK_SIGNATURE_MAX_SIZE;
        return AXL_OK;
    }

    uint8_t hash[32];
    size_t  hlen = 0;
    if (!compute_hash(MBEDTLS_MD_SHA256, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    uint8_t tmp[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t  olen = 0;
    /* mbedtls_pk_sign_ext with RSASSA-PSS uses MGF1 and a salt length both
       matching the message hash (SHA-256) — the JWS PS256 convention. */
    if (mbedtls_pk_sign_ext(MBEDTLS_PK_RSASSA_PSS, pk, MBEDTLS_MD_SHA256,
                            hash, hlen, tmp, sizeof(tmp), &olen,
                            mbedtls_ctr_drbg_random, rng) != 0) {
        return AXL_ERR;
    }
    if (*sig_len < olen) {
        *sig_len = olen;
        return AXL_ERR;
    }
    axl_memcpy(sig, tmp, olen);
    *sig_len = olen;
    return AXL_OK;
}

int
axl_pk_rsa_pss_sha256_verify(const AxlPkKey *key,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *sig, size_t sig_len)
{
    if (key == NULL || key->alg != AXL_PK_RSA || sig == NULL || sig_len == 0
        || (msg == NULL && msg_len != 0)) {
        return AXL_ERR;
    }

    uint8_t hash[32];
    size_t  hlen = 0;
    if (!compute_hash(MBEDTLS_MD_SHA256, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    mbedtls_pk_rsassa_pss_options opts = {
        .mgf1_hash_id     = MBEDTLS_MD_SHA256,
        /* Accept any salt length — the interoperable choice; JWS signers
           use salt == hash length but verifiers should not depend on it. */
        .expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY,
    };
    return (mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &opts, pk,
                                  MBEDTLS_MD_SHA256, hash, hlen,
                                  sig, sig_len) == 0) ? AXL_OK : AXL_ERR;
}

// -------------------------------------------------------------------
// AxlPkProvider member implementations.
// -------------------------------------------------------------------

static bool
mbedtls_key_init(AxlPkKey *key)
{
    mbedtls_pk_init(&key->u.mbedtls);
    return true;
}

static void
mbedtls_key_free(AxlPkKey *key)
{
    mbedtls_pk_free(&key->u.mbedtls);
}

static bool
mbedtls_keygen(AxlPkKey *key)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL) {
        return false;
    }
    /* key->alg is already the dispatcher's target (axl_pk_key_new sets
       it before calling key_init/keygen) -- but the arm is still
       addressed directly here rather than through mbedtls_ctx(),
       matching the other construction entry points in this file. */
    mbedtls_pk_context *ctx = &key->u.mbedtls;

    int rc = -1;
    if (key->alg == AXL_PK_ECDSA_P256 || key->alg == AXL_PK_ECDSA_P384) {
        mbedtls_ecp_group_id grp =
            (key->alg == AXL_PK_ECDSA_P384) ? MBEDTLS_ECP_DP_SECP384R1
                                            : MBEDTLS_ECP_DP_SECP256R1;
        if (mbedtls_pk_setup(ctx,
                mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0) {
            rc = mbedtls_ecp_gen_key(grp, mbedtls_pk_ec(*ctx),
                                     mbedtls_ctr_drbg_random, rng);
        }
    } else if (key->alg == AXL_PK_RSA) {
        if (mbedtls_pk_setup(ctx,
                mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
            rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*ctx),
                                     mbedtls_ctr_drbg_random, rng,
                                     3072, 65537);
        }
    }
    return rc == 0;
}

static int
mbedtls_sign(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
             AxlPkSigFormat fmt, AxlPkSigMode mode,
             const void *ctx, size_t ctx_len,
             uint8_t *sig, size_t *sig_len)
{
    /* Already validated by the dispatcher via axl_pk_sig_params_ok()
       before this is called -- only the pure mode exists, so there is
       nothing left for this provider to branch on. */
    (void)mode;
    (void)ctx;
    (void)ctx_len;

    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (sig_len == NULL || (msg == NULL && msg_len != 0) || rng == NULL) {
        return AXL_ERR;
    }

    uint8_t           hash[48];   /* fits SHA-256 and SHA-384 */
    size_t            hlen = 0;
    mbedtls_md_type_t md   = md_for_alg(key->alg);
    if (!compute_hash(md, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }

    /* Raw (P1363) ECDSA: fixed r||s (what SSH/JWS/COSE use) — 64 bytes for
       P-256, 96 for P-384. mbedtls signs in DER, so sign then convert. */
    bool   is_ec   = (key->alg == AXL_PK_ECDSA_P256
                      || key->alg == AXL_PK_ECDSA_P384);
    bool   raw     = (is_ec && fmt == AXL_PK_SIG_RAW);
    size_t raw_len = is_ec ? 2u * ec_order_bytes(key->alg) : 0;

    /* Size query: raw is exact; DER/RSA report a safe upper bound (an
       ECDSA DER signature's length varies a couple of bytes between
       signatures, so a per-signature value would not be a safe bound). */
    if (sig == NULL) {
        *sig_len = raw ? raw_len : (size_t)MBEDTLS_PK_SIGNATURE_MAX_SIZE;
        return AXL_OK;
    }
    if (raw && *sig_len < raw_len) {
        *sig_len = raw_len;
        return AXL_ERR;
    }

    uint8_t tmp[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t  olen = 0;
    if (mbedtls_pk_sign(pk, md, hash, hlen,
                        tmp, sizeof(tmp), &olen,
                        mbedtls_ctr_drbg_random, rng) != 0) {
        return AXL_ERR;
    }

    if (raw) {
        if (ecdsa_der_to_raw(tmp, olen, sig, ec_order_bytes(key->alg))
                != AXL_OK) {
            return AXL_ERR;
        }
        *sig_len = raw_len;
        return AXL_OK;
    }

    /* DER (ECDSA) or PKCS#1 (RSA). */
    if (*sig_len < olen) {
        *sig_len = olen;
        return AXL_ERR;
    }
    axl_memcpy(sig, tmp, olen);
    *sig_len = olen;
    return AXL_OK;
}

static int
mbedtls_verify(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
               AxlPkSigFormat fmt, AxlPkSigMode mode,
               const void *ctx, size_t ctx_len,
               const uint8_t *sig, size_t sig_len)
{
    (void)mode;
    (void)ctx;
    (void)ctx_len;

    uint8_t           hash[48];   /* fits SHA-256 and SHA-384 */
    size_t            hlen = 0;
    mbedtls_md_type_t md   = md_for_alg(key->alg);
    if (!compute_hash(md, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }

    /* Raw (P1363) ECDSA: convert r||s to DER, then verify via the pk
       path (which expects DER). */
    bool is_ec = (key->alg == AXL_PK_ECDSA_P256
                  || key->alg == AXL_PK_ECDSA_P384);
    if (is_ec && fmt == AXL_PK_SIG_RAW) {
        size_t order = ec_order_bytes(key->alg);
        if (sig_len != 2u * order) {
            return AXL_ERR;
        }
        /* SEQ of two INTEGERs, each <= order+1 content bytes + 2 header. */
        uint8_t  der[2u * (AXL_P384_ORDER_BYTES + 3u) + 4u];
        uint8_t *dp      = NULL;
        size_t   der_len = 0;
        if (ecdsa_raw_to_der(sig, order, der, sizeof(der), &dp, &der_len)
                != AXL_OK) {
            return AXL_ERR;
        }
        return (mbedtls_pk_verify(pk, md, hash, hlen, dp, der_len) == 0)
                   ? AXL_OK : AXL_ERR;
    }

    return (mbedtls_pk_verify(pk, md, hash, hlen, sig, sig_len) == 0)
               ? AXL_OK : AXL_ERR;
}

/* Populates key->u.mbedtls directly via ec_xy_populate() -- key is
   already allocated and key_init'd by the dispatcher, and key->alg is
   already the caller's chosen algorithm (unlike DER import,
   raw_public_import is never asked to classify content: the caller
   already knows what it's importing), so there is no separate
   allocate-and-copy step, just the shared point-import logic applied
   to the arm we were handed. Returns false for RSA
   (ec_group_for_alg() is false), per axl-pk-provider.h. */
static bool
mbedtls_raw_public_import(AxlPkKey *key, const uint8_t *raw, size_t len)
{
    mbedtls_ecp_group_id grp_id;
    size_t               flen;
    if (raw == NULL || !ec_group_for_alg(key->alg, &grp_id, &flen)
        || len != 1u + 2u * flen || raw[0] != 0x04) {
        return false;
    }
    return ec_xy_populate(key, grp_id, flen,
                          raw + 1u, flen, raw + 1u + flen, flen);
}

/* Thin wrapper over axl_pk_key_get_ec_xy(), which already does the SEC1
   point work -- join the two coordinates it returns into 0x04 || X || Y.
   Returns AXL_ERR for RSA (ec_order_bytes() is 0), per axl-pk-provider.h. */
static int
mbedtls_raw_public_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    size_t flen = ec_order_bytes(key->alg);
    if (flen == 0 || len == NULL) {
        return AXL_ERR;
    }
    size_t need = 1u + 2u * flen;
    if (out == NULL) {
        *len = need;
        return AXL_OK;
    }
    if (*len < need) {
        *len = need;
        return AXL_ERR;
    }

    size_t xlen = flen, ylen = flen;
    if (axl_pk_key_get_ec_xy(key, out + 1u, &xlen, out + 1u + flen, &ylen)
            != AXL_OK) {
        return AXL_ERR;
    }
    out[0] = 0x04;
    *len   = need;
    return AXL_OK;
}

static bool
mbedtls_der_private_import(AxlPkKey *key, const uint8_t *der, size_t len)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL) {
        return false;
    }
    /* Construction: address the arm directly. The DER's own content --
       not any pre-set tag -- determines the algorithm, via
       classify_pk() below. */
    mbedtls_pk_context *ctx = &key->u.mbedtls;
    return mbedtls_pk_parse_key(ctx, der, len, NULL, 0,
                                mbedtls_ctr_drbg_random, rng) == 0
        && classify_pk(ctx, &key->alg);
}

static bool
mbedtls_der_public_import(AxlPkKey *key, const uint8_t *der, size_t len)
{
    /* Construction: address the arm directly (see
       mbedtls_der_private_import() above). */
    mbedtls_pk_context *ctx = &key->u.mbedtls;
    return mbedtls_pk_parse_public_key(ctx, der, len) == 0
        && classify_pk(ctx, &key->alg);
}

static int
mbedtls_der_private_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    return emit_der(mbedtls_pk_write_key_der, pk, out, len);
}

static int
mbedtls_der_public_export(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    mbedtls_pk_context *pk = mbedtls_ctx(key);
    if (pk == NULL) {
        return AXL_ERR;
    }
    return emit_der(mbedtls_pk_write_pubkey_der, pk, out, len);
}

const AxlPkProvider _axl_pk_provider_mbedtls = {
    .key_init           = mbedtls_key_init,
    .key_free           = mbedtls_key_free,
    .keygen             = mbedtls_keygen,
    .sign               = mbedtls_sign,
    .verify             = mbedtls_verify,
    .raw_public_import  = mbedtls_raw_public_import,
    .raw_public_export  = mbedtls_raw_public_export,
    .der_private_import = mbedtls_der_private_import,
    .der_public_import  = mbedtls_der_public_import,
    .der_private_export = mbedtls_der_private_export,
    .der_public_export  = mbedtls_der_public_export,
};
