/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ecdh.c
    Ephemeral elliptic-curve Diffie-Hellman (P-256 and X25519).

    When AXL_HAVE_TLS is defined, key agreement over mbedTLS using the
    low-level ecp/ecdh primitives (no struct internals). Otherwise the
    constructor returns NULL and operations return AXL_ERR (fail-closed).
**/

#include <axl/axl-crypto.h>


#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-crypto-internal.h"

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>

/* The ECDH shared secret is 32 bytes for both curves we expose. */
#define AXL_ECDH_SECRET_LEN  32u

struct AxlEcdh {
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;   /* our private scalar */
    mbedtls_ecp_point Q;   /* our public point */
    AxlEcdhAlg        alg;
};

AxlEcdh *
axl_ecdh_new(AxlEcdhAlg alg)
{
    mbedtls_ecp_group_id grp_id;
    switch (alg) {
    case AXL_ECDH_P256:   grp_id = MBEDTLS_ECP_DP_SECP256R1; break;
    case AXL_ECDH_X25519: grp_id = MBEDTLS_ECP_DP_CURVE25519; break;
    default:              return NULL;
    }
    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    if (rng == NULL) {
        return NULL;
    }

    AxlEcdh *e = axl_malloc(sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->alg = alg;
    mbedtls_ecp_group_init(&e->grp);
    mbedtls_mpi_init(&e->d);
    mbedtls_ecp_point_init(&e->Q);

    if (mbedtls_ecp_group_load(&e->grp, grp_id) != 0
        || mbedtls_ecdh_gen_public(&e->grp, &e->d, &e->Q,
                                   mbedtls_ctr_drbg_random, rng) != 0) {
        axl_ecdh_free(e);
        return NULL;
    }
    return e;
}

int
axl_ecdh_get_public(AxlEcdh *e, uint8_t *out, size_t *len)
{
    if (e == NULL || len == NULL) {
        return AXL_ERR;
    }
    /* Write to a scratch buffer (an uncompressed P-256 point is 65 bytes,
       X25519 is 32), then apply the output-buffer protocol. */
    uint8_t tmp[65];
    size_t  olen = 0;
    if (mbedtls_ecp_point_write_binary(&e->grp, &e->Q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                       tmp, sizeof(tmp)) != 0) {
        return AXL_ERR;
    }
    if (out == NULL) {
        *len = olen;
        return AXL_OK;
    }
    if (*len < olen) {
        *len = olen;
        return AXL_ERR;
    }
    axl_memcpy(out, tmp, olen);
    *len = olen;
    return AXL_OK;
}

int
axl_ecdh_compute(AxlEcdh *e, const uint8_t *peer_pub, size_t peer_len,
                 uint8_t *out, size_t *len)
{
    if (e == NULL || len == NULL || peer_pub == NULL || peer_len == 0) {
        return AXL_ERR;
    }
    if (out == NULL) {            /* size query */
        *len = AXL_ECDH_SECRET_LEN;
        return AXL_OK;
    }
    if (*len < AXL_ECDH_SECRET_LEN) {
        *len = AXL_ECDH_SECRET_LEN;
        return AXL_ERR;
    }

    mbedtls_ctr_drbg_context *rng = axl_crypto_rng();
    mbedtls_ecp_point         peer;
    mbedtls_mpi               z;
    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&z);

    int rc = AXL_ERR;
    if (rng != NULL
        && mbedtls_ecp_point_read_binary(&e->grp, &peer, peer_pub, peer_len) == 0
        && mbedtls_ecp_check_pubkey(&e->grp, &peer) == 0
        && mbedtls_ecdh_compute_shared(&e->grp, &z, &peer, &e->d,
                                       mbedtls_ctr_drbg_random, rng) == 0
        && mbedtls_mpi_write_binary(&z, out, AXL_ECDH_SECRET_LEN) == 0) {
        /* mbedtls renders the shared coordinate big-endian. P-256 wants
           the big-endian X (SEC1 / SSH ecdh-sha2-nistp256); X25519 wants
           the little-endian octet string (RFC 7748 / SSH curve25519), so
           byte-reverse it there. */
        if (e->alg == AXL_ECDH_X25519) {
            for (size_t i = 0; i < AXL_ECDH_SECRET_LEN / 2; i++) {
                uint8_t t = out[i];
                out[i] = out[AXL_ECDH_SECRET_LEN - 1 - i];
                out[AXL_ECDH_SECRET_LEN - 1 - i] = t;
            }
        }
        *len = AXL_ECDH_SECRET_LEN;
        rc   = AXL_OK;
    }

    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    return rc;
}

void
axl_ecdh_free(AxlEcdh *e)
{
    if (e != NULL) {
        mbedtls_ecp_group_free(&e->grp);
        mbedtls_mpi_free(&e->d);
        mbedtls_ecp_point_free(&e->Q);
        axl_free(e);
    }
}

