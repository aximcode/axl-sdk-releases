/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pk-verify.c
    Public-key dispatch (the axl_pk_key_* family).

    AxlPkKey routes every operation through a per-algorithm provider
    (see axl-pk-provider.h): argument validation that does not depend
    on a specific algorithm lives here, the provider bodies live in
    axl-pk-mbedtls.c and axl-pk-ed25519.c. This is also the one
    translation unit that references a provider by a possibly-unresolved
    weak symbol -- see the AXL_PK_PROVIDER_WEAK comment below -- so it
    must reference no mbedTLS symbol itself: anything it named strongly
    would be linked into every image that calls the dispatcher. The
    raw-bytes axl_pk_verify() lives in axl-pk-mbedtls.c for that reason.
**/

#include <axl/axl-crypto.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "axl-crypto-internal.h"

/* This TU makes the (possibly weak) reference to each provider extern,
   so it -- and only it -- defines AXL_PK_PROVIDER_WEAK before pulling
   in the header. See axl-pk-provider.h for why the weak attribute must
   not appear on the declaration a provider's own translation unit
   sees. */
#define AXL_PK_PROVIDER_WEAK
#include "axl-pk-provider.h"

AXL_LOG_DOMAIN("pk");

/* The one line an image gets when a call needs an algorithm whose
   provider it did not enable. It names the function to call, so the
   reader of a log can fix the image without opening a header. The
   query, axl_pk_alg_available(), stays silent: it is how a caller asks
   before committing. Pinned verbatim by AxlTestEd25519 (the mbedTLS
   line) and AxlTestCrypto (the Ed25519 line). */
#define PK_MBEDTLS_ABSENT_LINE \
    "ECDSA/RSA unavailable: call axl_pk_enable_mbedtls() before using " \
    "ECDSA/RSA keys"
#define PK_ED25519_ABSENT_LINE \
    "Ed25519 unavailable: call axl_pk_enable_ed25519() before using " \
    "Ed25519 keys"

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
    /* Both providers are weak: the symbol is unresolved unless the image
       linked it, and then its ADDRESS is NULL. Test the address -- there
       is no object to read, and under UEFI a load from 0 returns zero
       rather than faulting. */
    switch (alg) {
    case AXL_PK_ECDSA_P256:
    case AXL_PK_ECDSA_P384:
    case AXL_PK_RSA:
        /* One provider serves all three -- they differ only by the
           curve, which the key already carries. Linked by a call to
           axl_pk_enable_mbedtls() or any other axl-pk-mbedtls.c
           function (AxlJose makes one). */
        return (&_axl_pk_provider_mbedtls != NULL)
                   ? &_axl_pk_provider_mbedtls : NULL;
    case AXL_PK_ED25519:
        /* Linked by a call to axl_pk_enable_ed25519() (AxlSsh makes
           one). The ~109 KB base-point table must not land in an image
           that never asked for it. */
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

/* An entry point that named @a alg found no provider for it: say so --
   once, naming the function that enables it. */
static void
report_missing_provider(AxlPkAlg alg)
{
    switch (alg) {
    case AXL_PK_ECDSA_P256:
    case AXL_PK_ECDSA_P384:
    case AXL_PK_RSA:
        axl_error(PK_MBEDTLS_ABSENT_LINE);
        return;
    case AXL_PK_ED25519:
        axl_error(PK_ED25519_ABSENT_LINE);
        return;
    }
}

/* Whether DER bytes NAME an algorithm: the id-ecPublicKey,
   rsaEncryption or id-Ed25519 OID, tag and length included. A substring
   search, not a parse, and it decides nothing about acceptance -- it
   only picks whether a refused import was refused for want of a
   provider, so the diagnostic is not printed for DER that was simply
   malformed or names an algorithm whose provider IS linked. A false
   positive costs one log line. Returns true and sets @a *alg on a
   match. */
static bool
der_names_alg(const uint8_t *der, size_t len, AxlPkAlg *alg)
{
    static const uint8_t ec_oid[] = {
        0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01
    };
    static const uint8_t rsa_oid[] = {
        0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01
    };
    static const uint8_t ed25519_oid[] = { 0x06, 0x03, 0x2b, 0x65, 0x70 };

    if (axl_memmem(der, len, ec_oid, sizeof(ec_oid)) != NULL) {
        *alg = AXL_PK_ECDSA_P256;
        return true;
    }
    if (axl_memmem(der, len, rsa_oid, sizeof(rsa_oid)) != NULL) {
        *alg = AXL_PK_RSA;
        return true;
    }
    if (axl_memmem(der, len, ed25519_oid, sizeof(ed25519_oid)) != NULL) {
        *alg = AXL_PK_ED25519;
        return true;
    }
    return false;
}

// -------------------------------------------------------------------
// Key-handle API
// -------------------------------------------------------------------

AxlPkKey *
axl_pk_key_new(AxlPkAlg alg)
{
    const AxlPkProvider *p = _axl_pk_provider_for(alg);
    if (p == NULL) {
        report_missing_provider(alg);
        return NULL;
    }
    if (p->key_init == NULL || p->keygen == NULL) {
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
   contract, so the bytes go to each linked provider in turn until one
   claims them.

   BOTH candidates are resolved through _axl_pk_provider_for(), the one
   place that tests a weak symbol's address -- never by naming a
   provider here. Naming `_axl_pk_provider_mbedtls` directly is exactly
   what kept mbedTLS's whole PK layer (and with it rsa.o) in every
   Ed25519-only image before 2026-10-20; and reusing the resolver
   rather than repeating the `!= NULL` test inline means this list's
   correctness does not depend on the compiler keeping a second,
   independent instance of the same comparison. */
static AxlPkKey *
der_import_any(const uint8_t *der, size_t len, bool want_private)
{
    const AxlPkProvider *cands[2];
    size_t               n = 0;

    const AxlPkProvider *mbedtls = _axl_pk_provider_for(AXL_PK_ECDSA_P256);
    if (mbedtls != NULL) {
        cands[n++] = mbedtls;
    }
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
    AxlPkAlg named;
    if (der_names_alg(der, len, &named)
        && _axl_pk_provider_for(named) == NULL) {
        report_missing_provider(named);
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
    if (p == NULL) {
        report_missing_provider(alg);
        return NULL;
    }
    if (p->key_init == NULL || p->raw_public_import == NULL
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
