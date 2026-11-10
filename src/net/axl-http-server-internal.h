/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-server-internal.h
    Private header shared between HTTP server implementation files.
**/

#ifndef AXL_HTTP_SERVER_INTERNAL_H
#define AXL_HTTP_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-time.h>
#include <axl/axl-digest.h>
#include <axl/axl-config.h>
#include <axl/axl-http-server.h>
#include <axl/axl-radix-tree.h>
#include "axl-net-internal.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define HTTP_DEFAULT_MAX_CONNS    8
#define HTTP_DEFAULT_BODY_LIMIT   (4 * 1024 * 1024)
#define HTTP_DEFAULT_KEEPALIVE    30
#define HTTP_DEFAULT_MAX_MW       8
/* Hex of the largest digest a PUT's Content-Digest is checked against
   (SHA-512: 128 digits), plus the NUL. */
#define AXL_WEBDAV_DIGEST_HEX_MAX 129
#define HTTP_HEADER_BUF_SIZE      4096
#define HTTP_CHUNK_READ_BUF_SIZE  4096
/* Longest chunk-size line (size and extensions) or trailer line, CRLF
   excluded, the chunk decoder takes; a longer one is 400. */
#define HTTP_CHUNK_LINE_MAX       1024
/* Most trailer bytes after the last chunk; more is 400. */
#define HTTP_CHUNK_TRAILER_MAX    8192
/* TLS ciphertext staging buffer — sized to hold one full TLS record's
   ciphertext (max 16384 plaintext + record overhead) in a single read.
   Reassembly across reads still works for anything larger, but fitting a
   whole record avoids needless re-arm cycles. */
#define HTTP_TLS_CIPHER_BUF_SIZE  16640

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct AxlHttpServer AxlHttpServer;
typedef struct HttpRoute HttpRoute;

/* Outbound WebSocket frame queue node (per-connection send serialization;
   the ws-broadcast-over-TLS desync fix). */
typedef struct WsOutNode {
    uint8_t          *data;   ///< raw WS frame bytes (plaintext, pre-encryption)
    size_t            len;
    struct WsOutNode *next;
} WsOutNode;

typedef struct {
    bool             active;
    bool             tearing_down;   ///< reset_connection in progress (re-entry guard)
    AxlTcp          *sock;
    AxlHttpServer   *server;
    AxlTlsContext   *tls_ctx;
    char             header_buf[HTTP_HEADER_BUF_SIZE];
    size_t           header_len;
    bool             headers_done;
    char            *method;
    char            *path;
    char            *query;
    AxlHashTable    *headers;
    size_t           content_length;
    size_t           body_bytes_read;
    void            *body;
    size_t           body_alloc;
    bool             chunked;
    bool             chunked_done;
    bool             keep_alive;
    char             client_addr[46];
    char             chunk_read_buf[HTTP_CHUNK_READ_BUF_SIZE];
    /* Dedicated TLS ciphertext receive/staging buffer. Kept SEPARATE from
       every plaintext buffer (header_buf / body / chunk_read_buf) so that
       decrypting one record's plaintext can never overwrite a following
       record's not-yet-consumed ciphertext that arrived in the same TCP
       read. Only used on TLS connections. */
    uint8_t          tls_cipher_buf[HTTP_TLS_CIPHER_BUF_SIZE];
    /* Bytes that arrived past the end of the request being read -- the
       start of the next request a client pipelined into the same segment.
       Fed to the next read phase before the transport is read again;
       heap, NULL when empty. */
    char            *carry;
    size_t           carry_len;
    /* Chunked-body decoder state; every field carries across recvs. */
    uint8_t          chunk_state;         ///< HTTP_CHUNK_* (axl-http-request.c)
    char             chunk_line[HTTP_CHUNK_LINE_MAX]; ///< the size or trailer line so far
    size_t           chunk_line_len;
    size_t           chunk_trailer_len;   ///< trailer bytes read so far
    size_t           chunk_remaining;     ///< unconsumed bytes of the in-progress chunk's DATA (spans recvs)
    /* TLS record-drain state. A single TCP segment can carry several
       TLS records; while draining buffered records, the dispatch's
       start_conn_recv must NOT issue a transport recv (it would block on
       bytes already buffered) — instead it sets tls_rearm_wanted and the
       drain loop owns the single transport re-arm once the buffer empties. */
    bool             tls_draining;
    bool             tls_rearm_wanted;
    /* Upload streaming state (set by early route lookup) */
    bool             is_upload_stream;
    bool             upload_ended;     ///< the upload handler had its clean-EOF call: no abort call may follow
    HttpRoute       *upload_route;
    AxlHttpRequest   upload_req;
    AxlHttpResponse  upload_resp;
    uint8_t         *upload_buf;
    size_t           upload_buf_len;
    uint64_t         request_id;       ///< AxlHttpRequest.request_id of the request being parsed
    /* WebSocket state (after upgrade) */
    bool             is_websocket;
    AxlWsHandler     ws_handler;      ///< broadcast-only API (add_websocket)
    AxlWsConnHandler ws_conn_handler; ///< per-connection API (add_websocket_ex)
    void            *ws_data;         ///< per-endpoint opaque
    void            *ws_user_data;    ///< per-connection consumer slot (P1; AXL never frees)
    AxlAuthInfo      ws_auth;         ///< identity captured at upgrade (P1)
    bool             ws_authed;       ///< true once ws_auth is valid (P1)
    bool             ws_connect_pending; ///< fire AXL_WS_CONNECT once the 101 is sent (P1)
    bool             ws_connected;     ///< CONNECT delivered and not rejected — gates DISCONNECT (P1)
    bool             ws_in_connect;    ///< the app's CONNECT handler is on the stack
    bool             ws_close_deferred; ///< a teardown asked for during CONNECT; run when it returns
    bool             ws_close_abortive; ///< ...and whether that teardown was abortive
    char            *ws_path;         ///< path for broadcast matching
    uint8_t         *ws_partial_buf;  ///< incomplete frame buffer
    size_t           ws_partial_len;
    /* Outbound send queue. It began as the ws-broadcast-over-TLS desync fix,
       back when the transport refused a second send and the refused frame had
       already advanced the TLS write seqno. The transport queues now, so this
       FIFO's remaining job is the one nothing below it does: LOSSY, bounded,
       frame-aware back-pressure. ALL outbound WS frames — broadcast,
       axl_ws_send, pong — go through it, enqueued PRE-encryption with only the
       head handed down at a time, so a drop-on-overflow sheds a raw frame and
       never desyncs the stream. */
    WsOutNode       *ws_out_head;
    WsOutNode       *ws_out_tail;
    size_t           ws_out_count;
    size_t           ws_out_bytes;
    bool             ws_out_inflight;  ///< the head frame's async send is pending
    /* Async-response TX state.
     *
     * AxlHttpServer's response path is fully event-driven: the
     * route handler builds an AxlHttpResponse, send_response
     * memcpy's headers + body into a single heap-owned buffer
     * (tx_buf), submits one axl_tcp_send_async (or
     * axl_tls_write_async for TLS), and returns. The completion
     * callback (on_response_sent in axl-http-response.c) frees
     * tx_buf and decides keep-alive vs reset_connection. No
     * synchronous wait inside source callbacks — that path used
     * gBS->WaitForEvent and broke under axl_loop_attach_driver
     * (TPL > TPL_APPLICATION).
     *
     * tx_buf is also freed by reset_connection() if the connection
     * is torn down before the send completes (axl_tcp_close cancels
     * the send, so on_response_sent won't fire). */
    void            *tx_buf;          ///< heap-owned headers+body, freed in completion
    /* Streaming response state. Only meaningful when
     * stream_active == true (set by send_response when the
     * AxlHttpResponse had streamer != NULL). on_response_sent's
     * normal post-send branch is replaced by stream_pump_next while
     * the stream is active; the cleanup hook fires exactly once when
     * the stream completes (EOF) or aborts (error / reset). */
    bool             stream_active;
    AxlResponseStreamer stream_fn;
    void                *stream_ctx;
    AxlResponseCleanup   stream_cleanup;
    size_t           stream_remaining;     ///< Content-Length countdown (0 once exhausted)
    bool             stream_chunked;       ///< Transfer-Encoding: chunked path
    bool             stream_terminator_sent; ///< chunked: final `0\r\n\r\n` queued
    void            *stream_chunk_buf;     ///< heap-owned chunk staging buffer (incl. framing room)
    size_t           stream_chunk_cap;     ///< capacity of stream_chunk_buf in bytes
} HttpConn;

struct HttpRoute {
    char             *method;
    char             *path;
    AxlHttpHandler    handler;
    void             *data;
    uint32_t          auth_flags;
    bool              is_static;
    char             *fs_path;
    bool              is_upload;
    AxlUploadHandler  upload_handler;
};

typedef struct {
    char   *buf;
    size_t  buf_size;
    size_t  len;
    bool    overflow;   /* a line did not fit: the block is incomplete */
} HeaderBuildCtx;

struct AxlHttpServer {
    AxlConfig         *config;
    uint16_t           port;
    /* Optional listen-bind IP. Empty / "0.0.0.0" = auto-pick.
       Otherwise pin to the interface whose station IP matches. */
    char              *listen_ip;
    AxlTcp            *listener;
    HttpConn          *conns;
    size_t             max_conns;
    /* Server code on the stack that can call application code: the loop
       callbacks (http_dispatch), axl_ws_conn_close / axl_ws_send / the
       broadcast, and axl_http_server_run (http_dispatch_begin/_end). While
       any is, axl_http_server_free only records free_pending, and the
       outermost one completes the free once it returns -- the code that
       called the handler still uses the connection and the server. freeing
       guards the free itself, whose teardown calls app hooks (DISCONNECT,
       upload abort) that may free again. */
    uint32_t           dispatch_depth;
    bool               free_pending;
    bool               free_abortive;
    bool               freeing;
    /* axl_http_server_run's private loop, which only a free can end. */
    bool               run_loop;
    AxlHashTable      *exact_routes;   /* "GET /foo"  — O(1) hashed lookup */
    AxlRadixTree      *prefix_routes;  /* "GET /css/" — longest-prefix lookup */
    size_t             body_limit;
    size_t             keep_alive_sec;
    AxlHttpMiddleware *middleware;
    void             **mw_data;
    size_t             mw_count;
    size_t             mw_max;
    AxlLoop           *loop;
    bool               running;
    bool               tls_enabled;
    AxlAuthCallback    auth_cb;
    void              *auth_data;
    /* Optional WWW-Authenticate challenge emitted on a 401 so interactive
       clients (browsers, Finder, Explorer) are prompted. NULL scheme =
       no challenge (preemptive-credentials only). Owned by the server. */
    char              *auth_scheme;
    char              *auth_realm;
    AxlHashTable      *cache;        /* key: "METHOD /path", value: CachedResponse* */
    size_t             cache_max;
    size_t             cache_ttl_ms; /* default TTL */
    uint64_t           cache_seq;    /* monotonic insertion counter for FIFO eviction */
    AxlHashTable      *route_ttls;   /* key: request path, value: (void *)(uintptr_t)ttl_ms; lazily created */
    size_t             upload_chunk_size;
    /* WebSocket routes (small array — typically 1-3 endpoints). A route uses
       EITHER handler (broadcast API) OR conn_handler (per-connection API). */
    struct {
        char            *path;
        AxlWsHandler     handler;       ///< add_websocket (broadcast)
        AxlWsConnHandler conn_handler;  ///< add_websocket_ex (per-connection)
        void            *data;
        uint32_t         auth_flags;    ///< AXL_ROUTE_* for _ex upgrades
    } ws_routes[8];
    size_t             ws_route_count;
    /* WebDAV mounts (heap-allocated AxlWebDavCtx, owned by server,
       freed in axl_http_server_free). Typically 1 mount per server. */
    struct AxlWebDavCtx *webdav_ctxs[4];
    size_t               webdav_ctx_count;
};

// ---------------------------------------------------------------------------
// WebDAV — internal context shared between dispatcher + per-verb handlers
// ---------------------------------------------------------------------------

/* Per-route wrapper: pairs the WebDAV ctx with the verb-specific
   inner handler so a single dispatch wrapper can call inner then
   fire the before_response hook. Lives inline in AxlWebDavCtx —
   sized to fit the 8 verb routes the mount registers (+ slack). */
typedef struct {
    struct AxlWebDavCtx *parent;
    AxlHttpHandler       inner;
} DavRouteSlot;

typedef struct AxlWebDavCtx {
    AxlHttpServer *server;
    char          *prefix;       ///< "/dav" — strdup'd, no trailing slash
    size_t         prefix_len;
    AxlWebDavOps   ops;          ///< copied from caller
    void          *user_data;    ///< borrowed (unless user_data_free set)
    /// When non-NULL, the server OWNS user_data and calls this on free.
    /// Set by axl_http_server_serve_fs for its allocated AxlFsRoot;
    /// left NULL by axl_http_server_add_webdav (user_data borrowed).
    void         (*user_data_free)(void *user_data);

    /* Single in-flight PUT per mount. Concurrent uploads to the
       same WebDAV mount are not supported in v1; a second PUT
       arriving while the first is mid-stream is refused (503,
       Retry-After) and the first continues untouched. Per-connection upload state
       lives in HttpConn; this is the state the WebDAV adapter
       threads across chunk callbacks, keyed on the PUT's request_id
       (an address would be shared by a later request on a reused
       connection). It is held only while a PUT is in flight: every
       call that ends a PUT -- clean EOF, abort, or a chunk call
       returning AXL_ERR -- clears it, so one PUT's failure never
       reaches the next. */
    uint64_t        put_id;      ///< request_id of the PUT in flight; 0 = none
    void           *put_ctx;     ///< opaque ctx from ops.write_open
    bool            put_in_place; ///< write_open declared the write in place
    AxlChecksum    *put_sum;     ///< running digest of its body; NULL = not checked
    char            put_claim[AXL_WEBDAV_DIGEST_HEX_MAX]; ///< the Content-Digest hex it must match

    /* Per-route inner-handler slots. The SDK registers each verb's
       inner handler indirectly via dav_route_dispatch, which fires
       the before_response hook after the inner returns. */
    DavRouteSlot   route_slots[12];
    size_t         route_slot_count;
} AxlWebDavCtx;

void webdav_ctx_free(AxlWebDavCtx *ctx);

// ---------------------------------------------------------------------------
// Configuration descriptors
// ---------------------------------------------------------------------------

extern const AxlConfigDesc http_server_descs[];

// ---------------------------------------------------------------------------
// Internal functions shared across implementation files
// ---------------------------------------------------------------------------

/* axl-http-route.c */
void       uppercase_method(char *out, size_t out_size, const char *method);
HttpRoute *add_route_internal(AxlHttpServer *s, const char *method,
                              const char *path, AxlHttpHandler handler, void *data);
HttpRoute *find_route(AxlHttpServer *s, const char *method, const char *path);
int        static_file_handler(AxlHttpRequest *req, AxlHttpResponse *resp, void *data);

/* axl-http-conn.c */
void start_conn_recv(AxlHttpServer *s, HttpConn *conn);
/* Keep @p n bytes that arrived past the end of the request being read:
   they begin the next one on this connection. When they cannot be kept
   (no memory) the connection is closed after this response instead --
   never are they dropped while it stays open. */
void conn_carry_keep(HttpConn *conn, const char *data, size_t n);
/* HTTP server uses explicit re-arm via start_conn_recv (different
   buffers per phase: header, body, chunk, websocket). Always
   returns false; all continuation is caller-driven. */
bool on_conn_data(AxlTcp *sock, AxlStatus status, void *data);

/* Run a server loop callback's body with the server marked as dispatching,
   and complete an axl_http_server_free the body asked for once the outermost
   callback returns (then returning false: stop, the socket is gone).
   @p s may be NULL -- a callback delivered for a connection already reset --
   and then the body simply runs. */
bool http_dispatch(AxlHttpServer *s, AxlTcpCallback body, AxlTcp *sock,
                   AxlStatus status, void *data);

/* The same bracket for a public entry point the app calls OUTSIDE any server
   callback that can still reach its hooks -- axl_ws_conn_close, axl_ws_send
   and the broadcast fire DISCONNECT when a teardown or a send failure ends a
   connection. http_dispatch_end returns true when it completed a free: the
   server is gone and the caller must not touch it. NULL-safe. */
void http_dispatch_begin(AxlHttpServer *s);
bool http_dispatch_end(AxlHttpServer *s);
void dispatch_and_respond(AxlHttpServer *s, HttpConn *conn);
void reset_connection(HttpConn *conn);
/* Teardown variant: abortively closes the connection's socket (RST) so its
   firmware teardown finalizes synchronously — used by
   axl_http_server_free(., AXL_TEARDOWN_RESET) to release the listen port on return. */
void reset_connection_abortive(HttpConn *conn);

/* axl-http-request.c */
void *grow_buffer(void *old, size_t old_len, size_t new_size);
/* Decode one recv's worth of a chunked body. @return 0, or the HTTP
   status to answer the request with -- 400 for malformed framing, 413 for
   a whole body over the server's limit, 500 when it cannot be buffered --
   in which case nothing more of the body may be used. */
size_t process_chunked_data(AxlHttpServer *s, HttpConn *conn,
                            const char *buf, size_t buf_len);
/* Reset the chunked decoder for a new request. */
void chunked_reset(HttpConn *conn);
void process_request_data(AxlHttpServer *s, HttpConn *conn, size_t bytes);

/* axl-http-dispatch.c */
void cached_response_free(void *data);
int  run_middleware(AxlHttpServer *s, AxlHttpRequest *req, AxlHttpResponse *resp);
size_t http_check_route_auth(AxlHttpServer *s, uint32_t auth_flags,
                             AxlHttpRequest *req);
void dispatch_request(AxlHttpServer *s, HttpConn *conn);

/* axl-http-response.c */
void send_response(HttpConn *conn, AxlHttpResponse *resp);
void end_stream_state(HttpConn *conn);
void send_error_response(HttpConn *conn, size_t status_code);

/* Run and clear a streamer the response carries but will not send. */
void http_response_drop_streamer(AxlHttpResponse *resp);

/* Free what a response being built owns -- its body unless static, its
   headers -- once send_* has copied it out. Idempotent. */
void http_response_release(AxlHttpResponse *resp);

/* Remove Connection / Content-Length / Transfer-Encoding from resp->headers:
   send_response writes its own, and a middleware's copy would contradict it. */
void http_response_strip_framing(AxlHttpResponse *resp);

/* send_error_response, carrying @p resp's headers (what the middleware set)
   but not its body. @p resp stays the caller's to free. */
void send_error_response_headers(HttpConn *conn, size_t status_code,
                                 AxlHttpResponse *resp);

/* The answer to a middleware that refused a request: its response, sent and
   released, on a connection that then closes (a refused request's body may
   already be in flight, and would be read as the next request). A refusal
   that left the status 200 is a 500. */
void send_middleware_refusal(HttpConn *conn, AxlHttpResponse *resp);

/* Queue the interim `HTTP/1.1 100 Continue` (RFC 9110 15.2.1): the request
   asked for one and the server will read its body. Ahead of the final
   response on the same send queue, so the order on the wire is fixed.
   @return false when it could not be queued and the connection was reset
   (a TLS record that failed after encryption leaves the stream unusable),
   in which case the caller stops. */
bool send_continue(HttpConn *conn);

/* axl-http-upload.c */
/* End a streaming upload whose request cannot be read (malformed framing):
   the handler gets its abort call -- the write is discarded -- and the
   client @p status, with the connection closed. */
void stream_upload_fail(HttpConn *conn, size_t status);
void stream_upload_data(AxlHttpServer *s, HttpConn *conn,
                        const void *data, size_t len, bool is_final);

/* axl-http-ws.c */
void handle_websocket_upgrade(AxlHttpServer *s, HttpConn *conn);
/* Enqueue a raw WS frame on conn's outbound queue and pump it. Copies @p frame.
   Returns AXL_OK (queued/sent) or AXL_ERR (NULL args / alloc failure). */
int  ws_outq_enqueue(HttpConn *conn, const void *frame, size_t len);
/* Free every queued outbound frame (called from reset_connection). NULL-safe. */
void ws_outq_clear(HttpConn *conn);
void process_websocket_data(AxlHttpServer *s, HttpConn *conn,
                            const uint8_t *data, size_t data_len);

#endif /* AXL_HTTP_SERVER_INTERNAL_H */
