/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-core-internal.h
    HTTP/1.1 message-framing helpers shared by the server and both clients
    (axl-http-core.c). Plain C types only, so a unit test can include it.
**/

#ifndef AXL_HTTP_CORE_INTERNAL_H
#define AXL_HTTP_CORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <axl/axl-hash-table.h>

/** A Transfer-Encoding value read as its comma list (RFC 9112 6.1). */
typedef struct {
    size_t chunked;       ///< members that are "chunked"
    size_t other;         ///< members that are any other coding
    bool   last_chunked;  ///< the final member is "chunked"
    bool   empty;         ///< an empty member, or an empty value
} AxlHttpCodings;

/** How a response's body is delimited (RFC 9112 6.3). */
typedef enum {
    AXL_HTTP_BODY_NONE,      ///< no body: HEAD, 1xx, 204, 304, a 2xx to CONNECT
    AXL_HTTP_BODY_LENGTH,    ///< Content-Length bytes
    AXL_HTTP_BODY_CHUNKED,   ///< chunked, applied once, last
    AXL_HTTP_BODY_TO_CLOSE,  ///< until the close: a TE not ending in chunked, or neither field
} AxlHttpBodyKind;

/** A response's framing, from its headers. */
typedef struct {
    AxlHttpBodyKind kind;
    size_t          length;    ///< the Content-Length (AXL_HTTP_BODY_LENGTH)
    bool            reusable;  ///< the connection may carry another request
} AxlHttpFraming;

/**
 * @brief Read a Transfer-Encoding value's comma list.
 */
void
_axl_http_codings_scan(
    const char     *te,   ///< the field value
    AxlHttpCodings *out   ///< [out] what the list holds
);

/**
 * @brief A response's framing, as a user agent reads it (RFC 9112 6.3).
 *
 * A response to HEAD, a 1xx, 204 or 304, and a 2xx to CONNECT have no
 * body, whatever their fields say (rule 1). Otherwise a Transfer-Encoding
 * wins over Content-Length. Its final coding chunked (once, no empty
 * member): the body is chunked; anything else: the body runs until the
 * connection closes. With a Content-Length beside it, the connection is
 * not reused -- the sender's framing is in doubt. With no
 * Transfer-Encoding, Content-Length must read strictly
 * (axl_http_parse_content_length()). With neither, the body runs until
 * the connection closes (rule 8), and the connection is not reused.
 *
 * @return AXL_OK with @p out set; AXL_ERR for a Content-Length (and no
 *     Transfer-Encoding) that is not a usable length.
 */
int
_axl_http_response_framing(
    AxlHashTable   *headers,  ///< the response's (axl_http_parse_response_headers)
    const char     *method,   ///< the request's method
    size_t          status,   ///< the response's status code
    AxlHttpFraming *out       ///< [out] the framing
);

/**
 * @brief Parse a response head -- the status line and the header block
 *     after it -- as a user agent reads it.
 *
 * The status line ends at LF, a CR before it being part of the end;
 * the headers are read with axl_http_parse_response_headers().
 *
 * @return AXL_OK with both outputs set; AXL_ERR for a malformed head (no
 *     table is returned); AXL_NO_RESOURCES when an allocation fails.
 */
int
_axl_http_response_head_parse(
    const char    *buf,          ///< the head, from the status line
    size_t         head_len,     ///< from axl_http_find_response_header_end()
    size_t        *status_code,  ///< [out] the status code
    AxlHashTable **headers       ///< [out] the headers (caller frees)
);

/**
 * @brief axl_http_parse_request_line(), also returning the version.
 *
 * The line must be exactly `method SP request-target SP HTTP-version`,
 * the version `HTTP/` DIGIT `.` DIGIT (RFC 9112 2.3, 3); any major number
 * parses -- whether the server speaks it is the caller's call (505).
 *
 * @return AXL_OK with every output set; AXL_ERR for a malformed line,
 *     AXL_NO_RESOURCES for a failed allocation -- every output NULL / 0.
 */
int
_axl_http_parse_request_line_version(
    const char *line,      ///< the request line
    size_t      line_len,  ///< its length, without the CRLF
    char      **method,    ///< [out] method (caller frees)
    char      **path,      ///< [out] path (caller frees)
    char      **query,     ///< [out] query, NULL if none (caller frees)
    unsigned   *major,     ///< [out] the version's major number
    unsigned   *minor      ///< [out] the version's minor number
);

/** A request head, as the server parsed it. */
typedef struct {
    char         *method;   ///< from the request line
    char         *path;     ///< from the request line
    char         *query;    ///< from the request line (NULL if none)
    AxlHashTable *headers;  ///< the header block (axl_http_parse_headers)
    bool          http10;   ///< the version is HTTP/1.0
} AxlHttpReqHead;

/**
 * @brief Parse a request head -- the request line and the header block
 *     after it -- as the server reads one, and judge it.
 *
 * Every output the call set is the caller's to free, a refused head's
 * too.
 *
 * @return 0 for a head the server serves; else the status to answer with:
 *     400 for a malformed request line or header block, or an HTTP/1.1
 *     request with no Host (RFC 9112 3.2); 505 for a major version
 *     other than 1 (RFC 9110 15.6.6); 500 when an allocation fails --
 *     the server's failure, not the request's.
 */
size_t
_axl_http_request_head_parse(
    const char     *buf,       ///< the head, from the request line
    size_t          head_len,  ///< from axl_http_find_header_end()
    AxlHttpReqHead *out        ///< [out] what was parsed
);

/** What a request's Expect field asks of the server (RFC 9110 10.1.1). */
typedef enum {
    AXL_HTTP_EXPECT_NONE = 0,   ///< nothing: no field, an empty one, or HTTP/1.0's 100-continue
    AXL_HTTP_EXPECT_CONTINUE,   ///< 100-continue: send 100 before reading the body
    AXL_HTTP_EXPECT_FAILED      ///< an expectation the server cannot meet: 417
} AxlHttpExpect;

/**
 * @brief Classify an Expect field value.
 *
 * The only expectation HTTP defines is `100-continue`, matched
 * case-insensitively once the field's surrounding whitespace is trimmed.
 * Any other value -- a list holding it included -- is one the server
 * cannot meet. An HTTP/1.0 request's 100-continue is ignored, as the RFC
 * requires (a 1.0 client may not understand a 1xx).
 *
 * @return the expectation, AXL_HTTP_EXPECT_NONE for a NULL or empty value.
 */
AxlHttpExpect
_axl_http_expect_check(
    const char *value,   ///< the Expect field value, or NULL for none
    bool        http10   ///< the request is HTTP/1.0
);

#endif /* AXL_HTTP_CORE_INTERNAL_H */
