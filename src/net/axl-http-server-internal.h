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

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct AxlHttpServer AxlHttpServer;
typedef struct HttpRoute HttpRoute;

typedef struct {
    bool             active;
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
    char             chunk_leftover[16];  ///< partial chunk header from previous recv
    size_t           chunk_leftover_len;
    /* Upload streaming state (set by early route lookup) */
    bool             is_upload_stream;
    HttpRoute       *upload_route;
    AxlHttpRequest   upload_req;
    AxlHttpResponse  upload_resp;
    uint8_t         *upload_buf;
    size_t           upload_buf_len;
    /* WebSocket state (after upgrade) */
    bool             is_websocket;
    AxlWsHandler     ws_handler;
    void            *ws_data;
    char            *ws_path;         ///< path for broadcast matching
    uint8_t         *ws_partial_buf;  ///< incomplete frame buffer
    size_t           ws_partial_len;
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
    AxlHashTable      *cache;        /* key: "METHOD /path", value: CachedResponse* */
    size_t             cache_max;
    size_t             cache_ttl_ms; /* default TTL */
    uint64_t           cache_seq;    /* monotonic insertion counter for FIFO eviction */
    AxlHashTable      *route_ttls;   /* key: request path, value: (void *)(uintptr_t)ttl_ms; lazily created */
    size_t             upload_chunk_size;
    /* WebSocket routes (small array — typically 1-3 endpoints) */
    struct {
        char         *path;
        AxlWsHandler  handler;
        void         *data;
    } ws_routes[8];
    size_t             ws_route_count;
};

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
void dispatch_request(AxlHttpServer *s, HttpConn *conn);

/* axl-http-response.c */
void send_response(HttpConn *conn, AxlHttpResponse *resp);
void send_error_response(HttpConn *conn, size_t status_code);

/* axl-http-upload.c */
void stream_upload_data(AxlHttpServer *s, HttpConn *conn,
                        const void *data, size_t len, bool is_final);

/* axl-http-ws.c */
void handle_websocket_upgrade(AxlHttpServer *s, HttpConn *conn);
void process_websocket_data(AxlHttpServer *s, HttpConn *conn,
                            const uint8_t *data, size_t data_len);

#endif /* AXL_HTTP_SERVER_INTERNAL_H */
