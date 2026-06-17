/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-verify.c
    Public-key signature verification (axl_pk_verify).

    When AXL_HAVE_TLS is defined, verifies detached signatures via
    mbedTLS. Otherwise axl_pk_available() returns false and
    axl_pk_verify() returns AXL_ERR (fail-closed).
**/

#include <axl/axl-crypto.h>

bool
axl_pk_available(void)
{
#ifdef AXL_HAVE_TLS
    return true;
#else
    return false;
#endif
}

#ifndef AXL_HAVE_TLS

// ===================================================================
// Stub — verification not compiled in (fail-closed).
// ===================================================================

int
axl_pk_verify(AxlPkAlg alg,
              const uint8_t *pubkey, size_t pubkey_len,
              const uint8_t *msg, size_t msg_len,
              const uint8_t *sig, size_t sig_len)
{
    (void)alg; (void)pubkey; (void)pubkey_len;
    (void)msg; (void)msg_len; (void)sig; (void)sig_len;
    return AXL_ERR;
}

AxlPkKey *axl_pk_keygen(AxlPkAlg alg)
{ (void)alg; return NULL; }
AxlPkKey *axl_pk_key_load_private(const uint8_t *der, size_t len)
{ (void)der; (void)len; return NULL; }
AxlPkKey *axl_pk_key_load_public(const uint8_t *der, size_t len)
{ (void)der; (void)len; return NULL; }
int axl_pk_key_get_private_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{ (void)key; (void)out; (void)len; return AXL_ERR; }
int axl_pk_key_get_public_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{ (void)key; (void)out; (void)len; return AXL_ERR; }
AxlPkAlg axl_pk_key_alg(const AxlPkKey *key)
{ (void)key; return AXL_PK_ED25519; }
int axl_pk_key_sign(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                    AxlPkSigFormat fmt, uint8_t *sig, size_t *sig_len)
{ (void)key; (void)msg; (void)msg_len; (void)fmt; (void)sig; (void)sig_len;
  return AXL_ERR; }
int axl_pk_key_verify(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                      AxlPkSigFormat fmt, const uint8_t *sig, size_t sig_len)
{ (void)key; (void)msg; (void)msg_len; (void)fmt; (void)sig; (void)sig_len;
  return AXL_ERR; }
void axl_pk_key_free(AxlPkKey *key)
{ (void)key; }

#else /* AXL_HAVE_TLS */

// ===================================================================
// ECDSA P-256 verification via mbedTLS.
// ===================================================================

#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-crypto-internal.h"

#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/rsa.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/asn1.h>
#include <mbedtls/asn1write.h>
#include <mbedtls/bignum.h>

/* ECDSA curve order sizes. A raw (P1363) signature is r||s, each padded
   to the order size: 64 bytes for P-256, 96 for P-384. */
#define AXL_P256_ORDER_BYTES  32u
#define AXL_P384_ORDER_BYTES  48u
/* Upper bound for a serialized key: a 3072-bit RSA PKCS#8 private key is
   ~1.9 KB; P-256 keys are far smaller. 4 KB covers every key AXL makes. */
#define AXL_PK_DER_MAX        4096u

// -------------------------------------------------------------------
// Key handle
// -------------------------------------------------------------------

struct AxlPkKey {
    mbedtls_pk_context pk;
    AxlPkAlg           alg;
    bool               has_private;
};

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

/* The message-digest a key-handle algorithm signs over: the ECDSA hash
   follows the curve (P-256 -> SHA-256, P-384 -> SHA-384); RSA PKCS#1 v1.5
   here is always SHA-256 (RS256). */
static mbedtls_md_type_t
md_for_alg(AxlPkAlg alg)
{
    return (alg == AXL_PK_ECDSA_P384) ? MBEDTLS_MD_SHA384
                                      : MBEDTLS_MD_SHA256;
}

/* The ECDSA curve order size in bytes for a raw r||s signature. */
static size_t
ec_order_bytes(AxlPkAlg alg)
{
    return (alg == AXL_PK_ECDSA_P384) ? AXL_P384_ORDER_BYTES
                                      : AXL_P256_ORDER_BYTES;
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

/**
 * Verify a detached ECDSA-with-SHA-256 signature over @p msg using the
 * P-256 public key in @p pubkey (DER SubjectPublicKeyInfo) and the DER
 * ECDSA signature in @p sig. Returns AXL_OK only on a valid signature.
 */
static int
verify_ecdsa_p256(const uint8_t *pubkey, size_t pubkey_len,
                  const uint8_t *msg, size_t msg_len,
                  const uint8_t *sig, size_t sig_len)
{
    mbedtls_pk_context pk;
    uint8_t            hash[32];
    int                rc = AXL_ERR;

    mbedtls_pk_init(&pk);

    if (mbedtls_pk_parse_public_key(&pk, pubkey, pubkey_len) != 0) {
        goto out;
    }

    /* Enforce the contracted algorithm: an EC key on NIST P-256. A key
       on a different curve must not silently verify under this enum. */
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY) ||
        mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(pk)) !=
            MBEDTLS_ECP_DP_SECP256R1) {
        goto out;
    }

    /* ECDSA-with-SHA-256: verify over SHA-256(msg). */
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, msg, msg_len,
                                    hash, sizeof(hash)) != AXL_OK) {
        goto out;
    }

    if (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                          sig, sig_len) == 0) {
        rc = AXL_OK;
    }

out:
    mbedtls_pk_free(&pk);
    return rc;
}

int
axl_pk_verify(AxlPkAlg alg,
              const uint8_t *pubkey, size_t pubkey_len,
              const uint8_t *msg, size_t msg_len,
              const uint8_t *sig, size_t sig_len)
{
    /* Argument validation precedes any crypto. msg may be NULL only
       when msg_len is 0 (an empty message is well-defined). */
    if (pubkey == NULL || pubkey_len == 0 ||
        sig == NULL || sig_len == 0 ||
        (msg == NULL && msg_len != 0)) {
        return AXL_ERR;
    }

    switch (alg) {
    case AXL_PK_ECDSA_P256:
        return verify_ecdsa_p256(pubkey, pubkey_len, msg, msg_len,
                                 sig, sig_len);
    case AXL_PK_ED25519:
        /* Reserved; not supported by this mbedTLS build. */
    default:
        return AXL_ERR;
    }
}

// -------------------------------------------------------------------
// Key-handle API
// -------------------------------------------------------------------

static AxlPkKey *
key_alloc(void)
{
    AxlPkKey *k = axl_malloc(sizeof(*k));
    if (k != NULL) {
        mbedtls_pk_init(&k->pk);
        k->alg         = AXL_PK_ED25519;
        k->has_private = false;
    }
    return k;
}

AxlPkKey *
axl_pk_keygen(AxlPkAlg alg)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }

    int rc = -1;
    if (alg == AXL_PK_ECDSA_P256 || alg == AXL_PK_ECDSA_P384) {
        mbedtls_ecp_group_id grp =
            (alg == AXL_PK_ECDSA_P384) ? MBEDTLS_ECP_DP_SECP384R1
                                       : MBEDTLS_ECP_DP_SECP256R1;
        if (mbedtls_pk_setup(&k->pk,
                mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0) {
            rc = mbedtls_ecp_gen_key(grp, mbedtls_pk_ec(k->pk),
                                     mbedtls_ctr_drbg_random, rng);
        }
    } else if (alg == AXL_PK_RSA) {
        if (mbedtls_pk_setup(&k->pk,
                mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
            rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(k->pk),
                                     mbedtls_ctr_drbg_random, rng,
                                     3072, 65537);
        }
    }

    if (rc != 0) {
        axl_pk_key_free(k);
        return NULL;
    }
    k->alg         = alg;
    k->has_private = true;
    return k;
}

AxlPkKey *
axl_pk_key_load_private(const uint8_t *der, size_t len)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (der == NULL || len == 0 || rng == NULL) {
        return NULL;
    }
    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    if (mbedtls_pk_parse_key(&k->pk, der, len, NULL, 0,
                             mbedtls_ctr_drbg_random, rng) != 0
        || !classify_pk(&k->pk, &k->alg)) {
        axl_pk_key_free(k);
        return NULL;
    }
    k->has_private = true;
    return k;
}

AxlPkKey *
axl_pk_key_load_public(const uint8_t *der, size_t len)
{
    if (der == NULL || len == 0) {
        return NULL;
    }
    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    if (mbedtls_pk_parse_public_key(&k->pk, der, len) != 0
        || !classify_pk(&k->pk, &k->alg)) {
        axl_pk_key_free(k);
        return NULL;
    }
    k->has_private = false;
    return k;
}

int
axl_pk_key_get_private_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    if (key == NULL || !key->has_private) {
        return AXL_ERR;
    }
    return emit_der(mbedtls_pk_write_key_der, &key->pk, out, len);
}

int
axl_pk_key_get_public_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    if (key == NULL) {
        return AXL_ERR;
    }
    return emit_der(mbedtls_pk_write_pubkey_der, &key->pk, out, len);
}

// -------------------------------------------------------------------
// Raw public-key component import/export (JWK support).
// -------------------------------------------------------------------

/* P-256/P-384 field width covers every EC coordinate AXL emits. */
#define AXL_EC_FIELD_MAX  48u

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

AxlPkKey *
axl_pk_key_from_ec_xy(AxlPkAlg alg,
                      const uint8_t *x, size_t x_len,
                      const uint8_t *y, size_t y_len)
{
    mbedtls_ecp_group_id grp_id;
    size_t               flen;
    if (x == NULL || y == NULL
        || !ec_group_for_alg(alg, &grp_id, &flen)
        || x_len == 0 || x_len > flen || y_len == 0 || y_len > flen) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }

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

    int rc = -1;
    if (mbedtls_pk_setup(&k->pk,
            mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0
        && mbedtls_ecp_group_load(&grp, grp_id) == 0
        && mbedtls_ecp_point_read_binary(&grp, &Q, point, plen) == 0
        /* point_read_binary does NOT verify curve membership — a JWK is
           attacker-controlled, so reject an off-curve point explicitly. */
        && mbedtls_ecp_check_pubkey(&grp, &Q) == 0
        && mbedtls_ecp_set_public_key(grp_id, mbedtls_pk_ec(k->pk), &Q) == 0
        && classify_pk(&k->pk, &k->alg)) {
        rc = 0;
    }

    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    if (rc != 0) {
        axl_pk_key_free(k);
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

    int rc = -1;
    if (mbedtls_pk_setup(&k->pk,
            mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(k->pk);
        /* A JWK is attacker-controlled. mbedtls_rsa_complete() accepts a
           degenerate modulus (e.g. a few bytes), so pin a 2048-bit floor —
           the RS256 security level — rather than minting a "valid" handle
           for a key that can never carry a real signature. */
        if (mbedtls_rsa_import_raw(rsa, n, n_len, NULL, 0, NULL, 0,
                                   NULL, 0, e, e_len) == 0
            && mbedtls_rsa_complete(rsa) == 0
            && mbedtls_rsa_get_len(rsa) >= 256u
            && classify_pk(&k->pk, &k->alg)) {
            rc = 0;
        }
    }

    if (rc != 0) {
        axl_pk_key_free(k);
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

    mbedtls_pk_context *pk = (mbedtls_pk_context *)&key->pk;
    mbedtls_ecp_group   grp;
    mbedtls_ecp_point   Q;
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

int
axl_pk_key_get_rsa_ne(const AxlPkKey *key,
                      uint8_t *n, size_t *n_len,
                      uint8_t *e, size_t *e_len)
{
    if (key == NULL || n == NULL || n_len == NULL || e == NULL
        || e_len == NULL || key->alg != AXL_PK_RSA) {
        return AXL_ERR;
    }

    mbedtls_pk_context  *pk   = (mbedtls_pk_context *)&key->pk;
    mbedtls_rsa_context *rsa  = mbedtls_pk_rsa(*pk);
    size_t               klen = mbedtls_rsa_get_len(rsa);
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

AxlPkAlg
axl_pk_key_alg(const AxlPkKey *key)
{
    return (key != NULL) ? key->alg : AXL_PK_ED25519;
}

int
axl_pk_key_sign(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                AxlPkSigFormat fmt, uint8_t *sig, size_t *sig_len)
{
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (key == NULL || !key->has_private || sig_len == NULL
        || (msg == NULL && msg_len != 0) || rng == NULL) {
        return AXL_ERR;
    }

    uint8_t           hash[48];   /* fits SHA-256 and SHA-384 */
    size_t            hlen = 0;
    mbedtls_md_type_t md   = md_for_alg(key->alg);
    if (!compute_hash(md, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = (mbedtls_pk_context *)&key->pk;

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

int
axl_pk_key_verify(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                  AxlPkSigFormat fmt, const uint8_t *sig, size_t sig_len)
{
    if (key == NULL || sig == NULL || sig_len == 0
        || (msg == NULL && msg_len != 0)) {
        return AXL_ERR;
    }

    uint8_t           hash[48];   /* fits SHA-256 and SHA-384 */
    size_t            hlen = 0;
    mbedtls_md_type_t md   = md_for_alg(key->alg);
    if (!compute_hash(md, msg, msg_len, hash, &hlen)) {
        return AXL_ERR;
    }

    mbedtls_pk_context *pk = (mbedtls_pk_context *)&key->pk;

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

// -------------------------------------------------------------------
// RSA-PSS (PS256) sign/verify — JOSE-internal.
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

    mbedtls_pk_context *pk = (mbedtls_pk_context *)&key->pk;
    uint8_t             tmp[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t              olen = 0;
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

    mbedtls_pk_context           *pk = (mbedtls_pk_context *)&key->pk;
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

void
axl_pk_key_free(AxlPkKey *key)
{
    if (key != NULL) {
        mbedtls_pk_free(&key->pk);
        axl_free(key);
    }
}

#endif /* AXL_HAVE_TLS */
