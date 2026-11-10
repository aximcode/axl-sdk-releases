/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-ssh-host-key-load.c -- the PROVIDER-BOUND half of
 * <axl/axl-ssh-host-key.h>: loading a record as a signing key, a record from
 * a key, and load-or-create over the default variable.
 *
 * Separate from axl-ssh-host-key.c so the provider-free half stays free of
 * the Ed25519 provider: this file calls axl_pk_enable_ed25519(), which is
 * what links it.
 */

#include <axl/axl-ssh-host-key.h>
#include <axl/axl-crypto.h>
#include <axl/axl-str.h>

#include "axl-crypto-internal.h"   /* axl_crypto_entropy_was_weak */
#include "axl-ssh-host-key-internal.h"

static void
set_why(const char **why, const char *reason)
{
    if (why != NULL) {
        *why = reason;
    }
}

int
axl_ssh_host_key_load(const AxlSshHostKeyRecord *rec, AxlPkKey **key,
                      const char **why)
{
    static const char NOT_USABLE[] = "the private key is not a usable Ed25519 key";

    if (key != NULL) {
        *key = NULL;
    }
    if (rec == NULL || key == NULL) {
        return AXL_INVALID;
    }
    axl_pk_enable_ed25519();
    if (!axl_ssh_hk_pkcs8_is_ed25519(rec->pkcs8)) {
        set_why(why, NOT_USABLE);
        return AXL_ERR;
    }
    AxlPkKey *k = axl_pk_key_load_private(rec->pkcs8, sizeof rec->pkcs8);
    if (k == NULL) {
        set_why(why, NOT_USABLE);
        return AXL_ERR;
    }
    /* The seed must derive the stored public key. A flipped bit in the seed
       loads cleanly as ANOTHER key, so without this a corrupted store is a
       silent change of host identity. */
    uint8_t pub[AXL_SSH_ED25519_PUBLIC_LEN];
    size_t  n = sizeof pub;
    if (axl_pk_key_get_raw_public(k, pub, &n) != AXL_OK || n != sizeof pub
        || axl_memcmp(pub, rec->public_key, sizeof pub) != 0) {
        axl_pk_key_free(k);
        set_why(why, "the private key does not match its public key");
        return AXL_ERR;
    }
    *key = k;
    return AXL_OK;
}

int
axl_ssh_host_key_record_from_key(const AxlPkKey *key, AxlSshHostKeyRecord *out)
{
    if (out == NULL) {
        return AXL_INVALID;
    }
    axl_ssh_host_key_record_clear(out);
    if (key == NULL || axl_pk_key_alg(key) != AXL_PK_ED25519) {
        return AXL_INVALID;
    }
    size_t der_len = sizeof out->pkcs8;
    size_t pub_len = sizeof out->public_key;
    if (axl_pk_key_get_private_der(key, out->pkcs8, &der_len) != AXL_OK
        || der_len != sizeof out->pkcs8
        || !axl_ssh_hk_pkcs8_is_ed25519(out->pkcs8)
        || axl_pk_key_get_raw_public(key, out->public_key, &pub_len) != AXL_OK
        || pub_len != sizeof out->public_key) {
        axl_ssh_host_key_record_clear(out);
        return AXL_INVALID;
    }
    return AXL_OK;
}

int
axl_ssh_host_key_nv_load_or_create(AxlPkKey **key, AxlSshHostKeyOrigin *origin,
                                   const char **why)
{
    AxlSshHostKeyRecord rec;
    const char         *reason = NULL;

    if (key != NULL) {
        *key = NULL;
    }
    if (key == NULL || origin == NULL) {
        return AXL_INVALID;
    }
    int rc = axl_ssh_host_key_nv_read(&rec, &reason);
    if (rc == AXL_OK) {
        rc = axl_ssh_host_key_load(&rec, key, &reason);
        axl_ssh_host_key_record_clear(&rec);
        if (rc == AXL_OK) {
            *origin = AXL_SSH_HOST_KEY_LOADED;
            return AXL_OK;
        }
        set_why(why, reason);
        return rc;
    }
    if (rc != AXL_NOT_FOUND) {
        /* Present but unusable, or unreadable: NEVER regenerated. */
        set_why(why, reason);
        return rc;
    }

    AxlPkKey *k = axl_pk_key_new(AXL_PK_ED25519);
    if (k == NULL || axl_ssh_host_key_record_from_key(k, &rec) != AXL_OK) {
        axl_pk_key_free(k);
        set_why(why, "cannot generate a key");
        return AXL_NO_RESOURCES;
    }
    if (axl_crypto_entropy_was_weak()) {
        /* Guessable by anyone who can bound when the machine booted. Stored,
           it would outlive this boot and every later one, which is the one
           thing axl_crypto_set_weak_entropy_allowed() forbids. */
        *origin = AXL_SSH_HOST_KEY_EPHEMERAL;
        set_why(why, "weak entropy; the key is not saved");
    } else if (axl_ssh_host_key_nv_write(&rec, &reason) == AXL_OK) {
        *origin = AXL_SSH_HOST_KEY_GENERATED;
    } else {
        *origin = AXL_SSH_HOST_KEY_EPHEMERAL;
        set_why(why, reason);
    }
    axl_ssh_host_key_record_clear(&rec);
    *key = k;
    return AXL_OK;
}
