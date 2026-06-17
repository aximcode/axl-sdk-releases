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
#define HTTP_HEADER_BUF_SIZE      4096
#define HTTP_CHUNK_READ_BUF_SIZE  4096
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
    char             chunk_leftover[16];  ///< partial chunk size-line from previous recv
    size_t           chunk_leftover_len;
    size_t           chunk_remaining;     ///< unconsumed bytes of the in-progress chunk's DATA (spans recvs)
    uint8_t          chunk_crlf_pending;  ///< trailing CRLF bytes still to skip after a chunk's data (0..2)
    /* TLS record-drain state. A single TCP segment can carry several
       TLS records; while draining buffered records, the dispatch's
       start_conn_recv must NOT issue a transport recv (it would block on
       bytes already buffered) — instead it sets tls_rearm_wanted and the
       drain loop owns the single transport re-arm once the buffer empties. */
    bool             tls_draining;
    bool             tls_rearm_wanted;
    /* Upload streaming state (set by early route lookup) */
    bool             is_upload_stream;
    HttpRoute       *upload_route;
    AxlHttpRequest   upload_req;
    AxlHttpResponse  upload_resp;
    uint8_t         *upload_buf;
    size_t           upload_buf_len;
    /* WebSocket state (after upgrade) */
    bool             is_websocket;
    AxlWsHandler     ws_handler;      ///< broadcast-only API (add_websocket)
    AxlWsConnHandler ws_conn_handler; ///< per-connection API (add_websocket_ex)
    void            *ws_data;         ///< per-endpoint opaque
    void            *ws_user_data;    ///< per-connection consumer slot (P1; AXL never frees)
    AxlAuthInfo      ws_auth;         ///< identity captured at upgrade (P1)
    bool             ws_authed;       ///< true once ws_auth is valid (P1)
    bool             ws_connect_pending; ///< fire AXL_WS_CONNECT once the 101 is sent (P1)
    bool             ws_connected;     ///< CONNECT delivered AND accepted — gates DISCONNECT (P1)
    char            *ws_path;         ///< path for broadcast matching
    uint8_t         *ws_partial_buf;  ///< incomplete frame buffer
    size_t           ws_partial_len;
    /* Outbound send queue (the ws-broadcast-over-TLS desync fix). ALL outbound
       WS frames — broadcast, axl_ws_send, pong — go through this per-connection
       FIFO so they SERIALIZE over the one-send-in-flight transport instead of
       being encrypted-then-dropped (which advances the TLS write seqno for a
       record never put on the wire, desyncing the stream). Frames are enqueued
       PRE-encryption; only the head is encrypted, at flush time, so a
       drop-on-overflow drops a raw frame and never desyncs. */
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
       arriving while the first is mid-stream causes the first to
       be aborted (write_close(aborted=true)) and the second to
       fail. Same constraint axl-webfs's PUT handler operates
       under. Per-connection upload state lives in HttpConn; this
       is the consumer-visible state the SDK has to thread across
       chunk callbacks for the WebDAV adapter. */
    AxlHttpRequest *put_req;     ///< identity check; NULL = no PUT in flight
    void           *put_ctx;     ///< opaque ctx from ops.write_open
    bool            put_failed;  ///< sticky after a write_chunk error

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
/* HTTP server uses explicit re-arm via start_conn_recv (different
   buffers per phase: header, body, chunk, websocket). Always
   returns false; all continuation is caller-driven. */
bool on_conn_data(AxlTcp *sock, AxlStatus status, void *data);
void dispatch_and_respond(AxlHttpServer *s, HttpConn *conn);
void reset_connection(HttpConn *conn);

/* axl-http-request.c */
void *grow_buffer(void *old, size_t old_len, size_t new_size);
void process_chunked_data(AxlHttpServer *s, HttpConn *conn,
                          const char *buf, size_t buf_len);
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

/* axl-http-upload.c */
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
