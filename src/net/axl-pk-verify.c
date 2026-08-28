/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-verify.c
    Public-key dispatch (axl_pk_key_* family) and raw-bytes verification
    (axl_pk_verify).

    AxlPkKey routes every operation through a per-algorithm provider
    (see axl-pk-provider.h): argument validation that does not depend
    on a specific algorithm lives here, the mbedTLS-specific bodies
    live in axl-pk-mbedtls.c. This is also the one translation unit
    that references a provider by a possibly-unresolved weak symbol —
    see the AXL_PK_PROVIDER_WEAK comment below.
**/

#include <axl/axl-crypto.h>

/* This TU makes the (possibly weak) reference to each provider extern,
   so it -- and only it -- defines AXL_PK_PROVIDER_WEAK before pulling
   in the header. See axl-pk-provider.h for why the weak attribute must
   not appear on the declaration a provider's own translation unit
   sees. */
#define AXL_PK_PROVIDER_WEAK
#include "axl-pk-provider.h"

bool
axl_pk_available(void)
{
    return true;
}

/**
 * Resolve the provider for @a alg, or NULL if this image has none.
 * Named per-provider rather than table-driven: see the comment beside
 * the extern declarations in axl-pk-provider.h for why reading a
 * provider's own `alg` field would be unsafe for the weak case.
 */
const AxlPkProvider *
_axl_pk_provider_for(AxlPkAlg alg)
{
    switch (alg) {
    case AXL_PK_ECDSA_P256:
    case AXL_PK_ECDSA_P384:
    case AXL_PK_RSA:
        /* One provider serves all three -- they differ only by the
           curve, which the key already carries. */
        return &_axl_pk_provider_mbedtls;
    case AXL_PK_ED25519:
        /* Weak: the symbol is unresolved unless the image linked an
           Ed25519 provider with `-u _axl_pk_provider_ed25519`, and
           then its ADDRESS is NULL. Test the address -- there is no
           object to read, and under UEFI a load from 0 returns zero
           rather than faulting. The 30 KB base-point table must not
           land in an image that never asked for it. */
        return (&_axl_pk_provider_ed25519 != NULL)
                   ? &_axl_pk_provider_ed25519 : NULL;
    }
    return NULL;
}

bool
axl_pk_alg_available(AxlPkAlg alg)
{
    return axl_pk_available() && _axl_pk_provider_for(alg) != NULL;
}


// ===================================================================
// ECDSA P-256 raw-bytes verification (axl_pk_verify) via mbedTLS.
// ===================================================================
//
// Distinct from the AxlPkKey family above: this takes a DER public key
// directly, with no persistent key handle and no provider involved --
// it predates the provider seam and is ECDSA-P256-only by contract.

#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include "axl-crypto-internal.h"

#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

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

AxlPkKey *
axl_pk_key_new(AxlPkAlg alg)
{
    const AxlPkProvider *p = _axl_pk_provider_for(alg);
    if (p == NULL || p->key_init == NULL || p->keygen == NULL) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    /* key_alloc()'s AXL_PK_ALG_UNCLASSIFIED placeholder is for the
       DER-import path below, where the algorithm is not known yet.
       Here it is -- overwrite immediately. */
    k->alg = alg;

    if (!p->key_init(k)) {
        if (p->key_free != NULL) {
            p->key_free(k);
        }
        axl_free(k);
        return NULL;
    }
    if (!p->keygen(k)) {
        if (p->key_free != NULL) {
            p->key_free(k);
        }
        axl_free(k);
        return NULL;
    }
    k->has_private = true;
    return k;
}

/* A mismatched OID is an ordinary false, per axl-pk-provider.h's
   contract, so the bytes go to each provider in turn until one claims
   them. This is what lets a later algorithm (e.g. an Ed25519 DER
   import in some future phase) join without editing this function --
   the alternative, hardcoding _axl_pk_provider_mbedtls here, is
   exactly the dispatcher surgery the DER vtable seat exists to avoid,
   and would silently turn into a STRONG reference to a future
   ref10-backed provider the day someone edited this function to add
   it.

   The Ed25519 slot is resolved through _axl_pk_provider_for(), the
   one place that tests the weak symbol's address -- never by reading
   a member off of it directly here. Reusing that resolver (rather
   than repeating the `&_axl_pk_provider_ed25519 != NULL` test inline)
   means this candidate list's correctness does not depend on the
   compiler keeping a second, independent instance of the same
   never-true-after-link-time comparison: _axl_pk_provider_for's is
   the one every other entry point already relies on. */
static AxlPkKey *
der_import_any(const uint8_t *der, size_t len, bool want_private)
{
    const AxlPkProvider *cands[2];
    size_t               n = 0;

    cands[n++] = &_axl_pk_provider_mbedtls;
    const AxlPkProvider *ed25519 = _axl_pk_provider_for(AXL_PK_ED25519);
    if (ed25519 != NULL) {
        cands[n++] = ed25519;
    }

    for (size_t i = 0; i < n; i++) {
        const AxlPkProvider *p      = cands[i];
        bool (*import)(AxlPkKey *, const uint8_t *, size_t) =
            want_private ? p->der_private_import : p->der_public_import;
        if (p->key_init == NULL || import == NULL) {
            continue;
        }

        AxlPkKey *k = key_alloc();
        if (k == NULL) {
            return NULL;
        }
        if (p->key_init(k) && import(k, der, len)) {
            k->has_private = want_private;
            return k;
        }
        /* Free through THIS candidate's own key_free, not through
           axl_pk_key_free(): a failed import may leave k->alg at
           key_alloc()'s AXL_PK_ALG_UNCLASSIFIED sentinel --
           der_private_import / der_public_import overwrite it only on
           success -- so resolving a provider from the tag here would
           find no provider at all (the sentinel is deliberately
           out of AxlPkAlg's range, permanently, regardless of which
           providers this image links) and skip freeing what key_init
           actually set up. `p` is already the provider that ran
           key_init on `k`, so it is the only correct one to free
           with. */
        if (p->key_free != NULL) {
            p->key_free(k);
        }
        axl_free(k);
    }
    return NULL;
}

AxlPkKey *
axl_pk_key_load_private(const uint8_t *der, size_t len)
{
    if (der == NULL || len == 0) {
        return NULL;
    }
    return der_import_any(der, len, true);
}

AxlPkKey *
axl_pk_key_load_public(const uint8_t *der, size_t len)
{
    if (der == NULL || len == 0) {
        return NULL;
    }
    return der_import_any(der, len, false);
}

AxlPkKey *
axl_pk_key_from_raw_public(AxlPkAlg alg, const uint8_t *raw, size_t len)
{
    const AxlPkProvider *p = _axl_pk_provider_for(alg);
    if (p == NULL || p->key_init == NULL || p->raw_public_import == NULL
        || raw == NULL || len == 0) {
        return NULL;
    }

    AxlPkKey *k = key_alloc();
    if (k == NULL) {
        return NULL;
    }
    /* Unlike DER import, the caller already names the algorithm, so
       the tag is set immediately -- exactly as axl_pk_key_new() does
       -- and stays correct through the error path below: `p` is
       already the provider that ran key_init on `k`, so free through
       it directly rather than axl_pk_key_free(), matching
       axl_pk_key_new()'s and der_import_any()'s own construction-
       error handling. */
    k->alg = alg;

    if (!p->key_init(k) || !p->raw_public_import(k, raw, len)) {
        if (p->key_free != NULL) {
            p->key_free(k);
        }
        axl_free(k);
        return NULL;
    }
    k->has_private = false;
    return k;
}

int
axl_pk_key_get_raw_public(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    if (key == NULL || len == NULL) {
        return AXL_ERR;
    }
    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p == NULL || p->raw_public_export == NULL) {
        return AXL_ERR;
    }
    return p->raw_public_export(key, out, len);
}

int
axl_pk_key_get_private_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    if (key == NULL || !key->has_private) {
        return AXL_ERR;
    }
    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p == NULL || p->der_private_export == NULL) {
        return AXL_ERR;
    }
    return p->der_private_export(key, out, len);
}

int
axl_pk_key_get_public_der(const AxlPkKey *key, uint8_t *out, size_t *len)
{
    if (key == NULL) {
        return AXL_ERR;
    }
    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p == NULL || p->der_public_export == NULL) {
        return AXL_ERR;
    }
    return p->der_public_export(key, out, len);
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
    if (key == NULL || !key->has_private || sig_len == NULL
        || (msg == NULL && msg_len != 0)) {
        return AXL_ERR;
    }
    /* The public API has no mode/context parameter yet (see
       axl-pk-provider.h's AxlPkSigMode) -- this is the one legal
       combination, checked explicitly so the dispatcher visibly keeps
       the contract axl_pk_sig_params_ok() documents rather than
       relying on these literals matching it by construction. */
    if (!axl_pk_sig_params_ok(AXL_PK_SIG_MODE_PURE, NULL, 0)) {
        return AXL_ERR;
    }

    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p == NULL || p->sign == NULL) {
        return AXL_ERR;
    }
    return p->sign(key, msg, msg_len, fmt, AXL_PK_SIG_MODE_PURE, NULL, 0,
                   sig, sig_len);
}

int
axl_pk_key_verify(const AxlPkKey *key, const uint8_t *msg, size_t msg_len,
                  AxlPkSigFormat fmt, const uint8_t *sig, size_t sig_len)
{
    if (key == NULL || sig == NULL || sig_len == 0
        || (msg == NULL && msg_len != 0)) {
        return AXL_ERR;
    }
    if (!axl_pk_sig_params_ok(AXL_PK_SIG_MODE_PURE, NULL, 0)) {
        return AXL_ERR;
    }

    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p == NULL || p->verify == NULL) {
        return AXL_ERR;
    }
    return p->verify(key, msg, msg_len, fmt, AXL_PK_SIG_MODE_PURE, NULL, 0,
                     sig, sig_len);
}

void
axl_pk_key_free(AxlPkKey *key)
{
    if (key == NULL) {
        return;
    }
    const AxlPkProvider *p = _axl_pk_provider_for(key->alg);
    if (p != NULL && p->key_free != NULL) {
        p->key_free(key);
    }
    axl_free(key);
}
