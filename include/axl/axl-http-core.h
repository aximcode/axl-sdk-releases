/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-http-core.h:
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
 * On success, allocates and returns the method, path, and (optional)
 * query string. The caller frees each non-NULL output with axl_free.
 * On error, all outputs are set to NULL.
 *
 * @return AXL_OK on success, AXL_ERR if the line is malformed or allocation fails.
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
 * @return AXL_OK on success, AXL_ERR if the line is malformed.
 */
int
axl_http_parse_status_line(
    const char *line,         ///< start of the status line
    size_t      line_len,     ///< length up to (but not including) CRLF
    size_t     *status_code   ///< receives the numeric status code (e.g. 200)
);

/**
 * @brief Parse HTTP headers from raw bytes into a hash table.
 *
 * Reads header lines starting at @p data until the empty line that
 * separates headers from the body. Header names are stored in lowercase
 * to enable case-insensitive lookups. The returned table owns its
 * keys and values; the caller frees it with axl_hash_table_free.
 *
 * @return AXL_OK on success, AXL_ERR on error.(table is left as NULL).
 */
int
axl_http_parse_headers(
    const char    *data,      ///< start of header block (after request/status line + CRLF)
    size_t         data_len,  ///< length of data
    AxlHashTable **headers    ///< receives populated hash table (caller frees)
);

/**
 * @brief Read the Content-Length value from a parsed header table.
 *
 * Looks up the lowercase "content-length" key. Stops parsing at the
 * first non-digit character.
 *
 * @return Content-Length value, or 0 if the header is absent or
 *         @p headers is NULL.
 */
size_t
axl_http_get_content_length(
    AxlHashTable *headers  ///< parsed headers (from axl_http_parse_headers)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_CORE_H */
