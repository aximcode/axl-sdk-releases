/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-http-server.h:
 *
 * HTTP server with routing, middleware pipeline, WebSocket,
 * authentication, response caching, and upload streaming.
 */

#ifndef AXL_HTTP_SERVER_H
#define AXL_HTTP_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-hash-table.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

// ---------------------------------------------------------------------------
// HTTP Request / Response
// ---------------------------------------------------------------------------

typedef struct {
    const char  *method;
    const char  *path;
    const char  *query;
    AxlHashTable     *headers;
    const void  *body;
    size_t      body_size;
    char        client_addr[46];
    void        *middleware_data;
} AxlHttpRequest;

typedef struct {
    size_t      status_code;
    AxlHashTable     *headers;
    void        *body;
    size_t      body_size;
    const char  *content_type;
} AxlHttpResponse;

// ---------------------------------------------------------------------------
// Handler and Middleware types
// ---------------------------------------------------------------------------

/**
 * @brief Route handler callback.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
typedef int (*AxlHttpHandler)(
    AxlHttpRequest  *req,   ///< incoming request
    AxlHttpResponse *resp,  ///< response to fill in
    void            *data   ///< opaque caller data
);

/**
 * @brief Middleware callback. Return 0 to continue pipeline, -1 to
 * short-circuit.
 *
 * @return AXL_OK to continue, AXL_ERR to abort.
 */
typedef int (*AxlHttpMiddleware)(
    AxlHttpRequest  *req,   ///< incoming request
    AxlHttpResponse *resp,  ///< response to fill in
    void            *data   ///< opaque caller data
);

// ---------------------------------------------------------------------------
// HTTP Server
// ---------------------------------------------------------------------------

typedef struct AxlHttpServer AxlHttpServer;

/**
 * @brief Create a new HTTP server bound to the given port.
 *
 * @return server instance, or NULL on failure.
 */
AxlHttpServer *
axl_http_server_new(
    uint16_t port  ///< TCP port to listen on
);

/**
 * @brief Free an HTTP server and all resources.
 */
void
axl_http_server_free(
    AxlHttpServer *server  ///< server to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlHttpServer, axl_http_server_free)
#endif

/**
 * @brief Set a server option by key.
 *
 * Supported keys: "max.connections", "body.limit", "keep.alive.sec".
 *
 * @return AXL_OK on success, AXL_ERR on unknown key or invalid value.
 */
int
axl_http_server_set(
    AxlHttpServer *s,      ///< server
    const char    *key,    ///< option key
    const char    *value   ///< option value (string)
);

/**
 * @brief Get a server option value as string.
 *
 * @return option value, or NULL for unknown keys.
 */
const char *
axl_http_server_get(
    AxlHttpServer *s,    ///< server
    const char    *key   ///< option key
);

/**
 * @brief Set maximum simultaneous connections.
 *
 * @return AXL_OK on success, AXL_ERR if @p s is NULL or the underlying setter
 *     rejected the value.
 */
int
axl_http_server_set_max_connections(
    AxlHttpServer *s,    ///< server
    size_t        max    ///< maximum simultaneous connections (default 8)
);

/**
 * @brief Set maximum request body size.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_http_server_set_body_limit(
    AxlHttpServer *s,          ///< server
    size_t        max_bytes    ///< maximum request body size in bytes (default 4 MB)
);

/**
 * @brief Set keep-alive timeout.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_http_server_set_keep_alive(
    AxlHttpServer *s,             ///< server
    size_t        timeout_sec     ///< keep-alive timeout in seconds (default 30)
);

/**
 * @brief Register middleware executed in registration order.
 * Return 0 from mw to continue pipeline, -1 to short-circuit.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_use(
    AxlHttpServer     *s,     ///< server
    AxlHttpMiddleware mw,     ///< middleware function
    void              *data   ///< context passed to mw
);

/**
 * @brief Register a route handler.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_route(
    AxlHttpServer  *s,        ///< server
    const char     *method,   ///< HTTP method ("GET", "POST", etc.) or NULL for any
    const char     *path,     ///< path pattern; trailing slash-star matches prefix
    AxlHttpHandler handler,   ///< handler function
    void           *data      ///< context passed to handler
);

/**
 * @brief Serve static files from a filesystem path.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_static(
    AxlHttpServer *s,        ///< server
    const char    *prefix,   ///< URL prefix (e.g. "/")
    const char    *fs_path   ///< filesystem path (UTF-8)
);

/**
 * @brief Attach server to an event loop for cooperative polling.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_attach(
    AxlHttpServer *s,     ///< server
    AxlLoop       *loop   ///< event loop from axl_loop_new
);

/**
 * @brief Run the server in standalone mode (blocks until quit).
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_run(
    AxlHttpServer *s  ///< server
);

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

/**
 * @brief Set JSON body and "application/json" content type (default 200).
 */
void
axl_http_response_set_json(
    AxlHttpResponse *r,     ///< response
    const char      *json   ///< JSON string
);

/**
 * @brief Set plain text body and "text/plain" content type (default 200).
 */
void
axl_http_response_set_text(
    AxlHttpResponse *r,     ///< response
    const char      *text   ///< plain text string
);

/**
 * @brief Set or override the HTTP status code.
 */
void
axl_http_response_set_status(
    AxlHttpResponse *r,     ///< response
    size_t          code    ///< HTTP status code
);

/**
 * @brief Set response body from a file, inferring content type from
 * extension.
 */
void
axl_http_response_set_file(
    AxlHttpResponse *r,     ///< response
    const char      *path   ///< filesystem path (UTF-8)
);

/**
 * @brief Set a byte-range response (HTTP 206) with Content-Range header.
 */
void
axl_http_response_set_range(
    AxlHttpResponse *r,           ///< response
    const void      *data,        ///< full data buffer
    size_t          offset,       ///< byte offset into data
    size_t          length,       ///< number of bytes to send
    size_t          total_size    ///< total size of the resource
);

// ---------------------------------------------------------------------------
// Range request parsing (RFC 7233)
// ---------------------------------------------------------------------------

/**
 * @brief Parsed byte range from an HTTP Range request header.
 */
typedef struct {
    uint64_t start;   ///< first byte position (inclusive)
    uint64_t end;     ///< last byte position (inclusive)
    uint64_t total;   ///< total file size (from file_size parameter)
    bool     valid;   ///< true if parsing succeeded
} AxlHttpRange;

/**
 * @brief Parse an HTTP Range request header.
 *
 * Supports a single "bytes=START-END" range (not multi-range).
 * Handles "bytes=START-", "bytes=-SUFFIX", and "bytes=START-END".
 * Clamps end to file_size - 1. Sets out->valid on success.
 *
 * @return true if a valid range was parsed, false otherwise.
 */
bool
axl_http_parse_range(
    const char   *range_header,  ///< Range header value (e.g. "bytes=0-499")
    uint64_t      file_size,     ///< total file size
    AxlHttpRange *out            ///< receives the parsed range
);

// ---------------------------------------------------------------------------
// Content negotiation
// ---------------------------------------------------------------------------

/**
 * @brief Check if an HTTP Accept header includes a media type.
 *
 * Searches the comma-separated Accept header value for @p media_type
 * (e.g. "application/json", "text/html"). Matching is case-insensitive
 * and ignores quality parameters. Also matches wildcard types
 * (wildcard accepts everything).
 *
 * @return true if @p media_type is acceptable.
 */
bool
axl_http_accepts(
    const char *accept_header,  ///< Accept header value (may be NULL)
    const char *media_type      ///< media type to check (e.g. "application/json")
);

// ---------------------------------------------------------------------------
// TLS / HTTPS
// ---------------------------------------------------------------------------

/**
 * @brief Enable TLS on the server with DER-encoded cert and key.
 *
 * After this call, all accepted connections use TLS. The cert and
 * key can be generated with axl_tls_generate_self_signed().
 * Requires AXL_TLS=1 at build time.
 *
 * @return AXL_OK on success, AXL_ERR if TLS not available or cert/key invalid.
 */
int
axl_http_server_use_tls(
    AxlHttpServer *s,          ///< server
    const void    *cert_der,   ///< DER-encoded certificate
    size_t         cert_len,   ///< certificate length
    const void    *key_der,    ///< DER-encoded private key
    size_t         key_len     ///< key length
);

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455)
// ---------------------------------------------------------------------------

#define AXL_WS_CONNECT     0
#define AXL_WS_TEXT        1
#define AXL_WS_BINARY      2
#define AXL_WS_DISCONNECT  3

/**
 * @brief WebSocket event callback.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
typedef int (*AxlWsHandler)(
    size_t     event,       ///< one of AXL_WS_CONNECT, AXL_WS_TEXT, AXL_WS_BINARY, AXL_WS_DISCONNECT
    const void *frame,      ///< frame data (NULL for CONNECT/DISCONNECT)
    size_t     frame_size,  ///< frame data size
    void       *data        ///< opaque caller data
);

/**
 * @brief Register a WebSocket endpoint.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_websocket(
    AxlHttpServer *s,        ///< server
    const char    *path,     ///< WebSocket endpoint path
    AxlWsHandler  handler,   ///< WebSocket event handler
    void          *data      ///< opaque caller data
);

/**
 * @brief Broadcast data to all connected WebSocket clients on a path.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_ws_broadcast(
    AxlHttpServer *s,     ///< server
    const char    *path,  ///< WebSocket endpoint path
    const void    *data,  ///< data to broadcast
    size_t        size    ///< data size in bytes
);

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

typedef struct {
    const char  *username;
    size_t      role;
} AxlAuthInfo;

/**
 * @brief Authentication callback.
 *
 * @return AXL_OK on success (authenticated), AXL_ERR on failure.
 */
typedef int (*AxlAuthCallback)(
    AxlHttpRequest *req,       ///< incoming request
    AxlAuthInfo    *auth_out,  ///< receives authentication info on success
    void           *data       ///< opaque caller data
);

#define AXL_ROUTE_NO_AUTH  0x00
#define AXL_ROUTE_AUTH     0x01
#define AXL_ROUTE_ADMIN    0x02

/**
 * @brief Register an authentication handler for the server.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_use_auth(
    AxlHttpServer   *s,    ///< server
    AxlAuthCallback cb,    ///< authentication callback
    void            *data  ///< opaque caller data
);

/**
 * @brief Register a route handler with authentication requirements.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_route_auth(
    AxlHttpServer  *s,           ///< server
    const char     *method,      ///< HTTP method or NULL for any
    const char     *path,        ///< path pattern
    AxlHttpHandler handler,      ///< handler function
    void           *data,        ///< context passed to handler
    uint32_t       auth_flags    ///< AXL_ROUTE_* flags
);

// ---------------------------------------------------------------------------
// Response Caching
// ---------------------------------------------------------------------------

#define AXL_CACHE_FOREVER  ((size_t)-1)

/**
 * @brief Enable response caching on the server.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_use_cache(
    AxlHttpServer *s,            ///< server
    size_t        max_entries    ///< maximum cache entries
);

/**
 * @brief Set cache TTL for a specific route path.
 *
 * Stores a `path → ttl_ms` mapping; the next cached response whose
 * request path equals `path` exactly uses this TTL instead of the
 * server-wide default from `axl_http_server_use_cache`. Prefix
 * routes (e.g. `/css/` followed by a wildcard) are not matched —
 * set the TTL on the exact sub-paths you expect, or rely on the
 * server default.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_set_route_ttl(
    AxlHttpServer *s,       ///< server
    const char    *path,    ///< exact request path
    size_t        ttl_ms    ///< time-to-live in milliseconds, or AXL_CACHE_FOREVER
);

/**
 * @brief Invalidate cached responses whose path starts with @p prefix.
 *
 * Walks the cache and removes every entry whose path portion begins
 * with @p prefix (the leading "METHOD " token in the internal cache
 * key is skipped). Pass NULL or "" to clear the whole cache.
 */
void
axl_http_server_cache_invalidate(
    AxlHttpServer *s,       ///< server
    const char    *prefix   ///< path prefix to invalidate (NULL or "" for all)
);

// ---------------------------------------------------------------------------
// Upload Streaming
// ---------------------------------------------------------------------------

/**
 * @brief Upload streaming callback, called per chunk as body data arrives.
 *
 * Called repeatedly with chunks up to the configured upload.chunk.size.
 * The final call has chunk=NULL, chunk_size=0 — set resp fields there.
 * Return AXL_ERR from any call to abort the upload (sends 500).
 *
 * @return AXL_OK on success, AXL_ERR to abort.
 */
typedef int (*AxlUploadHandler)(
    AxlHttpRequest  *req,         ///< incoming request
    AxlHttpResponse *resp,        ///< response (set on final call)
    const void      *chunk,       ///< chunk data (NULL on final call)
    size_t           chunk_size,  ///< chunk size (0 on final call)
    void            *data         ///< opaque caller data
);

/**
 * @brief Register a streaming upload route.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_upload_route(
    AxlHttpServer   *s,        ///< server
    const char      *method,   ///< HTTP method
    const char      *path,     ///< path pattern
    AxlUploadHandler handler,  ///< upload handler
    void            *data      ///< opaque caller data
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_SERVER_H */
