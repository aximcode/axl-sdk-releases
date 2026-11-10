/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-client-internal.h
    Private definitions shared between the sync HTTP client (axl-http-client.c)
    and the async state machine (axl-http-client-async.c): the AxlHttpClient
    struct, the recv-buffer size, and an accessor for the registered TLS ops
    (so the async file reaches TLS through the SAME strippable vtable the sync
    path uses — never a static axl_tls_* reference).

    NOT a public header — internal to src/net.
**/

#ifndef AXL_HTTP_CLIENT_INTERNAL_H
#define AXL_HTTP_CLIENT_INTERNAL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-hash-table.h>
#include <axl/axl-config.h>
#include <axl/axl-url.h>           /* AxlUrl */
#include <axl/axl-http-client.h>   /* AxlHttpClient, AxlHttpClientResponse, AxlHttpClientDoneFn, AxlLoop, AxlCancellable */
#include "axl-http-client-tls.h"   /* AxlHttpClientTlsOps, AxlTlsContext, AxlTcp */

#define HTTP_CLIENT_RECV_BUF  8192

/* Interim 1xx responses read past before the final one (RFC 9110 15.2);
   one more fails the request. Two or three is the real-world most (a 103
   Early Hints, a 100 Continue); 16 bounds a server that never sends a
   final response. */
#define HTTP_CLIENT_INTERIM_MAX  16u

/* Whole-body sanity ceiling. A KNOWN Content-Length body is allowed to grow to
   its declared size (the response buffer is pre-reserved to exactly that, so an
   absurd declaration fails the one allocation cleanly — like the old sync
   do_request's malloc(content_len)); this ceiling rejects an absurd declaration
   up front and bounds the unknown-size cases (chunked, and a body read to the
   close). 256 MiB is far above any realistic firmware image / API response the
   client fetches, so it does not bite real use — it is purely an
   OOM-by-declaration guard. (The earlier 1 MiB cap here silently truncated a
   ~1 MB gBS->LoadImage GET to an error.) */
#define HTTP_CLIENT_BODY_MAX  (256u * 1024u * 1024u)

struct AxlHttpClient {
    AxlConfig      *config;
    AxlTcp         *sock;
    AxlTlsContext  *tls_ctx;
    char           *connected_host;
    uint16_t        connected_port;
    AxlHashTable   *default_headers;
    bool            tls_enabled;
    bool            tls_verify;
    bool            tls_close_notify_lenient;  /* tls.close_notify=lenient */
    bool            keep_alive;
    bool            async_busy;          /* one async request in flight */
    size_t          timeout_ms;
    size_t          connect_timeout_ms;
    int             max_redirects;
    /* Optional source-IPv4 (dotted-quad string). Empty / "0.0.0.0"
       means auto-pick (skip 0.0.0.0 interfaces, prefer subnet match,
       else first valid). */
    char           *source_ip;
    /* Persistent ciphertext staging buffer for TLS recv. Held here
       (not on the stack of client_recv) so the BIO's stage_buf
       pointer remains valid across calls — mbedtls reads ciphertext
       from this buffer incrementally as the caller drains plaintext.
       Keeping it separate from the caller's plaintext destination
       avoids the buffer-aliasing class of bugs (mbedtls writing
       plaintext over still-staged ciphertext). */
    uint8_t         tls_rx_buf[HTTP_CLIENT_RECV_BUF];
};

/* The registered HTTP-client TLS ops (set by axl_tls_init via
   axl_http_client_set_tls_ops), or NULL when TLS was never linked / inited.
   The async machine uses this to reach the async TLS ops without a static
   axl_tls_* reference (keeps mbedTLS strippable). */
const AxlHttpClientTlsOps *
_axl_http_client_tls_ops(void);

/* Build the HTTP request header block (request line through the blank line)
   into a buffer sized to fit it exactly, returned in @p *out (axl_free it;
   NUL-free, not terminated) with its byte count in @p *out_len. The request
   target is @p url's path ("/" when NULL or empty) and, when its query is
   non-empty, "?" and the query -- whole, whatever its length: no part of a
   request is ever cut to fit, since a cut target names a different
   resource. The Host header is @p url's authority (RFC 9110 7.2): its host,
   in brackets when an IPv6 literal, then ":port" exactly when the port is
   not the scheme's default (80 for http, 443 for https). Body
   framing: @p chunked emits Transfer-Encoding: chunked; otherwise
   @p content_length > 0 emits Content-Length (a body of 0 length and not
   chunked emits neither). Content-Type is emitted only when there is a body.
   Shared by the async core and the sync streaming path so there is one
   builder. Returns AXL_OK, or AXL_NO_RESOURCES when the buffer cannot be
   allocated (@p *out NULL). Defined in axl-http-client.c. */
int
_axl_http_build_request(
    char        **out,
    size_t       *out_len,
    const char   *method,
    const AxlUrl *url,
    size_t        content_length,
    bool          chunked,
    const char   *content_type,
    bool          keep_alive,
    AxlHashTable *default_headers,
    AxlHashTable *extra_headers);

/* Internal async request entry — the public axl_http_get_async/post_async and
   every sync wrapper funnel through this. Adds @p method + @p extra_headers
   (borrowed until the callback) to the public async contract. @p sync_close is
   set ONLY by the sync wrapper: its loop is ephemeral, so a connection drop must
   complete the close inline (clear async_loop → axl_tcp_close's loop-free
   fallback) rather than register a close_event on a loop freed before it fires.
   Defined in axl-http-client-async.c. */
int
_axl_http_request_async(
    AxlHttpClient        *c,
    AxlLoop              *loop,
    const char           *method,
    const char           *url,
    const void           *body,
    size_t                size,
    const char           *content_type,
    AxlHashTable         *extra_headers,
    AxlCancellable       *cancel,
    AxlHttpClientDoneFn   cb,
    void                 *user,
    bool                  sync_close);

/* Synchronous request — the single ephemeral-loop wrapper over the async core
   that all sync entry points (get/post/put/delete/request) share. Spins a
   private loop, runs the async request to completion (driving a Poll tick over
   the client's current socket when called at a raised TPL so it progresses
   there too), and harvests the response. Defined in axl-http-client-async.c.
   Returns AXL_OK with @p *out_resp set, or a specific negative AxlStatus
   (and @p *out_resp NULL). */
AxlStatus
_axl_http_request_sync(
    AxlHttpClient          *c,
    const char             *method,
    const char             *url,
    const void             *body,
    size_t                  size,
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **out_resp);

/* How a body framed by the close ended (@p got bytes of it so far).
   AXL_OK at an ORDERLY end: the peer's FIN on a plain connection, its
   close_notify over TLS -- or, with tls.close_notify=lenient, a TLS stream
   ended by a FIN without close_notify (logged as a WARNING). Otherwise
   AXL_INCOMPLETE, with the cause logged: a reset or failed connection, or
   TLS ended without close_notify. Defined in axl-http-client.c. */
AxlStatus
_axl_http_client_close_status(
    const AxlHttpClient *c,
    size_t               got
    );

/* Is the response head with @p status interim (RFC 9110 15.2)? @return 0
   for a final response; 1 for an interim one to read past (@p count, the
   interim heads so far this request, is incremented); AXL_ERR, logged, for
   a 101 -- the client asks for no protocol upgrade -- or one interim head
   past HTTP_CLIENT_INTERIM_MAX. The caller fails the request and drops
   the connection on AXL_ERR. Defined in axl-http-client.c. */
int
_axl_http_client_interim(
    size_t  status,
    size_t *count
    );

#endif /* AXL_HTTP_CLIENT_INTERNAL_H */
