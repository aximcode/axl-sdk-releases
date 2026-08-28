/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest-sha512.c
    SHA-512 (FIPS 180-4) for the AXL digest subsystem.

    Unlike its MD5 / SHA-1 / SHA-256 siblings this file computes
    nothing: it forwards to mbedTLS, which compiles into every build
    since the AXL_TLS flag was removed. See
    docs/superpowers/specs/2026-08-21-ed25519-design.md §3 for why
    wrapping beats a fourth hand-written compressor -- briefly, the
    two cost the same bytes and only one of them adds cryptographic
    code that a security review has to audit.

    The mbedtls_sha512_* calls each return int. This file discards
    those returns because the AxlChecksum API it implements has no
    error channel on update: axl_checksum_update() returns void. That
    is sound only while the underlying compressor cannot fail, which
    is true for MBEDTLS_SHA512_C with no *_ALT hook. E4 of the spec
    introduces MBEDTLS_SHA512_PROCESS_ALT; if that path can fail, the
    error must be plumbed through AxlChecksum rather than dropped
    here.
**/

#include "axl-digest-internal.h"

void
sha512_init(Sha512State *s)
{
    mbedtls_sha512_init(&s->ctx);
    mbedtls_sha512_starts(&s->ctx, 0);  /* 0 = SHA-512, 1 would be SHA-384 */
}

void
sha512_update(Sha512State *s, const uint8_t *data, size_t len)
{
    mbedtls_sha512_update(&s->ctx, data, len);
}

void
sha512_final(Sha512State *s, uint8_t out[64])
{
    mbedtls_sha512_finish(&s->ctx, out);
    /* Scrubs the context ON THIS PATH -- not a lifetime guarantee.
       axl_checksum_free() is a bare axl_free(); a context freed
       without ever being finalized never reaches this call, so its
       state is never scrubbed. Same as the three sibling algorithms,
       not a regression here. The caller may re-arm a finalized
       context with sha512_init() via axl_checksum_reset(), which
       mbedtls_sha512_init() supports on a freed context. */
    mbedtls_sha512_free(&s->ctx);
}
