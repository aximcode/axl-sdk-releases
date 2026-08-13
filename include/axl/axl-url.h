/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-url.h
 *
 * URL parser and builder.
 */

#ifndef AXL_URL_H
#define AXL_URL_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parsed URL components per RFC 3986 (subset). Heap-allocated by
 * axl_url_parse; freed by axl_url_free. NULL string fields mean the
 * component wasn't present in the input — distinguish from
 * "present but empty" via empty-string check (e.g. `user:@host` →
 * password is `""`, not NULL).
 */
typedef struct {
    char      *scheme;     ///< URL scheme without `://` ("http", "https", ...)
    char      *user;       ///< userinfo user portion before `:` (NULL if no `user[:pass]@` in authority)
    char      *password;   ///< userinfo password portion after `:` (NULL if no `:` in userinfo; "" if `user:@host`)
    char      *host;       ///< hostname or IP literal (no userinfo, no port)
    char      *path;       ///< path component starting with `/` ("/" if absent)
    char      *query;      ///< raw query string after `?` (NULL if no `?`)
    char      *fragment;   ///< fragment after `#` (NULL if no `#`)
    uint16_t   port;       ///< port number (default for scheme if not specified)
} AxlUrl;

/**
 * @brief Parse a URL string into components.
 *
 * Supports the RFC 3986 generic URI shape:
 *   `scheme://[user[:password]@]host[:port][/path][?query][#fragment]`
 *
 * Userinfo is recognized only when an `@` appears in the authority
 * (between `://` and the first `/`, `?`, or `#`); an `@` later in
 * the path or query is left alone. Percent-decoding is NOT applied
 * to any field — the parser returns raw bytes for round-trip
 * fidelity.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_url_parse(
    const char *url,         ///< URL string (e.g. "http://user:pass@host:8080/path?q=1#frag")
    AxlUrl     **out_parsed  ///< receives allocated AxlUrl; free with axl_url_free()
);

/**
 * @brief Free an AxlUrl returned by axl_url_parse.
 */
void
axl_url_free(
    AxlUrl *url  ///< URL to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlUrl, axl_url_free)
#endif

/**
 * @brief Build a URL string from components.
 *
 * @return allocated URL string, or NULL on failure. Caller frees.
 */
char *
axl_url_build(
    const char *scheme,  ///< "http" or "https"
    const char *host,    ///< hostname or IP
    uint16_t   port,     ///< port number (0 = use default for scheme)
    const char *path     ///< path starting with "/" (NULL = "/")
);

// ---------------------------------------------------------------------------
// Percent-encoding (RFC 3986)
// ---------------------------------------------------------------------------

/**
 * @brief Percent-encode a string for use in a URL.
 *
 * Encodes all characters except unreserved characters (A-Z, a-z, 0-9,
 * '-', '.', '_', '~') and optionally '/' (preserved by default for
 * path encoding). Each encoded byte becomes %XX.
 *
 * @return number of bytes written (excluding NUL), or -1 on error
 *     or truncation.
 */
int
axl_url_encode(
    const char *src,   ///< input string (UTF-8)
    char       *out,   ///< output buffer
    size_t      size   ///< output buffer size
);

/**
 * @brief Decode a percent-encoded URL string.
 *
 * Replaces %XX sequences with the corresponding byte. Passes
 * through characters that are not percent-encoded.
 *
 * @return number of bytes written (excluding NUL), or -1 on error
 *     or truncation.
 */
int
axl_url_decode(
    const char *src,   ///< percent-encoded string
    char       *out,   ///< output buffer
    size_t      size   ///< output buffer size
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_URL_H */
