/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-url-internal.h
    The authority of a URL as a request names it, shared by axl_url_build
    and the HTTP client's Host header. Plain C types only.
**/

#ifndef AXL_URL_INTERNAL_H
#define AXL_URL_INTERNAL_H

#include <stdint.h>

/** A URL's authority in pieces: `open host close port`. */
typedef struct {
    const char *open;      ///< "[" for an IPv6 literal, else ""
    const char *close;     ///< "]" for an IPv6 literal, else ""
    char        port[7];   ///< ":<port>" when not the scheme's default, else ""
} AxlUrlAuthority;

/**
 * @brief Split a URL's authority into the pieces a request names it by
 *     (RFC 9110 7.2, RFC 3986 3.2.2).
 *
 * The host goes in brackets when it is an IPv6 literal (it holds a ':');
 * ":port" follows exactly when @p port is not @p scheme's default (80 for
 * http, 443 for https; any port for another scheme, 0 aside).
 */
void
_axl_url_authority(
    const char      *scheme,   ///< the URL's scheme
    const char      *host,     ///< the host, without brackets
    uint16_t         port,     ///< the port (0: none)
    AxlUrlAuthority *out       ///< [out] the pieces
);

#endif /* AXL_URL_INTERNAL_H */
