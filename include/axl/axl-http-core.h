/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-core.h
 *
 * Low-level HTTP/1.1 parsing helpers shared by axl-http-server,
 * axl-http-client, and consumer code (proxies, middleware, custom
 * transports). All functions operate on raw byte buffers and use
 * AxlHashTable for header storage.
 *
 * Higher-level header utilities like axl_http_parse_range live in
 * axl-http-server.h.
 */

#ifndef AXL_HTTP_CORE_H
#define AXL_HTTP_CORE_H

#include <stddef.h>
#include <axl/axl-macros.h>
#include <axl/axl-hash-table.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find the end of HTTP headers (CRLF CRLF) in a buffer.
 *
 * Scans @p buf for the "\r\n\r\n" sequence that terminates the
 * request/response header block.
 *
 * @return offset of the byte just past the "\r\n\r\n", or 0 if not
 *         found (also 0 if @p len is less than 4).
 */
size_t
axl_http_find_header_end(
    const char *buf,   ///< raw data buffer
    size_t      len    ///< buffer length in bytes
);

/**
 * @brief Parse an HTTP request line: "METHOD PATH?QUERY HTTP/1.x".
 *
 * The line must be exactly three parts separated by single spaces --
 * `method SP request-target SP HTTP-version` (RFC 9112 §3) -- none
 * empty, with no control character, and the version spelled `HTTP/`
 * DIGIT `.` DIGIT, case-sensitive. Anything else is malformed: a fourth
 * part, a missing version, a target that only ends like one
 * (`GET /HTTP/1.0`), `http/1.1`. Any major version parses; whether it is
 * one to serve is the caller's call (the server answers 505 for one
 * other than 1).
 *
 * On success, allocates and returns the method, path, and (optional)
 * query string. The caller frees each non-NULL output with axl_free.
 * On error, all outputs are set to NULL.
 *
 * @return AXL_OK on success; AXL_ERR if the line is malformed;
 *     AXL_NO_RESOURCES if an allocation fails (outputs NULL either way).
 */
int
axl_http_parse_request_line(
    const char *line,      ///< start of the request line
    size_t      line_len,  ///< length up to (but not including) CRLF
    char      **method,    ///< receives method string (allocated; caller frees)
    char      **path,      ///< receives path string (allocated; caller frees)
    char      **query      ///< receives query string (allocated; NULL if none)
);

/**
 * @brief Parse an HTTP status line: "HTTP/1.x NNN Reason".
 *
 * The line holds no control character but HTAB (RFC 9112 §4): a bare CR,
 * NUL, another control or DEL inside it makes it malformed.
 *
 * @return AXL_OK on success, AXL_ERR if the line is malformed.
 */
int
axl_http_parse_status_line(
    const char *line,         ///< start of the status line
    size_t      line_len,     ///< length up to (but not including) CRLF
    size_t     *status_code   ///< receives the numeric status code (e.g. 200)
);

/**
 * @brief Parse a REQUEST's header block into a hash table, as a server
 *     reads one.
 *
 * Reads header lines starting at @p data until the empty line that
 * separates headers from the body. Header names are stored in lowercase
 * to enable case-insensitive lookups. The WHOLE block is parsed or none
 * of it: a line ends at CRLF and nowhere else, and the block fails
 * (AXL_ERR, no table) on a bare CR or LF, NUL or another control
 * character (HTAB aside) in a value, a name that is not an RFC 9110 token,
 * a line with no colon or whitespace before it (obs-fold included), a
 * second Content-Length, Transfer-Encoding or Host line, or no empty line
 * to end it -- it never stops partway with the headers behind the stop
 * unread. A value is stored without the optional whitespace (spaces and
 * tabs) before and after it, RFC 9110 §5.5; whitespace inside it is kept.
 * The returned table owns its keys and values; the caller frees it with
 * axl_hash_table_free.
 *
 * These are a server's rules (RFC 9112 §5.1, §5.2, §6.3). A client
 * reading a RESPONSE uses axl_http_parse_response_headers(), whose rules
 * are a user agent's.
 *
 * @return AXL_OK on success; AXL_ERR for a block these rules refuse;
 *     AXL_NO_RESOURCES if an allocation fails. The table is left NULL
 *     on either failure.
 */
int
axl_http_parse_headers(
    const char    *data,      ///< start of header block (after the request line + CRLF)
    size_t         data_len,  ///< length of data
    AxlHashTable **headers    ///< receives populated hash table (caller frees)
);

/**
 * @brief Find the end of an HTTP RESPONSE head, a bare LF counting as a
 *     line end.
 *
 * axl_http_find_header_end() as a user agent may read it (RFC 9112 §2.2):
 * the head ends at the first empty line, and a line ends at LF whether or
 * not a CR precedes it -- so "\r\n\r\n", "\n\n" and "\n\r\n" all end
 * it. A server's request head still ends only at CRLF CRLF.
 *
 * @return offset of the byte just past the empty line, or 0 if there is
 *         none in the first @p len bytes.
 */
size_t
axl_http_find_response_header_end(
    const char *buf,   ///< raw data buffer (the status line first)
    size_t      len    ///< buffer length in bytes
);

/**
 * @brief Parse a RESPONSE's header block into a hash table, as a user
 *     agent reads one.
 *
 * The same tokenizer as axl_http_parse_headers(), with a user agent's
 * rules where RFC 9112 gives one a different duty from a server's:
 *
 * - a line ends at LF, with or without a CR before it (§2.2);
 * - an obs-fold -- a line starting with SP or HTAB -- continues the value
 *   above it, and the fold is replaced with one SP (§5.2: a user agent
 *   MUST); a fold with no field above it fails the block;
 * - whitespace between a name and its colon is trimmed (§5.1), and a name
 *   is any bytes before the first colon, stored lowercased;
 * - a control character in a value other than CR, LF and NUL is kept
 *   (RFC 9110 §5.5 lets a recipient retain them);
 * - Content-Length lines, and the members of a Content-Length list
 *   (`5, 5`), merge into the one value when they are identical, and
 *   fail the block when any differs (RFC 9110 §8.6, RFC 9112 §6.3);
 * - a second Host line is not checked: it has no meaning in a response.
 *
 * Shared with the server's rules, and refused (AXL_ERR, no table): a bare
 * CR or a NUL anywhere in a line, a line with no colon, a second
 * Transfer-Encoding line, and a block with no empty line to end it.
 * Content-Length and Transfer-Encoding keep their own strict readers
 * (axl_http_parse_content_length()), so no leniency here reaches
 * framing: a folded `5` + `6` is the value `5 6`, which that reader
 * refuses.
 *
 * @return AXL_OK on success; AXL_ERR for a block these rules refuse;
 *     AXL_NO_RESOURCES if an allocation fails. The table is left NULL
 *     on either failure.
 */
int
axl_http_parse_response_headers(
    const char    *data,      ///< start of header block (after the status line's end)
    size_t         data_len,  ///< length of data
    AxlHashTable **headers    ///< receives populated hash table (caller frees)
);

/**
 * @brief Read the Content-Length value from a parsed header table,
 *     strictly.
 *
 * Looks up the lowercase "content-length" key. Present, the value must
 * be one or more decimal digits (the table holds it without its
 * surrounding whitespace) whose number is below SIZE_MAX: anything else
 * -- empty, a sign, any other byte, or a number that would reach SIZE_MAX
 * or wrap -- is refused, never read as a prefix or wrapped. The server
 * answers such a request 400 with this function.
 *
 * @return AXL_OK with @p out set (0 when the header is absent or
 *     @p headers is NULL); AXL_INVALID for a value that is not a usable
 *     length, or a NULL @p out (@p out is 0 then).
 */
int
axl_http_parse_content_length(
    AxlHashTable *headers,  ///< parsed headers (from axl_http_parse_headers)
    size_t       *out       ///< [out] the length
);

/**
 * @brief Read the Content-Length value from a parsed header table.
 *
 * axl_http_parse_content_length() with its refusal folded into the value:
 * a value that is not a usable length -- not all digits, or not below
 * SIZE_MAX (2^64-1 and 2^64+1 alike) -- is SIZE_MAX, which no caller can
 * mistake for a length it can allocate; it is never a wrapped number or
 * the digits before a stray byte (it once was: 2^64+1 read as 1, "42abc"
 * as 42). Use axl_http_parse_content_length() to tell the cases apart,
 * and check for SIZE_MAX before any arithmetic on the result: `len + 1`
 * on it wraps to 0.
 *
 * @return the Content-Length; 0 if the header is absent or @p headers is
 *         NULL; SIZE_MAX for a value that is not a usable length.
 */
size_t
axl_http_get_content_length(
    AxlHashTable *headers  ///< parsed headers (from axl_http_parse_headers)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_CORE_H */
