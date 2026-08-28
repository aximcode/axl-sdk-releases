/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-hmac.h
    Keyed-hash message authentication code (HMAC, RFC 2104).

    Mirrors GLib's GHmac, layered on the AxlChecksum digest engine
    (@ref axl-digest.h) — MD5, SHA-1, SHA-256 or SHA-512. Use it to
    authenticate a message with a shared secret: API tokens, signed
    cookies, webhook signatures, Redfish/IPMI session integrity.

    The API matches AxlChecksum: create with a key + algorithm, feed
    data incrementally, then read the result once as a hex string or
    raw bytes. After a get, the context is finalized — do not update
    again.

    One-shot:
    @code
    char *mac = axl_compute_hmac(AXL_CHECKSUM_SHA256,
                                 key, key_len, msg, msg_len);
    axl_printf("HMAC-SHA256: %s\n", mac);
    axl_free(mac);
    @endcode

    Incremental:
    @code
    AXL_AUTOPTR(AxlHmac) h = axl_hmac_new(AXL_CHECKSUM_SHA256, key, key_len);
    axl_hmac_update(h, part1, len1);
    axl_hmac_update(h, part2, len2);
    const char *hex = axl_hmac_get_string(h);
    @endcode

    NOTE: HMAC-MD5 and HMAC-SHA1 are provided for interoperability with
    legacy protocols; prefer HMAC-SHA256 for new designs.
**/

#ifndef AXL_HMAC_H
#define AXL_HMAC_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-digest.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlHmac AxlHmac;

/**
 * @brief Create an HMAC context for @p type keyed with @p key.
 *
 * The key may be any length: keys longer than the hash block size
 * (64 bytes for MD5/SHA-1/SHA-256, 128 for SHA-512) are hashed down
 * per RFC 2104, and a @p key_len of 0 is valid (empty key). @p key
 * may be NULL only when @p key_len is 0.
 *
 * @return a new HMAC context, or NULL on allocation failure, an
 *     unsupported @p type, or @p key == NULL with @p key_len > 0.
 *     Free with axl_hmac_free().
 */
AxlHmac *
axl_hmac_new(
    AxlChecksumType  type,     ///< digest algorithm
    const void      *key,      ///< secret key bytes
    size_t           key_len   ///< key length in bytes
);

/**
 * @brief Feed data into the HMAC.
 *
 * May be called repeatedly. Must NOT be called after
 * axl_hmac_get_string() or axl_hmac_get_digest() — the context is
 * finalized by the first get. No-op if @p h is NULL.
 */
void
axl_hmac_update(
    AxlHmac    *h,    ///< HMAC context
    const void *data, ///< input data
    size_t      len   ///< input length in bytes
);

/**
 * @brief Get the HMAC as a lowercase hex string.
 *
 * Finalizes the context on first call. The returned string is owned
 * by @p h and stays valid until axl_hmac_free(); repeated calls
 * return the same pointer. After this call, axl_hmac_update() must
 * not be used.
 *
 * @return hex string (e.g. "5bdcc146..."), or NULL if @p h is NULL
 *     or the finalize step hit an allocation failure.
 */
const char *
axl_hmac_get_string(
    AxlHmac *h  ///< HMAC context
);

/**
 * @brief Get the raw HMAC digest bytes.
 *
 * Finalizes the context. On entry @p *len is the buffer size; on
 * return it is the number of bytes actually written —
 * `min(buffer size, digest length)`, where the digest is 16/20/32/64
 * bytes for MD5/SHA-1/SHA-256/SHA-512. A buffer shorter than the digest
 * receives a truncated prefix and @p *len reports that shorter
 * count, never the full length. A @p *len of 0 on return means the
 * finalize step hit an allocation failure and no MAC exists — the
 * one other case that yields 0 is a zero-size @p buf, which is
 * degenerate. After this call, axl_hmac_update() must not be used.
 */
void
axl_hmac_get_digest(
    AxlHmac *h,    ///< HMAC context
    uint8_t *buf,  ///< output buffer
    size_t  *len   ///< [in/out] buffer size / bytes written
);

/**
 * @brief Free an HMAC context. NULL-safe.
 */
void
axl_hmac_free(
    AxlHmac *h  ///< HMAC context (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlHmac, axl_hmac_free)
#endif

/**
 * @brief Compute an HMAC in one call and return it as a hex string.
 *
 * Convenience wrapper for new + update + get_string + dup. Matches
 * GLib's g_compute_hmac_for_data().
 *
 * @return newly allocated lowercase hex string (free with axl_free),
 *     or NULL on failure (see axl_hmac_new).
 */
char *
axl_compute_hmac(
    AxlChecksumType  type,      ///< digest algorithm
    const void      *key,       ///< secret key bytes
    size_t           key_len,   ///< key length in bytes
    const void      *data,      ///< message bytes
    size_t           data_len   ///< message length in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HMAC_H */
