/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-client-tls.h
    Indirection that lets the always-linked HTTP client reach the optional
    TLS module WITHOUT a static reference to it.

    A static `axl_tls_*` call from the client object pins the entire mbedTLS
    (~280 KB) into every client consumer — even one that only ever speaks
    plain `http://` — because the TLS handshake graph is all-or-nothing once
    referenced, so `ld --gc-sections` can't drop it. Routing the client's
    `https` path through this vtable (populated only when the consumer
    references TLS, via `axl_tls_init`) lets `--gc-sections` drop the TLS
    module + mbedTLS for a plain-HTTP client, while `https` clients (which
    call `axl_tls_init`) and `axl_http_server_use_tls` still pull it in.

    NOT a public header — internal to src/net.
**/

#ifndef AXL_HTTP_CLIENT_TLS_H
#define AXL_HTTP_CLIENT_TLS_H

#include <stddef.h>
/* AxlTcp, AxlTcpCallback, AxlLoop, AxlStatus — type declarations only, no TLS
   reference, so the header stays self-contained and never pins mbedTLS.
   axl-tcp.h uses AxlIPv4Address without including its header, so pull that
   first. */
#include <axl/axl-inet-address.h>
#include <axl/axl-tcp.h>

/* Opaque — defined in axl-tls.h. Forward-declared so this header is
   self-contained and never references TLS at link time. */
typedef struct AxlTlsContext AxlTlsContext;

/* The TLS operations the HTTP client needs. Field signatures match the
   corresponding axl_tls_* functions exactly, so axl-tls.c can register the
   real functions directly. The async pair (handshake_async / write_async) is
   what lets the ALWAYS-LINKED async HTTP client drive a TLS request on a loop
   without a static axl_tls_* reference — keeping mbedTLS strippable for a
   plain-HTTP consumer exactly as the sync ops do. */
typedef struct {
    AxlTlsContext *(*connect)(AxlTcp *sock, const char *hostname);
    void           (*free)(AxlTlsContext *ctx);
    void           (*stage_data)(AxlTlsContext *ctx, const void *data, size_t len);
    int            (*handshake)(AxlTlsContext *ctx);
    int            (*write)(AxlTlsContext *ctx, const void *data, size_t len);
    int            (*read)(AxlTlsContext *ctx, void *buf, size_t size, size_t *out_len);
    int            (*handshake_async)(AxlTlsContext *ctx, AxlLoop *loop);
    int            (*write_async)(AxlTlsContext *ctx, const void *data, size_t len,
                                  AxlLoop *loop, AxlTcpCallback cb, void *cb_data);
} AxlHttpClientTlsOps;

/**
 * @brief Wire the HTTP client's TLS path to a concrete implementation.
 *
 * Called by `axl_tls_init()` so a consumer that references TLS (an `https`
 * client via `axl_tls_init`, or the server via `axl_http_server_use_tls`)
 * enables the client's `https` support — while a consumer that never
 * references TLS leaves @p ops NULL and the linker strips mbedTLS.
 *
 * @param ops static-storage ops table (borrowed, not copied), or NULL.
 */
void
axl_http_client_set_tls_ops(const AxlHttpClientTlsOps *ops);

#endif /* AXL_HTTP_CLIENT_TLS_H */
