/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-server.h
 *
 * HTTP server with routing, middleware pipeline, WebSocket,
 * authentication, response caching, and upload streaming.
 */

#ifndef AXL_HTTP_SERVER_H
#define AXL_HTTP_SERVER_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-fs.h>          /* AxlFsEntry — used by webdav callbacks */
#include <axl/axl-hash-table.h>
#include <axl/axl-json.h>
#include <axl/axl-bytes.h>
#include <axl/axl-tcp.h>        /* AxlTeardown */

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
                             size_t *out_size) AXL_CB_NOEXCEPT;

    /// Opaque user data passed to streamer on each invocation.
    /// Owned by the caller; lifetime managed via streamer_cleanup.
    void        *streamer_ctx;

    /// Optional finalizer called once the streaming response either
    /// completes (EOF, all bytes sent) OR is aborted (streamer error,
    /// connection reset before EOF). Receives streamer_ctx so
    /// the caller can close files / free buffers. NULL means the
    /// streamer self-cleans via its EOF / error transitions.
    void         (*streamer_cleanup)(void *ctx) AXL_CB_NOEXCEPT;

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
) AXL_CB_NOEXCEPT;

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
) AXL_CB_NOEXCEPT;

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
 *
 * Graceful teardown: in-flight connections are closed with a FIN and,
 * when a loop is still running, their firmware finalization is deferred
 * to it. With `AXL_TEARDOWN_GRACEFUL` the listen port may therefore remain
 * bound until those closes finalize on a later loop tick — fine for normal
 * shutdown, but NOT when the caller is about to stop pumping the loop (e.g.
 * block in `axl_image_run`) and needs the port immediately. For that, pass
 * `AXL_TEARDOWN_RESET`.
 *
 * @p mode (see @ref AxlTeardown):
 * - `AXL_TEARDOWN_GRACEFUL` — orderly FIN close of the listener and every
 *   in-flight connection; deferred finalize; the polite default.
 * - `AXL_TEARDOWN_RESET` — the port-releasing teardown: the listener and every
 *   in-flight connection are closed abortively (TCP RST) and finalized
 *   **synchronously and loop-free** (including draining the accept backlog and
 *   finalizing pending deferred closes), so a fresh
 *   @ref axl_http_server_new + @ref axl_http_server_start on the same port
 *   succeeds **immediately with no loop pumping**, even with connections in
 *   flight. For an in-place self-upgrade / port hand-off. The RST discards
 *   un-ACKed in-flight response bytes — the intended trade — and leaves no
 *   deferred close source behind. RAII cleanup uses `AXL_TEARDOWN_GRACEFUL`.
 */
void
axl_http_server_free(
    AxlHttpServer *server,  ///< server to free (NULL-safe)
    AxlTeardown    mode     ///< AXL_TEARDOWN_GRACEFUL or AXL_TEARDOWN_RESET
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP_ARG(AxlHttpServer, axl_http_server_free, AXL_TEARDOWN_GRACEFUL)
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
 * The reader references the body buffer directly — do not free
 * @c req->body while the reader is in use, and call @c axl_json_free
 * on @p out when done.
 *
 * STRICT RFC 8259 (@c AXL_JSON_STRICT), deliberately NOT the liberal
 * axl_json_parse(): a request body arrives off the network from a client that
 * may be hostile, and that is the case @c AXL_JSON_STRICT exists for. So a
 * comment, an unquoted key, a single-quoted string, a hex literal or a
 * trailing comma in the body is a parse error here. A handler that genuinely
 * wants a lenient body can parse @c req->body itself with any dialect via
 * axl_json_parse().
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
) AXL_CB_NOEXCEPT;

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
typedef void (*AxlResponseCleanup)(void *ctx) AXL_CB_NOEXCEPT;

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
 * Item size 1 in the axl_fread below is load-bearing: it makes the
 * item count the BYTE count, which is what @p out_size means. Any
 * other item size would silently drop the partial trailing item that
 * axl_fread does not count but does write.
 *
 * @code
 * static int file_streamer(void *ctx, void *buf, size_t cap, size_t *out)
 * {
 *     AxlStream *f = (AxlStream *)ctx;
 *     *out = axl_fread(buf, 1, cap, f);
 *     // A short read is EOF (*out == 0 ends the response) unless the
 *     // stream errored, which is the only case the dispatcher must
 *     // hear about.
 *     return axl_ferror(f) ? AXL_ERR : AXL_OK;
 * }
 * static void file_close(void *ctx) { axl_fclose((AxlStream *)ctx); }
 *
 * AxlStream *f = axl_fopen("fs0:/big.iso", "r");
 * AxlFsEntry st;
 * if (f == NULL || axl_file_info("fs0:/big.iso", &st) != AXL_OK) { ... }
 * axl_http_response_set_streamer(resp, file_streamer, f, file_close,
 *                                (size_t)st.size, "application/octet-stream");
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

/// WebSocket event kind delivered to an AxlWsHandler / AxlWsConnHandler.
typedef enum {
    AXL_WS_CONNECT    = 0,  ///< connection opened (after the 101 handshake)
    AXL_WS_TEXT       = 1,  ///< a text frame arrived
    AXL_WS_BINARY     = 2,  ///< a binary frame arrived
    AXL_WS_DISCONNECT = 3   ///< connection closed
} AxlWsEvent;

/**
 * @brief WebSocket event callback.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
typedef int (*AxlWsHandler)(
    AxlWsEvent  event,      ///< one of AXL_WS_CONNECT, AXL_WS_TEXT, AXL_WS_BINARY, AXL_WS_DISCONNECT
    const void *frame,      ///< frame data (NULL for CONNECT/DISCONNECT)
    size_t     frame_size,  ///< frame data size
    void       *data        ///< opaque caller data
) AXL_CB_NOEXCEPT;

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
 * Each client's frame is queued on a per-connection outbound FIFO and handed
 * to the transport one at a time, so a burst of broadcasts is delivered in
 * order without racing. Holding one frame in flight is what keeps the rest
 * DROPPABLE: under sustained back-pressure (a slow client) the queue is
 * bounded and sheds the oldest *unsent* frames — lossy by design, which suits
 * a live feed (a console mirror, a metrics stream); a consumer that needs
 * every byte must apply its own flow control.
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
    /// Caller-defined privilege level returned by the auth callback. A
    /// route gated with AXL_ROUTE_ADMIN admits the request only when
    /// `role >= AXL_ROUTE_ADMIN` (i.e. >= 2) — the route flag doubles as
    /// the role threshold. Lower values authenticate but are not admin.
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
) AXL_CB_NOEXCEPT;

/// Route auth flags, passed to every `*_auth` registration function.
/// Any non-zero value requires the server's auth callback
/// (`axl_http_server_use_auth`) to succeed — if NO callback is
/// registered, a gated request is rejected 401 unconditionally.
#define AXL_ROUTE_NO_AUTH  0x00  ///< open: no authentication
#define AXL_ROUTE_AUTH     0x01  ///< any authenticated user (callback succeeds; 401 otherwise)
#define AXL_ROUTE_ADMIN    0x02  ///< authenticated AND role >= AXL_ROUTE_ADMIN (403 otherwise)

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
 * @brief Set the WWW-Authenticate challenge emitted on a 401.
 *
 * When @p scheme is set, every 401 the server returns carries
 *   `WWW-Authenticate: <scheme> realm="<realm>"`
 * (the realm param is omitted when @p realm is NULL), so interactive
 * clients — browsers, macOS Finder, Windows Explorer — prompt for
 * credentials instead of showing a bare 401. @p scheme is typically
 * `"Basic"`. Passing a NULL @p scheme clears the challenge (the default:
 * no header, which only works with clients that send credentials
 * preemptively). @p scheme must contain no whitespace; neither argument
 * may contain a quote or CR/LF (rejected to prevent header injection).
 *
 * @return AXL_OK on success; AXL_ERR on a NULL server, an invalid
 *     scheme/realm, or allocation failure.
 */
int
axl_http_server_set_auth_challenge(
    AxlHttpServer *s,       ///< server
    const char    *scheme,  ///< e.g. "Basic"; NULL clears the challenge
    const char    *realm    ///< realm label, or NULL to omit the realm
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
// WebSocket — per-connection API (authenticated / per-client endpoints)
// ---------------------------------------------------------------------------
//
// The broadcast-only API above (axl_http_server_add_websocket +
// ws_broadcast) fits a single shared feed to all clients. A per-client
// endpoint — a web terminal, a per-user subscription — needs to know WHICH
// client a frame came from, reply to just that client, and authenticate the
// upgrade. This block adds that without breaking the broadcast API: register
// with axl_http_server_add_websocket_ex instead, and the handler receives an
// AxlWsConn handle.

/// Opaque per-connection WebSocket handle (one per connected client),
/// distinct from the per-endpoint registration. Passed to an
/// AxlWsConnHandler and the axl_ws_* accessors below.
typedef struct AxlWsConn AxlWsConn;

/**
 * @brief Per-connection WebSocket event callback.
 *
 * The richer sibling of AxlWsHandler: it additionally receives the
 * AxlWsConn the event is for, enabling per-client reply (axl_ws_send),
 * identity (axl_ws_conn_auth / axl_ws_conn_peer), and per-connection
 * session state (axl_ws_conn_set_user_data). Registered via
 * axl_http_server_add_websocket_ex.
 *
 * @p event is AXL_WS_CONNECT (stash session state now), AXL_WS_TEXT /
 * AXL_WS_BINARY (a frame), or AXL_WS_DISCONNECT (tear session state down).
 * @p conn is valid for the duration of the call, and for any later
 * axl_ws_* call while the connection is open — but NOT after this handler
 * returns from AXL_WS_DISCONNECT. @p data is the per-endpoint opaque from
 * registration (shared by all clients on the path).
 *
 * AXL_WS_CONNECT fires AFTER the 101 handshake response has been sent, so
 * an `axl_ws_send` from it (a greeting / banner) is valid. Returning
 * AXL_ERR from AXL_WS_CONNECT rejects the connection — it is closed with no
 * further events (no DISCONNECT). For AXL_WS_TEXT / _BINARY the return is
 * currently ignored; call axl_ws_conn_close to drop a connection mid-stream.
 *
 * @return AXL_OK to keep the connection; AXL_ERR from CONNECT to reject it.
 */
typedef int (*AxlWsConnHandler)(
    AxlWsConn  *conn,       ///< the client this event is for
    AxlWsEvent  event,      ///< AXL_WS_CONNECT / _TEXT / _BINARY / _DISCONNECT
    const void *frame,      ///< frame data (NULL for CONNECT/DISCONNECT)
    size_t      frame_size, ///< frame data size
    void       *data        ///< per-endpoint opaque (from registration)
) AXL_CB_NOEXCEPT;

/**
 * @brief Register a WebSocket endpoint with per-connection callbacks + auth.
 *
 * Like axl_http_server_add_websocket, but the handler is an
 * AxlWsConnHandler (per-client handle) and the upgrade is gated by
 * @p auth_flags exactly like an HTTP route: AXL_ROUTE_NO_AUTH opens it;
 * AXL_ROUTE_AUTH / AXL_ROUTE_ADMIN run the server's auth callback
 * (axl_http_server_use_auth) against the upgrade request and reject it
 * 401 / 403 — no handshake — on failure, so an authenticated endpoint never
 * completes the upgrade for an unauthorized client. On success the resolved
 * AxlAuthInfo is attached to the connection (axl_ws_conn_auth).
 *
 * A given path is registered with EITHER this or add_websocket, not both.
 *
 * @return AXL_OK on success; AXL_ERR on NULL @p s / @p path / @p handler,
 *     the route table being full, or OOM.
 */
int
axl_http_server_add_websocket_ex(
    AxlHttpServer    *s,           ///< server
    const char       *path,        ///< WebSocket endpoint path
    AxlWsConnHandler  handler,     ///< per-connection event handler
    void             *data,        ///< per-endpoint opaque
    uint32_t          auth_flags   ///< AXL_ROUTE_NO_AUTH / _AUTH / _ADMIN
);

/**
 * @brief Send a frame to one WebSocket client.
 *
 * The per-client counterpart of axl_http_server_ws_broadcast: delivers
 * exactly to @p conn. @p opcode is AXL_WS_TEXT or AXL_WS_BINARY. Safe to
 * call from within the handler — including AXL_WS_CONNECT (a greeting),
 * which fires after the 101 — or from any later code holding a still-open
 * @p conn (e.g. a terminal pump). NOT valid after that connection's
 * AXL_WS_DISCONNECT handler has returned.
 *
 * Like axl_http_server_ws_broadcast, frames are queued on the connection's
 * outbound FIFO and handed down one at a time, so back-to-back sends are
 * delivered in order; the queue is bounded and drops oldest-unsent under
 * sustained back-pressure. A single
 * frame whose framed size exceeds the per-connection outbound budget (512 KB)
 * is REJECTED with AXL_ERR rather than admitted — chunk a larger payload into
 * multiple sends yourself (one oversized frame could otherwise wedge the
 * single-threaded server draining it through the transport).
 *
 * @return AXL_OK on success; AXL_ERR on NULL @p conn / @p data, a bad
 *     @p opcode, a closed / non-WebSocket connection, a frame larger than the
 *     outbound budget, or a send failure.
 */
int
axl_ws_send(
    AxlWsConn  *conn,    ///< target client
    size_t      opcode,  ///< AXL_WS_TEXT or AXL_WS_BINARY
    const void *data,    ///< frame payload
    size_t      size     ///< payload size in bytes
);

/**
 * @brief Read the authenticated identity captured at upgrade.
 *
 * Fills @p out with the AxlAuthInfo resolved when the connection upgraded
 * on an AXL_ROUTE_AUTH / _ADMIN endpoint. An AXL_ROUTE_NO_AUTH endpoint has
 * no identity: returns AXL_ERR with @p out untouched.
 *
 * @return AXL_OK with @p out filled; AXL_ERR on NULL args or an
 *     unauthenticated endpoint.
 */
int
axl_ws_conn_auth(
    AxlWsConn   *conn,   ///< connection
    AxlAuthInfo *out     ///< [out] identity (username / role)
);

/**
 * @brief Get the peer (client) IPv4 address of a WebSocket connection.
 *
 * Writes the 4 octets of the client's address into @p out (matching the
 * AxlNetInterface.ipv4 byte layout).
 *
 * @return AXL_OK with @p out filled; AXL_ERR on NULL args or an
 *     address that could not be determined.
 */
int
axl_ws_conn_peer(
    AxlWsConn *conn,    ///< connection
    uint8_t    out[4]   ///< [out] client IPv4 (4 octets)
);

/**
 * @brief Attach a per-connection user pointer (a session object).
 *
 * A slot for the consumer's own per-client state (a terminal session, a
 * subscription set), distinct from the per-endpoint @p data shared by all
 * clients on the path. Set it on AXL_WS_CONNECT, use it per frame, free it
 * on AXL_WS_DISCONNECT — AXL never frees it (the consumer owns the lifetime).
 */
void
axl_ws_conn_set_user_data(
    AxlWsConn *conn,   ///< connection
    void      *user    ///< consumer-owned pointer (AXL does not free it)
);

/**
 * @brief Read the per-connection user pointer set by
 *     axl_ws_conn_set_user_data (NULL if never set).
 */
void *
axl_ws_conn_user_data(
    AxlWsConn *conn    ///< connection
);

/**
 * @brief Close a specific WebSocket connection (server-initiated).
 *
 * Sends a WebSocket close frame and tears the connection down; the
 * handler's AXL_WS_DISCONNECT fires as usual. Use for a logout / idle
 * timeout. @p conn is invalid after the disconnect handler returns.
 *
 * @return AXL_OK on success; AXL_ERR on a NULL / closed connection.
 */
int
axl_ws_conn_close(
    AxlWsConn *conn    ///< connection to close
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
) AXL_CB_NOEXCEPT;

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

/**
 * @brief Register a streaming upload route with authentication requirements.
 *
 * Like `axl_http_server_add_upload_route`, but the route is gated by
 * the server's authentication callback (`axl_http_server_use_auth`).
 * The callback runs BEFORE a single body byte reaches the upload
 * handler — a failed check sends 401 (or 403 when an `AXL_ROUTE_ADMIN`
 * route is presented a lesser role) and the body is never streamed.
 * Passing `AXL_ROUTE_NO_AUTH` is identical to the non-auth variant.
 *
 * Auth for uploads is necessarily header-based (cookie / Authorization
 * / client address): the body is not materialized, so the auth
 * callback sees a request whose `body` is NULL.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_server_add_upload_route_auth(
    AxlHttpServer   *s,          ///< server
    const char      *method,     ///< HTTP method
    const char      *path,       ///< path pattern
    AxlUploadHandler handler,    ///< upload handler
    void            *data,       ///< opaque caller data
    uint32_t         auth_flags  ///< AXL_ROUTE_* flags
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
 *
 * `write_close` returns a status because the final flush is where a
 * streaming write actually becomes durable, and nothing earlier can
 * report that: every chunk may have been accepted and the last flush
 * still fail on a full volume or write-protected media. On the clean-EOF
 * call an `AXL_ERR` return makes PUT answer **500**, not 201 — a backend
 * that cannot guarantee the bytes landed must say so here or the client
 * records a stored file that does not exist. The return is IGNORED on
 * the abort call (`aborted=true`), where the partial file is expected
 * and no response is transmitted.
 *
 * **Wire `write_open` and `write_close` as a PAIR.** A vtable with a
 * `write_open` but a NULL `write_close` still accepts uploads, and
 * answers **201 Created** for every one that carried a body — the only
 * place a durability failure can be reported is the call that is
 * missing (an empty-body PUT answers 405 instead, since it cannot
 * materialize the file without a close). The SDK's own
 * `axl_http_serve_fs` backend fills or nulls the two together; a
 * hand-written vtable is where the halves drift apart.
 */
typedef struct {
    /// PROPFIND backing — list children of a directory.
    int  (*list_dir)(void *user, const char *path,
                     AxlFsEntry *out, size_t max,
                     size_t *count) AXL_CB_NOEXCEPT;

    /// Stat — for PROPFIND on a single resource.
    int  (*stat)(void *user, const char *path,
                 AxlFsEntry *out) AXL_CB_NOEXCEPT;

    /// Streaming read — drives axl_http_response_set_streamer for GET.
    int  (*read_open)(void *user, const char *path,
                      uint64_t offset, void **out_ctx) AXL_CB_NOEXCEPT;
    int  (*read_chunk)(void *ctx, void *buf, size_t buf_size,
                       size_t *bytes_read) AXL_CB_NOEXCEPT;
    void (*read_close)(void *ctx) AXL_CB_NOEXCEPT;

    /// Streaming write — drives the upload-route chunk handler for PUT.
    int  (*write_open)(void *user, const char *path, void **out_ctx) AXL_CB_NOEXCEPT;
    int  (*write_chunk)(void *ctx, const void *data, size_t len) AXL_CB_NOEXCEPT;
    int  (*write_close)(void *ctx, bool aborted) AXL_CB_NOEXCEPT;

    /// Lifecycle — MKCOL / DELETE / MOVE / COPY.
    int  (*mkdir)(void *user, const char *path) AXL_CB_NOEXCEPT;
    int  (*remove)(void *user, const char *path) AXL_CB_NOEXCEPT;
    int  (*move)(void *user, const char *src, const char *dst,
                 bool overwrite) AXL_CB_NOEXCEPT;
    /// COPY: replicate @p src to @p dst, leaving @p src in place.
    /// @p depth is 0 (collection itself only, no contents) or -1
    /// (infinity / deep). The SDK rejects Depth: 1 before reaching
    /// here per RFC 4918 §9.8.3. Returning AXL_ERR maps to 409. To
    /// get RFC-correct 404 (rather than 409) for missing-source,
    /// also set stat — the SDK pre-stats @p src when stat is
    /// wired.
    int  (*copy)(void *user, const char *src, const char *dst,
                 bool overwrite, int depth) AXL_CB_NOEXCEPT;

    /// Content-Type hint for GET responses (optional). Returning
    /// NULL or omitting the callback uses application/octet-stream.
    const char *(*content_type)(void *user, const char *path) AXL_CB_NOEXCEPT;

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
                   char *out_hex, size_t hex_size) AXL_CB_NOEXCEPT;

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
                            AxlHttpResponse *resp) AXL_CB_NOEXCEPT;
} AxlWebDavOps;

/**
 * @brief Mount a WebDAV handler at @p prefix.
 *
 * Registers verb routes (OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE,
 * MKCOL, MOVE, COPY) under `<prefix>/<wildcard>` that drive @p ops. The @p ops
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
    AxlHttpServer       *s,         ///< server
    const char          *prefix,    ///< URL prefix, e.g. "/dav"
    const AxlWebDavOps  *ops,       ///< callback table (copied)
    void                *user_data  ///< opaque, passed back to ops
);

/**
 * @brief Mount a WebDAV handler with authentication requirements.
 *
 * Like `axl_http_server_add_webdav`, but @p auth_flags is applied to
 * every verb route the mount registers — including the streaming PUT,
 * which is gated before any body byte is read (see
 * `axl_http_server_add_upload_route_auth`). The server's
 * authentication callback (`axl_http_server_use_auth`) therefore gates
 * the whole mount with no per-route glue. `AXL_ROUTE_NO_AUTH` leaves
 * it open — identical to `axl_http_server_add_webdav`.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments or if the
 *     server already has the maximum number of WebDAV mounts.
 */
int
axl_http_server_add_webdav_auth(
    AxlHttpServer       *s,          ///< server
    const char          *prefix,     ///< URL prefix, e.g. "/dav"
    const AxlWebDavOps  *ops,        ///< callback table (copied)
    void                *user_data,  ///< opaque, passed back to ops
    uint32_t             auth_flags  ///< AXL_ROUTE_* flags applied to every verb route
);

// ---------------------------------------------------------------------------
// Filesystem-backed WebDAV — the generic glue between add_webdav and
// <axl/axl-fs.h>, so a consumer serving a mounted volume doesn't
// hand-write the ~13 AxlWebDavOps callbacks.
// ---------------------------------------------------------------------------

/// Serve read-only: PUT / DELETE / MKCOL / MOVE / COPY are rejected
/// with 405 Method Not Allowed; GET / HEAD / PROPFIND / OPTIONS work.
#define AXL_SERVE_FS_READONLY     0x01u
/// Reject the DELETE verb (405). Scope is literal: DELETE only. MOVE is
/// still allowed (and a MOVE relocates, removing the source from its
/// original path) — use AXL_SERVE_FS_READONLY for "nothing changes".
#define AXL_SERVE_FS_NO_DELETE    0x02u
/// PUT / MOVE / COPY must not replace an existing resource: an
/// overwrite is refused. Enforced by a destination existence-check in
/// the ops (PUT opens with exclusive-create; MOVE/COPY pre-stat the
/// destination — `axl_file_move` itself replaces unconditionally, so it
/// is never delegated the check). A refusal surfaces as 409 Conflict —
/// the ops layer has no channel to return the RFC-ideal 412 in v1.
#define AXL_SERVE_FS_NO_OVERWRITE 0x04u

/**
 * @brief A filesystem subtree bound to a WebDAV path-space.
 *
 * The `user_data` for `axl_fs_webdav_ops()`. Maps each relative WebDAV
 * path onto a path within `fs_root` under the given access flags.
 *
 * Traversal containment (security guarantee): the request path is
 * normalized against the mount root first — `.` / `..` components are
 * resolved away and any `..` that would climb above the mount is
 * rejected (404) — so the mapped path is always inside `fs_root`. A
 * request like `/../../FS0:/secret` cannot escape the served subtree.
 *
 * Opaque; create with `axl_fs_root_new`, free with `axl_fs_root_free`.
 */
typedef struct AxlFsRoot AxlFsRoot;

/**
 * @brief Bind a filesystem subtree to serve via WebDAV.
 *
 * @p fs_root is an axl filesystem path — a volume root like `"FS0:"` or
 * a subdirectory like `"FS0:\\share"` (a RAM-disk volume works too).
 * Either separator works (`"FS0:/share"` too); a trailing separator is
 * accepted and normalized away. The WebDAV root path `/` maps to
 * `fs_root` itself, so `list_dir("/")` lists `fs_root`'s entries.
 * The returned root is the `user_data` you pass to
 * `axl_http_server_add_webdav` alongside `axl_fs_webdav_ops()`. It is
 * borrowed by the mount, never freed by `add_webdav` — you free it once
 * with `axl_fs_root_free` after every server using it is freed (it may
 * back more than one mount).
 *
 * @return new root, or NULL on OOM / NULL @p fs_root.
 */
AxlFsRoot *
axl_fs_root_new(
    const char *fs_root,   ///< filesystem base path (e.g. "FS0:" or "FS0:\\share")
    uint32_t    flags      ///< AXL_SERVE_FS_* access flags
);

/**
 * @brief Free an AxlFsRoot. NULL-safe.
 */
void
axl_fs_root_free(
    AxlFsRoot *root
);

/**
 * @brief The generic axl-fs-backed WebDAV callback table.
 *
 * Each callback maps to an `<axl/axl-fs.h>` primitive (list_dir →
 * `axl_dir_*`, stat → `axl_file_info`, read → `axl_file_view`, write →
 * `AxlFileWriter`, mkdir → `axl_dir_mkdir`, remove →
 * `axl_file_delete`/`axl_dir_rmdir`, move → `axl_file_move`, copy →
 * stream read+write / recursive). Pass it to
 * `axl_http_server_add_webdav` with an `AxlFsRoot*` as `user_data`. A
 * consumer that wants to wrap or extend the behavior can copy this
 * table and override individual callbacks.
 *
 * The returned table is the full read-write set regardless of any
 * AxlFsRoot flags — the flags are applied by `axl_http_server_serve_fs`,
 * which drops the mutating callbacks from its own copy for READONLY /
 * NO_DELETE. A consumer copying this table for the READONLY case should
 * NULL the mutators itself (the handler returns 405 for a NULL op).
 *
 * @return pointer to a static, immutable ops table (do not free). All
 *     state lives in the AxlFsRoot user_data, so the table is
 *     re-entrant across mounts.
 */
const AxlWebDavOps *
axl_fs_webdav_ops(void);

/**
 * @brief Mount a filesystem subtree as a WebDAV file server. One call.
 *
 * Equivalent to `axl_fs_root_new(fs_root, flags)` + a copy of
 * `axl_fs_webdav_ops()` (with mutating callbacks dropped per the flags)
 * registered via `axl_http_server_add_webdav` at @p prefix. The server
 * owns the root and frees it on `axl_http_server_free`.
 *
 * The mount is traversal-contained: a request path is normalized and
 * any `..` escaping @p fs_root is rejected (404), so the server can
 * only ever touch files within @p fs_root (see AxlFsRoot).
 *
 * Auth/role gating is built in: @p auth_flags is applied to every verb
 * route, including the streaming PUT, so the server's authentication
 * callback (`axl_http_server_use_auth`) gates the whole mount. Pass
 * `AXL_ROUTE_NO_AUTH` for an open mount, `AXL_ROUTE_AUTH` to require a
 * logged-in user, or `AXL_ROUTE_ADMIN` to require the admin role.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments, OOM, or if the
 *     server already has the maximum number of WebDAV mounts.
 */
int
axl_http_server_serve_fs(
    AxlHttpServer *s,          ///< server
    const char    *prefix,     ///< URL prefix, e.g. "/dav"
    const char    *fs_root,    ///< filesystem base path (e.g. "FS0:")
    uint32_t       flags,      ///< AXL_SERVE_FS_* access flags
    uint32_t       auth_flags  ///< AXL_ROUTE_* flags; gates the whole mount
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_SERVER_H */
