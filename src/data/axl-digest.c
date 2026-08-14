/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest.c
    Message digest checksums: MD5 (RFC 1321), SHA-1 (RFC 3174),
    SHA-256 (FIPS 180-4). Standalone — no mbedTLS dependency.

    This file is the public dispatcher. Per-algorithm implementations
    live in axl-digest-md5.c, axl-digest-sha1.c, axl-digest-sha256.c
    and are reached via the prototypes in axl-digest-internal.h.
**/

#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-str.h>
#include <axl/axl-log.h>

#include "axl-digest-internal.h"

AXL_LOG_DOMAIN("data");

#define MAX_DIGEST_LEN 32  /* SHA-256 */
#define MAX_HEX_LEN   65  /* SHA-256 hex + NUL */

struct AxlChecksum {
    AxlChecksumType type;
    bool            closed;
    union {
        Md5State    md5;
        Sha1State   sha1;
        Sha256State sha256;
    };
    uint8_t  digest[MAX_DIGEST_LEN];
    char     hex[MAX_HEX_LEN];
};

size_t
axl_checksum_type_get_length(AxlChecksumType type)
{
    switch (type) {
    case AXL_CHECKSUM_MD5:    return 16;
    case AXL_CHECKSUM_SHA1:   return 20;
    case AXL_CHECKSUM_SHA256: return 32;
    default:                  return 0;
    }
}

AxlChecksum *
axl_checksum_new(AxlChecksumType type)
{
    AxlChecksum *cs;

    if (axl_checksum_type_get_length(type) == 0) {
        axl_debug("invalid checksum type: %d", (int)type);
        return NULL;
    }

    cs = axl_calloc(1, sizeof(*cs));
    if (cs == NULL) {
        axl_debug(
          "axl_checksum_new: OOM allocating AxlChecksum (%zu bytes)",
          sizeof(*cs)
          );
        return NULL;
    }

    cs->type = type;
    axl_checksum_reset(cs);

    return cs;
}

void
axl_checksum_update(AxlChecksum *cs, const void *data, size_t len)
{
    /* Yield every 64 KiB so multi-MB digest updates stay Ctrl-C
       responsive. Each transform call is microseconds; at 64 KiB
       boundaries we've run ~1024 block transforms — a natural
       outer-loop point. Smaller inputs skip the chunking overhead
       and take the fast single-call path. */
    const size_t YIELD_CHUNK = 64 * 1024;

    if (cs == NULL || cs->closed || data == NULL || len == 0) {
        return;
    }

    const uint8_t *p       = (const uint8_t *)data;
    size_t         remain  = len;

    while (remain > 0) {
        size_t chunk = remain < YIELD_CHUNK ? remain : YIELD_CHUNK;
        switch (cs->type) {
        case AXL_CHECKSUM_MD5:
            md5_update(&cs->md5, p, chunk);
            break;
        case AXL_CHECKSUM_SHA1:
            sha1_update(&cs->sha1, p, chunk);
            break;
        case AXL_CHECKSUM_SHA256:
            sha256_update(&cs->sha256, p, chunk);
            break;
        }
        p      += chunk;
        remain -= chunk;
        if (remain > 0) {
            _axl_poll_break();
        }
    }
}

static void
checksum_finalize(AxlChecksum *cs)
{
    if (cs->closed) {
        return;
    }

    cs->closed = true;

    switch (cs->type) {
    case AXL_CHECKSUM_MD5:
        md5_final(&cs->md5, cs->digest);
        break;
    case AXL_CHECKSUM_SHA1:
        sha1_final(&cs->sha1, cs->digest);
        break;
    case AXL_CHECKSUM_SHA256:
        sha256_final(&cs->sha256, cs->digest);
        break;
    }

    /* Build hex string */
    static const char hex_chars[] = "0123456789abcdef";
    size_t dlen = axl_checksum_type_get_length(cs->type);
    for (size_t i = 0; i < dlen; i++) {
        cs->hex[i * 2]     = hex_chars[cs->digest[i] >> 4];
        cs->hex[i * 2 + 1] = hex_chars[cs->digest[i] & 0x0F];
    }
    cs->hex[dlen * 2] = '\0';
}

const char *
axl_checksum_get_string(AxlChecksum *cs)
{
    if (cs == NULL) {
        return NULL;
    }

    checksum_finalize(cs);
    return cs->hex;
}

void
axl_checksum_get_digest(AxlChecksum *cs, uint8_t *buf, size_t *len)
{
    if (cs == NULL || buf == NULL || len == NULL) {
        return;
    }

    checksum_finalize(cs);

    size_t dlen = axl_checksum_type_get_length(cs->type);
    if (*len < dlen) {
        dlen = *len;
    }
    axl_memcpy(buf, cs->digest, dlen);
    *len = axl_checksum_type_get_length(cs->type);
}

void
axl_checksum_reset(AxlChecksum *cs)
{
    if (cs == NULL) {
        return;
    }

    cs->closed = false;

    switch (cs->type) {
    case AXL_CHECKSUM_MD5:
        md5_init(&cs->md5);
        break;
    case AXL_CHECKSUM_SHA1:
        sha1_init(&cs->sha1);
        break;
    case AXL_CHECKSUM_SHA256:
        sha256_init(&cs->sha256);
        break;
    }
}

void
axl_checksum_free(AxlChecksum *cs)
{
    axl_free(cs);
}

// ===================================================================
// One-shot convenience functions
// ===================================================================

char *
axl_compute_checksum(AxlChecksumType type, const void *data, size_t len)
{
    AxlChecksum *cs = axl_checksum_new(type);
    if (cs == NULL) {
        return NULL;
    }

    axl_checksum_update(cs, data, len);
    const char *hex = axl_checksum_get_string(cs);

    char *result = axl_strdup(hex);
    axl_checksum_free(cs);

    return result;
}

int
axl_compute_checksum_digest(AxlChecksumType type, const void *data,
                            size_t len, uint8_t *out, size_t out_len)
{
    size_t dlen = axl_checksum_type_get_length(type);

    if (out == NULL || dlen == 0 || out_len < dlen) {
        return AXL_ERR;
    }

    AxlChecksum *cs = axl_checksum_new(type);
    if (cs == NULL) {
        return AXL_ERR;
    }

    axl_checksum_update(cs, data, len);
    axl_checksum_get_digest(cs, out, &out_len);
    axl_checksum_free(cs);

    return AXL_OK;
}
