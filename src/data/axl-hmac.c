/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hmac.c
    HMAC (RFC 2104) over the AxlChecksum digest engine.

    HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m)), where K' is the
    key padded to the hash's BLOCK size (longer keys are hashed
    first). MD5, SHA-1 and SHA-256 use a 64-byte block; SHA-512 uses
    128. The block is a property of the hash, not of its digest width.
**/

#include <axl/axl-hmac.h>
#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#define HMAC_MAX_BLOCK  128  // SHA-512's block; MD5/SHA-1/SHA-256 use 64
#define HMAC_MAX_DIGEST 64   // SHA-512 is the widest supported digest

struct AxlHmac {
    AxlChecksumType type;
    AxlChecksum    *inner;                     // H((K^ipad) || message)
    size_t          block;                     // block size for `type`
    uint8_t         opad[HMAC_MAX_BLOCK];      // K ^ opad, applied at finalize
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

// HMAC's block size is the *hash's* block, not its digest width, and
// the two do not track each other: SHA-512 has a 64-byte digest and a
// 128-byte block. Returns 0 for an algorithm we cannot MAC with, which
// axl_hmac_new turns into a NULL -- failing closed beats MACing with
// the wrong block and producing a plausible, wrong answer.
static size_t
hmac_block_size(AxlChecksumType type)
{
    switch (type) {
    case AXL_CHECKSUM_MD5:
    case AXL_CHECKSUM_SHA1:
    case AXL_CHECKSUM_SHA256:
        return 64;
    case AXL_CHECKSUM_SHA512:
        return 128;
    }
    // No default: label -- an out-of-range cast falls through to this
    // return, but a fifth enumerator left unhandled here fails the
    // build with -Wswitch instead of silently returning 0 for it.
    return 0;
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
    axl_checksum_update(outer, h->opad, h->block);
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
    uint8_t  k[HMAC_MAX_BLOCK];
    uint8_t  ipad[HMAC_MAX_BLOCK];
    AxlHmac *h;

    size_t block = hmac_block_size(type);
    if (block == 0) {
        return NULL;  // unsupported algorithm
    }
    if (key == NULL && key_len > 0) {
        return NULL;
    }

    // K' = key padded to the block with zeros; keys longer than the
    // block are hashed down first (RFC 2104).
    axl_memset(k, 0, sizeof(k));
    if (key_len > block) {
        AxlChecksum *kc = axl_checksum_new(type);
        size_t       klen = block;
        if (kc == NULL) {
            return NULL;
        }
        axl_checksum_update(kc, key, key_len);
        axl_checksum_get_digest(kc, k, &klen);  // writes klen bytes; rest stay 0
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
    h->block = block;
    h->inner = axl_checksum_new(type);
    if (h->inner == NULL) {
        secure_zero(k, sizeof(k));
        axl_free(h);
        return NULL;
    }

    for (size_t i = 0; i < block; i++) {
        ipad[i]    = k[i] ^ 0x36;
        h->opad[i] = k[i] ^ 0x5C;
    }
    axl_checksum_update(h->inner, ipad, block);

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
    /* Report what was WRITTEN -- same reasoning as
       axl_checksum_get_digest(). Note *len == 0 remains the
       failed-finalize signal: a successful call with a zero-size
       buffer also reports 0, which is degenerate and which no caller
       does. */
    *len = n;
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
