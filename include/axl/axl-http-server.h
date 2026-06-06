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
#include <axl/axl-fs.h>          /* AxlFsEntry — used by webdav callbacks */
#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-bytes.h>

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
    size_t       status_code;
    AxlHashTable *headers;

    /// Response body bytes. **Ownership: the SDK calls axl_free
    /// on this pointer after the response is sent**, unless
    /// body_static is set. Handlers that assign body
    /// directly must therefore pass an axl_malloc'd buffer.
    /// Assigning a `.rodata` / static const literal here is a heap
    /// corruption bug — use axl_http_response_set_static for
    /// embedded read-only assets, or one of the copy-based helpers
    /// (`axl_http_response_set_text` / `_json` / `_file`) which
    /// allocate internally.
    void        *body;
    size_t       body_size;

    /// Content-Type header value. Borrowed pointer — the SDK does
    /// NOT free this. Static string literals are fine; if a caller
    /// allocates dynamically, the caller is responsible for the
    /// lifetime (must outlive the response send).
    const char  *content_type;

    /// When true, the SDK will NOT free body after the response
    /// is sent. Set by axl_http_response_set_static; ignore
    /// otherwise. Default false (zero-init) preserves the
    /// "axl_malloc'd, SDK frees" contract for every existing
    /// caller — no migration required.
    bool         body_static;

    /* --- Streaming response (set by axl_http_response_set_streamer).
     *     When streamer is non-NULL, body / body_size
     *     / body_static are IGNORED — the dispatcher pulls
     *     chunks from streamer instead. Mutually exclusive with
     *     the contiguous-body fields above. */

    /// When non-NULL, the dispatcher streams the body by calling
    /// streamer repeatedly. See axl_http_response_set_streamer.
    /// Set body / body_size / body_static via the setter rather than
    /// touching these fields directly.
    int          (*streamer)(void *ctx, void *out_buf, size_t out_buf_size,
                             size_t *out_size);

    /// Opaque user data passed to streamer on each invocation.
    /// Owned by the caller; lifetime managed via streamer_cleanup.
    void        *streamer_ctx;

    /// Optional finalizer called once the streaming response either
    /// completes (EOF, all bytes sent) OR is aborted (streamer error,
    /// connection reset before EOF). Receives streamer_ctx so
    /// the caller can close files / free buffers. NULL means the
    /// streamer self-cleans via its EOF / error transitions.
    void         (*streamer_cleanup)(void *ctx);

    /// Total response body size in bytes. Used as Content-Length when
    /// known. Pass `(size_t)-1` to signal unknown length — the
    /// dispatcher emits Transfer-Encoding: chunked instead.
    size_t       streamer_total_size;
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
 * @brief Register multiple route handlers in one call.
 *
 * Variadic batch form of axl_http_server_add_route. Each route
 * is a four-arg group `(method, path, handler, data)` repeated until
 * a sentinel @c NULL method terminates the list. Stops on the first
 * registration failure and returns @c AXL_ERR — earlier successfully-
 * registered routes stay installed (the server's route table is
 * append-only and the failure is most likely "table full," which the
 * caller can surface to the user).
 *
 * @code
 * axl_http_server_add_routes(server,
 *     "GET",    "/",       handle_root, NULL,
 *     "GET",    "/x",      handle_x,    o,
 *     "PUT",    "/y",      handle_y,    o,
 *     "DELETE", "/z",      handle_z,    o,
 *     NULL);   // sentinel — required
 * @endcode
 *
 * Replaces the equivalent five separate `axl_http_server_add_route`
 * calls and the per-call error-check chain. Route precedence is
 * the same as for repeated single-route calls — exact path before
 * prefix, method-specific before method-wildcard — independent of
 * the order routes are registered.
 *
 * @return AXL_OK if every route registered; AXL_ERR on the first
 *     failure (with that route and all later groups in the list NOT
 *     registered).
 */
int
axl_http_server_add_routes(
    AxlHttpServer *s,         ///< server
    ...                       ///< (method, path, handler, data) groups, terminated by NULL method
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
 * @brief Bring the server up on a caller-owned event loop.
 *
 * Allocates the per-connection pool sized from the server's
 * `max.connections` config, opens the TCP listener (pinned to
 * `listen.ip` if set, else auto-pick), and registers the async
 * accept on @p loop so each incoming connection re-arms
 * automatically. The server is fully wired and listening when
 * this returns; the caller drives @p loop via `axl_loop_run`
 * (foreground) or `axl_loop_attach_driver` (DXE driver mode).
 *
 * `axl_http_server_run` is the convenience wrapper that creates
 * its own loop, calls this, and runs the loop to completion.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_start(
    AxlHttpServer *s,     ///< server
    AxlLoop       *loop   ///< event loop from axl_loop_new
);

/**
 * @brief Run the server standalone — creates a loop, calls
 *     `axl_http_server_start`, and blocks in `axl_loop_run` until
 *     `axl_loop_quit`.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_run(
    AxlHttpServer *s  ///< server
);

// ---------------------------------------------------------------------------
// Request helpers — content negotiation + JSON body parse
// ---------------------------------------------------------------------------

/**
 * @brief Does the request's @c Accept header advertise interest in @p mime?
 *
 * Routes from @c req->headers["accept"] into axl_http_accepts —
 * the same matcher used elsewhere in the HTTP machinery: case-insensitive,
 * handles multi-type lists, recognizes the catch-all wildcard,
 * and tolerates @c ;q= parameters (matches regardless of q-value, so
 * an explicit @c q=0 reject is still treated as "accepts" — a future
 * tightening if a consumer needs strict negotiation). Missing Accept
 * header returns false.
 *
 * @return true if @p req would accept a response of MIME type @p mime.
 */
bool
axl_http_request_accepts(
    const AxlHttpRequest *req,    ///< incoming request
    const char           *mime    ///< MIME type to look for ("application/json")
);

/**
 * @brief Convenience: does the request want JSON?
 *
 * Equivalent to axl_http_request_accepts (req, "application/json").
 * Common-enough pattern in REST handlers that it earns its own name —
 * `if (axl_http_request_wants_json(req)) { ... }` reads at the right
 * level. Used when a single endpoint serves both HTML and JSON
 * representations and picks based on the client's Accept header.
 */
bool
axl_http_request_wants_json(
    const AxlHttpRequest *req     ///< incoming request
);

/**
 * @brief Parse the request body as JSON.
 *
 * Calls axl_json_parse on @c req->body / @c req->body_size.
 * The reader references the body buffer directly — do not free
 * @c req->body while the reader is in use, and call @c axl_json_free
 * on @p out when done.
 *
 * Strict RFC 8259. For JSON5, parse manually with
 * axl_json_parse_flags.
 *
 * @return true on success (@p out populated and ready for
 *     @c axl_json_object_get / etc); false on NULL inputs, empty
 *     body, or JSON parse error.
 */
bool
axl_http_request_get_json(
    const AxlHttpRequest *req,    ///< incoming request
    AxlJsonReader        *out     ///< [out] reader to fill (caller owns; free with axl_json_free)
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
 * @brief Set response body to a borrowed static buffer that the SDK
 *     must NOT free.
 *
 * Use this for embedded read-only assets — `.rodata` C string
 * literals, static const arrays of HTML / JS / CSS, immutable
 * binary blobs xxd'd into the binary. Sets AxlHttpResponse.body
 * to @a body, body_size to @a size, marks
 * AxlHttpResponse.body_static = true so the dispatch loop skips
 * its post-send `axl_free`. Passing such a pointer to the
 * axl_http_response_set_text / `_json` family would force a copy
 * (waste); assigning it to AxlHttpResponse.body directly causes
 * heap corruption (the dispatch loop would treat the literal as an
 * `axl_malloc`'d buffer and free it).
 *
 * @a content_type is borrowed (typically a string literal). NULL
 * leaves the existing content-type unchanged.
 *
 * If a previous body was set via the copy-based helpers, this
 * function frees it before installing the static buffer.
 */
void
axl_http_response_set_static(
    AxlHttpResponse *r,             ///< response
    const void      *body,          ///< pointer to read-only / static buffer
    size_t           size,          ///< size of @a body in bytes
    const char      *content_type   ///< MIME type (borrowed); NULL = leave as-is
);

/**
 * @brief Set the response body from an AxlBytes.
 *
 * Convenience for handlers that already hold their payload as an
 * AxlBytes — a file read via axl_file_get_bytes, a cached blob,
 * a slice. The bytes are COPIED into the response's owned body (the
 * SDK frees that copy after the response is sent), so the caller may
 * unref @a body immediately. This is not a zero-copy path: the
 * contiguous-body send copies into the transmit buffer regardless, so
 * for large payloads prefer axl_http_response_set_file /
 * axl_http_response_set_streamer.
 *
 * @a content_type is borrowed (typically a string literal); NULL
 * leaves the existing content-type unchanged. An empty @a body sends
 * an empty payload.
 */
void
axl_http_response_set_bytes(
    AxlHttpResponse *r,             ///< response
    const AxlBytes  *body,          ///< payload to copy in
    const char      *content_type   ///< MIME type (borrowed); NULL = leave as-is
);

/**
 * @brief Producer callback for streaming response bodies.
 *
 * Called repeatedly by the dispatcher to fill outgoing chunks.
 * Implementations read the next chunk from their backing source
 * (file, generated content, network passthrough) into @p out_buf
 * and report the byte count via @p out_size. Returning @c AXL_OK with
 * @c *out_size == 0 signals end-of-stream.
 *
 * Re-entrancy: same constraints as @c AxlUploadHandler — runs on the
 * loop's normal dispatch level (TPL_APPLICATION foreground,
 * TPL_CALLBACK driver). Don't block, don't allocate gratuitously,
 * keep each invocation short — the dispatcher is single-threaded
 * and a slow streamer stalls other connections.
 *
 * @param ctx           opaque user data from
 *                      axl_http_response_set_streamer
 * @param out_buf       caller-supplied buffer to fill
 * @param out_buf_size  capacity of @p out_buf in bytes
 * @param out_size      [out] bytes written into @p out_buf this call;
 *                      0 = EOF
 *
 * @return @c AXL_OK to continue (including the EOF case with
 *     @p *out_size == 0); @c AXL_ERR to abort the response (the
 *     dispatcher resets the connection and invokes the cleanup hook).
 */
typedef int (*AxlResponseStreamer)(
    void   *ctx,
    void   *out_buf,
    size_t  out_buf_size,
    size_t *out_size
);

/**
 * @brief Optional finalizer called when a streaming response ends.
 *
 * Fires exactly once for any response that successfully installs a
 * streamer via axl_http_response_set_streamer, regardless of
 * how the response ended (EOF, streamer error, connection reset
 * before EOF). Use it to close files, free buffers, or release any
 * resource @c ctx holds onto. Pass NULL if the streamer manages its
 * own lifecycle.
 *
 * @param ctx the same @c ctx pointer registered with the streamer.
 */
typedef void (*AxlResponseCleanup)(void *ctx);

/**
 * @brief Set a streaming response body via producer callback.
 *
 * Replaces the contiguous-body model for large or unbounded
 * responses. The dispatcher allocates a chunk-sized tx buffer,
 * sends headers, then calls @p streamer repeatedly to fill the
 * buffer and `axl_tcp_send_async`'s each filled chunk (chained
 * completions). EOF (returned via `*out_size = 0`) terminates the
 * response.
 *
 * Sets Content-Length from @p total_size when known (the typical
 * file-serve case). Pass `(size_t)-1` to signal "unknown length"
 * and emit Transfer-Encoding: chunked instead — each chunk goes on
 * the wire framed as `<hex-size>\r\n<data>\r\n`, terminated by a
 * `0\r\n\r\n` final chunk.
 *
 * @p ctx is opaque to the SDK; the dispatcher passes it back
 * unchanged on each streamer invocation. @p cleanup (NULL-able)
 * fires exactly once when the response ends — successful EOF,
 * streamer error, OR connection reset. Use it to close the file
 * `ctx` wraps, free buffers, etc.
 *
 * Mutually exclusive with @c body / @c body_static / axl_http_response_set_text / `_json` / `_file`. Setting a
 * streamer overrides any prior body assignment (and frees a
 * previously-set non-static body).
 *
 * @code
 * static int file_streamer(void *ctx, void *buf, size_t cap, size_t *out)
 * {
 *     AxlFile *f = (AxlFile *)ctx;
 *     return axl_fread(f, buf, cap, out);
 * }
 * static void file_close(void *ctx) { axl_fclose((AxlFile *)ctx); }
 *
 * AxlFile *f = axl_fopen("fs0:/big.iso", "r");
 * uint64_t size = 0;
 * axl_file_size(f, &size);
 * axl_http_response_set_streamer(resp, file_streamer, f, file_close,
 *                                (size_t)size, "application/octet-stream");
 * @endcode
 */
void
axl_http_response_set_streamer(
    AxlHttpResponse     *r,            ///< response
    AxlResponseStreamer  streamer,     ///< producer callback (must be non-NULL)
    void                *ctx,          ///< opaque user data passed back to streamer / cleanup
    AxlResponseCleanup   cleanup,      ///< finalizer (NULL = streamer self-cleans)
    size_t               total_size,   ///< Content-Length, or (size_t)-1 for chunked
    const char          *content_type  ///< MIME type (borrowed; NULL = leave as-is)
);

/**
 * @brief Set a byte-range response (HTTP 206) with Content-Range header.
 *
 * Copies @p length bytes starting at `(uint8_t *)data + offset` into
 * a freshly-allocated body, sets `status_code = 206`, sets
 * `content_type = "application/octet-stream"`, and emits a
 * `Content-Range: bytes <offset>-<offset+length-1>/<total_size>`
 * header per RFC 9110 §15.3.7. Allocates `r->headers` if not already
 * present.
 */
void
axl_http_response_set_range(
    AxlHttpResponse *r,           ///< response
    const void      *data,        ///< full data buffer
    size_t          offset,       ///< byte offset into data
    size_t          length,       ///< number of bytes to send
    size_t          total_size    ///< total size of the resource
);

/**
 * @brief Set the `Content-Range` header on a 206 response.
 *
 * Use when sending a partial-content response via
 * axl_http_response_set_streamer or any other path that doesn't
 * go through axl_http_response_set_range (which sets the
 * header automatically). Callers must set `status_code = 206`
 * separately — this helper only formats and inserts the header.
 *
 * Allocates `r->headers` if not already present, using
 * `axl_hash_table_new_full` with `axl_free_impl` destructors for
 * BOTH keys and values. **If the consumer pre-allocates
 * `r->headers` themselves, it MUST be created with the same
 * destroy-func contract** (e.g. via `axl_hash_table_new_full(
 * axl_str_hash, axl_str_equal, axl_free_impl, axl_free_impl)`).
 * Mixing in a `axl_hash_table_new_str()`-shaped table would leak
 * both the strdup'd key (str-table double-strdups) and value
 * (str-table doesn't own values). Other axl-http-server callers
 * (e.g. WebSocket upgrade handler) build their own headers
 * tables — they don't compose with this helper today, but new
 * consumers should follow the full-destroy-funcs convention.
 *
 * Format per RFC 9110 §15.3.7: `bytes <start>-<end>/<total>`,
 * end inclusive (start <= end < total).
 */
void
axl_http_response_set_content_range(
    AxlHttpResponse *r,        ///< response
    uint64_t         start,    ///< first byte index of the slice
    uint64_t         end,      ///< last byte index of the slice (inclusive)
    uint64_t         total     ///< total size of the resource
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
 * Three terminating call shapes — the handler must distinguish them:
 *
 *   - chunk != NULL, aborted == false: a body chunk arrived. Process
 *     it and return AXL_OK to continue, AXL_ERR to abort and send 500.
 *   - chunk == NULL, chunk_size == 0, aborted == false: clean EOF.
 *     Set @p resp fields here; the response is sent after return.
 *   - chunk == NULL, chunk_size == 0, aborted == true: the connection
 *     was torn down mid-upload (TCP disconnect, recv error, server
 *     shutdown). @p resp is NOT transmitted. The handler MUST NOT
 *     touch the connection, send a response, or call any response
 *     setter (`axl_http_response_set_*`) — the call exists only to
 *     release per-request state (open file handles, accumulators,
 *     allocations) the handler accumulated across earlier chunk
 *     calls. Without this signal, that state leaks into the next
 *     request on the same handler globals — caused cross-request data
 *     corruption in axl-webfs's PUT path.
 *
 * Fires exactly once per upload: clean-EOF and abort calls are
 * mutually exclusive — a handler that already received the clean-EOF
 * call will NOT also receive an abort, even if the response send
 * subsequently fails. Return value is ignored on the abort call.
 *
 * @return AXL_OK on success, AXL_ERR to abort the upload and send 500.
 */
typedef int (*AxlUploadHandler)(
    AxlHttpRequest  *req,         ///< incoming request
    AxlHttpResponse *resp,        ///< response (set on clean-EOF final call only)
    const void      *chunk,       ///< chunk data (NULL on final/abort call)
    size_t           chunk_size,  ///< chunk size (0 on final/abort call)
    void            *data,        ///< opaque caller data
    bool             aborted      ///< true on connection teardown mid-upload
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

// ---------------------------------------------------------------------------
// WebDAV class-1 + MOVE
//
// Generic WebDAV (RFC 4918 §9) server adjunct to AxlHttpServer. The
// SDK owns all the protocol bits (verb dispatch, PROPFIND
// Multi-Status XML, Depth / Destination / Overwrite header parsing,
// DAV: 1 advertisement); the consumer fills in an AxlWebDavOps
// table that maps onto its own filesystem.
//
// Verb scope: OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL,
// MOVE, COPY. PROPPATCH, LOCK, UNLOCK, and If-header conditionals
// remain out of scope (modern clients — Windows Explorer, macOS
// Finder, davfs2, cadaver — work without them when the server
// doesn't advertise the lock class).
//
// GET and PUT inherit the streaming primitives that already exist:
// `axl_http_response_set_streamer` for response bodies (multi-GB
// safe) and the `AxlUploadHandler`-shape upload-route chunk
// callback for request bodies. Range requests on GET use the new
// `axl_http_response_set_content_range` to advertise the slice.
// ---------------------------------------------------------------------------

/* The PROPFIND backing's filesystem entry travels in the canonical
   `AxlFsEntry` (defined in `<axl/axl-fs.h>`). The consumer fills
   `name`, `size`, `mtime_unix`, and the `AXL_FS_ATTR_DIRECTORY`
   bit in `attributes`; the SDK reads those four fields to emit
   PROPFIND XML (resourcetype / getcontentlength / getlastmodified).
   mtime_unix == 0 means "unknown" and the SDK omits the property. */

/**
 * @brief Consumer-supplied filesystem callback table.
 *
 * Every callback receives @p user (the value passed to
 * axl_http_server_add_webdav) and a path RELATIVE to the
 * registered prefix (e.g. with prefix `/dav` and request URL
 * `/dav/foo/bar.txt`, the consumer sees `/foo/bar.txt`). Root path
 * is `/` and refers to the WebDAV mount itself — list_dir("/")
 * returns the top-level entries (one virtual entry per UEFI volume,
 * say).
 *
 * Callbacks return @c AXL_OK on success, @c AXL_ERR on failure. The
 * SDK maps the failure to an HTTP status: stat / list_dir / read
 * failures → 404; mkdir / write_open / move failures → 409 (parent
 * missing) or 500 (other); remove failure → 404 or 423 if locked
 * (latter not fully wired in v1).
 *
 * Streaming callbacks (read/write):
 *   - `read_open` returns an opaque ctx the SDK threads into
 *     `read_chunk` (drives the response-body streamer) and
 *     `read_close` (idempotent finalize).
 *   - `write_open` likewise; `write_chunk` receives one chunk per
 *     dispatcher buffer; `write_close(aborted)` runs on EOF
 *     (aborted=false) OR mid-upload TCP teardown (aborted=true).
 *     Same shape as AxlUploadHandler's clean-EOF/abort
 *     contract.
 */
typedef struct {
    /// PROPFIND backing — list children of a directory.
    int  (*list_dir)(void *user, const char *path,
                     AxlFsEntry *out, size_t max,
                     size_t *count);

    /// Stat — for PROPFIND on a single resource.
    int  (*stat)(void *user, const char *path,
                 AxlFsEntry *out);

    /// Streaming read — drives axl_http_response_set_streamer for GET.
    int  (*read_open)(void *user, const char *path,
                      uint64_t offset, void **out_ctx);
    int  (*read_chunk)(void *ctx, void *buf, size_t buf_size,
                       size_t *bytes_read);
    void (*read_close)(void *ctx);

    /// Streaming write — drives the upload-route chunk handler for PUT.
    int  (*write_open)(void *user, const char *path, void **out_ctx);
    int  (*write_chunk)(void *ctx, const void *data, size_t len);
    void (*write_close)(void *ctx, bool aborted);

    /// Lifecycle — MKCOL / DELETE / MOVE / COPY.
    int  (*mkdir)(void *user, const char *path);
    int  (*remove)(void *user, const char *path);
    int  (*move)(void *user, const char *src, const char *dst,
                 bool overwrite);
    /// COPY: replicate @p src to @p dst, leaving @p src in place.
    /// @p depth is 0 (collection itself only, no contents) or -1
    /// (infinity / deep). The SDK rejects Depth: 1 before reaching
    /// here per RFC 4918 §9.8.3. Returning AXL_ERR maps to 409. To
    /// get RFC-correct 404 (rather than 409) for missing-source,
    /// also set stat — the SDK pre-stats @p src when stat is
    /// wired.
    int  (*copy)(void *user, const char *src, const char *dst,
                 bool overwrite, int depth);

    /// Content-Type hint for GET responses (optional). Returning
    /// NULL or omitting the callback uses application/octet-stream.
    const char *(*content_type)(void *user, const char *path);

    /// Optional: produce a content digest for end-to-end integrity
    /// verification (RFC 3230). When wired AND the client sends a
    /// `Want-Digest: <algo>[, ...]` request header on GET / HEAD,
    /// the SDK iterates the requested algorithms (in client-listed
    /// order) and calls this callback for each — the first call
    /// that returns AXL_OK wins, and the SDK emits the matching
    /// `Digest: <algo>=<hex>` response header.
    ///
    /// @p algo is the lowercased canonical algorithm name as the
    /// client requested it (typically `"sha-256"`; legacy `"sha-1"`
    /// and `"md5"` are also forwarded if requested).
    /// @p out_hex is a caller-allocated buffer of @p hex_size bytes
    /// the consumer fills with the lowercase hex digest + trailing
    /// NUL. (Consumer always produces hex; the SDK emits hex per
    /// the RFC 3230 `id-sha-*` alias convention. The buffer is
    /// sized to fit SHA-512 hex output.) Return AXL_OK on success,
    /// AXL_ERR for any failure (unknown algo, unreadable file,
    /// OOM); on AXL_ERR the SDK silently moves on to the next
    /// algorithm in the Want-Digest list, or omits the header
    /// entirely if none succeed. Same omission behavior as a
    /// non-wired callback.
    ///
    /// Per RFC 3230 §4.3.2, the digest covers the FULL file even
    /// for 206 Partial Content responses — mount clients accumulate
    /// the value across their first Range read.
    int  (*digest)(void *user, const char *path,
                   const char *algo,
                   char *out_hex, size_t hex_size);

    /// Optional last-call hook to mutate the response before the
    /// SDK hands it to the dispatcher for wire send. Fires AFTER
    /// the SDK's per-verb logic has set status, headers, body /
    /// streamer, but BEFORE the dispatcher serializes. Consumer
    /// may add or replace headers via `resp->headers` (lazy-alloc
    /// it if NULL); reading `req->path` / `req->method` to scope
    /// behavior is fine.
    ///
    /// Fires for every WebDAV verb the handler dispatched. For
    /// PUT, fires once on clean EOF (when the response status is
    /// set), NOT per chunk. For HEAD, fires once with the
    /// headers-only response.
    ///
    /// Use cases: custom property emission (ETag, Cache-Control,
    /// resource-versioning headers), audit-trail header injection,
    /// rate-limit hints. For RFC 3230 Digest emission specifically,
    /// wire the @c digest callback instead — the SDK already does
    /// the Want-Digest parsing.
    void (*before_response)(void *user, AxlHttpRequest *req,
                            AxlHttpResponse *resp);
} AxlWebDavOps;

/**
 * @brief Mount a WebDAV handler at @p prefix.
 *
 * Registers verb routes (OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE,
 * MKCOL, MOVE) under `<prefix>/<wildcard>` that drive @p ops. The @p ops
 * table is COPIED into the server — caller may free / re-use the
 * struct after this returns. @p user_data is borrowed and must
 * outlive the server.
 *
 * Up to 4 WebDAV mounts per server. Cleanup is automatic on
 * axl_http_server_free.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments or if the
 *     server already has 4 mounts.
 */
int
axl_http_server_add_webdav(
    AxlHttpServer       *s,
    const char          *prefix,    ///< URL prefix, e.g. "/dav"
    const AxlWebDavOps  *ops,       ///< callback table (copied)
    void                *user_data  ///< opaque, passed back to ops
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_SERVER_H */
