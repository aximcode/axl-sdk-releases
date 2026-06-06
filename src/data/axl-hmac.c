/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hmac.c
    HMAC (RFC 2104) over the AxlChecksum digest engine.

    HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m)), where K' is the
    key padded to the hash block size (keys longer than the block are
    hashed first). MD5, SHA-1, and SHA-256 all use a 64-byte block.
**/

#include <axl/axl-hmac.h>
#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#define HMAC_BLOCK      64   // block size of MD5/SHA-1/SHA-256
#define HMAC_MAX_DIGEST 32   // SHA-256 is the widest supported digest

struct AxlHmac {
    AxlChecksumType type;
    AxlChecksum    *inner;                     // H((K^ipad) || message)
    uint8_t         opad[HMAC_BLOCK];          // K ^ opad, applied at finalize
    bool            finalized;
    uint8_t         digest[HMAC_MAX_DIGEST];   // cached result bytes
    size_t          digest_len;                // 0 after a failed finalize
    char            hexstr[HMAC_MAX_DIGEST * 2 + 1];
};

// Zero memory in a way the optimizer can't elide — used to scrub
// key-derived material (the ipad/opad pads are trivially reversible to
// the key) from the stack and heap.
static void
secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

static void
hmac_finalize(AxlHmac *h)
{
    static const char HEX[] = "0123456789abcdef";
    uint8_t      idig[HMAC_MAX_DIGEST];
    size_t       ilen = sizeof(idig);
    AxlChecksum *outer;

    if (h->finalized) {
        return;
    }
    h->finalized  = true;
    h->digest_len = 0;  // failure sentinel until we have a real digest

    // outer = H((K^opad) || H((K^ipad) || message)). Bail to the
    // zero-length failure state if the outer context can't be allocated,
    // rather than emitting an all-zero (bogus) MAC.
    outer = axl_checksum_new(h->type);
    if (outer == NULL) {
        return;
    }

    axl_checksum_get_digest(h->inner, idig, &ilen);
    axl_checksum_update(outer, h->opad, HMAC_BLOCK);
    axl_checksum_update(outer, idig, ilen);

    size_t dlen = sizeof(h->digest);
    axl_checksum_get_digest(outer, h->digest, &dlen);
    axl_checksum_free(outer);
    h->digest_len = dlen;

    for (size_t i = 0; i < dlen; i++) {
        h->hexstr[2 * i]     = HEX[h->digest[i] >> 4];
        h->hexstr[2 * i + 1] = HEX[h->digest[i] & 0x0F];
    }
    h->hexstr[2 * dlen] = '\0';
}

AxlHmac *
axl_hmac_new(AxlChecksumType type, const void *key, size_t key_len)
{
    uint8_t  k[HMAC_BLOCK];
    uint8_t  ipad[HMAC_BLOCK];
    AxlHmac *h;

    if (axl_checksum_type_get_length(type) == 0) {
        return NULL;  // unsupported algorithm
    }
    if (key == NULL && key_len > 0) {
        return NULL;
    }

    // K' = key padded to the block with zeros; keys longer than the
    // block are hashed down first (RFC 2104).
    axl_memset(k, 0, sizeof(k));
    if (key_len > HMAC_BLOCK) {
        AxlChecksum *kc = axl_checksum_new(type);
        size_t       klen = sizeof(k);
        if (kc == NULL) {
            return NULL;
        }
        axl_checksum_update(kc, key, key_len);
        axl_checksum_get_digest(kc, k, &klen);  // writes digest_len bytes; rest stay 0
        axl_checksum_free(kc);
    } else if (key_len > 0) {
        axl_memcpy(k, key, key_len);
    }

    h = axl_calloc(1, sizeof(*h));
    if (h == NULL) {
        secure_zero(k, sizeof(k));
        return NULL;
    }
    h->type  = type;
    h->inner = axl_checksum_new(type);
    if (h->inner == NULL) {
        secure_zero(k, sizeof(k));
        axl_free(h);
        return NULL;
    }

    for (size_t i = 0; i < HMAC_BLOCK; i++) {
        ipad[i]    = k[i] ^ 0x36;
        h->opad[i] = k[i] ^ 0x5C;
    }
    axl_checksum_update(h->inner, ipad, HMAC_BLOCK);

    // Scrub the key-derived stack material — the pads reveal the key.
    secure_zero(k, sizeof(k));
    secure_zero(ipad, sizeof(ipad));
    return h;
}

void
axl_hmac_update(AxlHmac *h, const void *data, size_t len)
{
    if (h == NULL || h->finalized) {
        return;
    }
    axl_checksum_update(h->inner, data, len);
}

const char *
axl_hmac_get_string(AxlHmac *h)
{
    if (h == NULL) {
        return NULL;
    }
    hmac_finalize(h);
    // digest_len == 0 only if finalize failed (OOM) — no valid MAC.
    return (h->digest_len == 0) ? NULL : h->hexstr;
}

void
axl_hmac_get_digest(AxlHmac *h, uint8_t *buf, size_t *len)
{
    if (h == NULL || buf == NULL || len == NULL) {
        return;
    }
    hmac_finalize(h);

    size_t n = (*len < h->digest_len) ? *len : h->digest_len;
    axl_memcpy(buf, h->digest, n);
    *len = h->digest_len;  // 0 signals a failed (OOM) finalize
}

void
axl_hmac_free(AxlHmac *h)
{
    if (h == NULL) {
        return;
    }
    axl_checksum_free(h->inner);
    secure_zero(h->opad, sizeof(h->opad));  // scrub key-derived pad
    axl_free(h);
}

char *
axl_compute_hmac(
    AxlChecksumType  type,
    const void      *key,
    size_t           key_len,
    const void      *data,
    size_t           data_len
    )
{
    AxlHmac    *h = axl_hmac_new(type, key, key_len);
    const char *hex;
    char       *dup;

    if (h == NULL) {
        return NULL;
    }
    axl_hmac_update(h, data, data_len);
    hex = axl_hmac_get_string(h);
    dup = axl_strdup(hex);
    axl_hmac_free(h);
    return dup;
}
