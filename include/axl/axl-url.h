/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-url.h:
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

typedef struct {
    char      *scheme;
    char      *host;
    char      *path;
    char      *query;
    uint16_t  port;
} AxlUrl;

/**
 * @brief Parse a URL string into components.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_url_parse(
    const char *url,         ///< URL string (e.g. "http://host:8080/path?q=1")
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
