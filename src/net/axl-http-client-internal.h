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
#include <axl/axl-http-client.h>   /* AxlHttpClient, AxlHttpClientResponse, AxlHttpClientDoneFn, AxlLoop, AxlCancellable */
#include "axl-http-client-tls.h"   /* AxlHttpClientTlsOps, AxlTlsContext, AxlTcp */

#define HTTP_CLIENT_RECV_BUF  8192

struct AxlHttpClient {
    AxlConfig      *config;
    AxlTcp         *sock;
    AxlTlsContext  *tls_ctx;
    char           *connected_host;
    uint16_t        connected_port;
    AxlHashTable   *default_headers;
    bool            tls_enabled;
    bool            tls_verify;
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
   into @p buf[@p cap], NUL-free, with the byte count in @p *out_len. Body
   framing: @p chunked emits Transfer-Encoding: chunked; otherwise
   @p content_length > 0 emits Content-Length (a body of 0 length and not
   chunked emits neither). Content-Type is emitted only when there is a body.
   Shared by the async core and the sync streaming path so there is one
   overflow-safe builder. Returns AXL_OK, or AXL_ERR if the headers do not fit
   @p cap (the caller treats that as a hard error). Defined in
   axl-http-client.c. */
int
_axl_http_build_request(
    char         *buf,
    size_t        cap,
    size_t       *out_len,
    const char   *method,
    const char   *full_path,
    const char   *host,
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
   Returns AXL_OK with @p *out_resp set, or an error (and @p *out_resp NULL). */
int
_axl_http_request_sync(
    AxlHttpClient          *c,
    const char             *method,
    const char             *url,
    const void             *body,
    size_t                  size,
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **out_resp);

#endif /* AXL_HTTP_CLIENT_INTERNAL_H */
