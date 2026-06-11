/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tcp-sync.c
    AxlTcp — synchronous (blocking) TCP API over EFI_TCP4_PROTOCOL.

    The blocking entry points spin up a temporary AxlLoop, call the
    corresponding _async / _start function from axl-tcp-async.c, and
    pump the loop until completion or timeout. The listener setup
    (axl_tcp_listen) is the one sync function that is not a wrapper
    over async — it configures a passive TCP4 child synchronously.
    Lifecycle (axl_tcp_close) and address getters also live here.
**/

#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-wait.h>
#include <axl/axl-cancellable.h>

#include "axl-net-internal.h"
#include "axl-tcp-internal.h"

AXL_LOG_DOMAIN("tcp");

typedef struct {
    AxlTcp    *sock;
    AxlStatus  status;
    bool       done;
    AxlLoop   *loop;
} SyncResult;

/* Heap-owned context for an in-flight TCP close. The Close()
   token must outlive axl_tcp_close because EDK2 parks
   &ctx->close_token in Sock->CloseToken and dereferences it from
   SockConnClosed long after the call returns. See axl_tcp_close. */
typedef struct {
    EFI_TCP4_CLOSE_TOKEN  close_token;
    AxlTcp               *sock;
    AxlLoop              *loop;       /* set on the async path */
    uint32_t              source_id;  /* set on the async path */
} AxlTcpCloseCtx;

// ---------------------------------------------------------------------------
// Locate TCP4 service binding — thin wrapper over the generic
// axl_net_locate_sb (in axl-net-interfaces.c). The same locator
// serves UDP via axl_udp_open_via.
// ---------------------------------------------------------------------------

EFI_STATUS
tcp_find_service_binding(
    const EFI_IPv4_ADDRESS         *dest,
    const EFI_IPv4_ADDRESS         *forced_source,
    EFI_SERVICE_BINDING_PROTOCOL  **sb,
    EFI_HANDLE                    *out_handle
    )
{
    return axl_net_locate_sb(&gEfiTcp4ServiceBindingProtocolGuid,
                             dest, forced_source, sb, out_handle);
}

// ---------------------------------------------------------------------------
// Create a TCP4 child handle
// ---------------------------------------------------------------------------

EFI_STATUS
tcp_create_child(
    EFI_SERVICE_BINDING_PROTOCOL  *sb,
    EFI_HANDLE                    *child_handle,
    EFI_TCP4_PROTOCOL             **tcp4
    )
{
    EFI_STATUS  status;

    *child_handle = NULL;
    status = axl_efi_call(sb->CreateChild, 2, sb, child_handle);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    *child_handle,
                    &gEfiTcp4ProtocolGuid,
                    (void **)tcp4
                    );
    if (EFI_ERROR(status)) {
        axl_efi_call(sb->DestroyChild, 2, sb, *child_handle);
        *child_handle = NULL;
        return status;
    }

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Blocking wrappers — thin layer over async functions + temporary AxlLoop
// ---------------------------------------------------------------------------

static bool
on_sync_complete(AxlTcp *sock, AxlStatus status, void *data)
{
    SyncResult *r = data;
    r->sock   = sock;
    r->status = status;
    r->done   = true;
    axl_loop_quit(r->loop);
    /* Sync wrappers want a single fire — stop any re-arming. */
    return false;
}

// ---------------------------------------------------------------------------
// axl_tcp_connect
// ---------------------------------------------------------------------------

static bool
on_sync_cancel_timeout(void *data)
{
    axl_cancellable_cancel((AxlCancellable *)data);
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_connect_via(const char *host, uint16_t port,
                    const AxlIPv4Address *source_ip,
                    AxlTcp **out_sock)
{
    if (host == NULL || out_sock == NULL) {
        return AXL_ERR;
    }

    AxlLoop        *loop   = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    SyncResult r = { .sock = NULL, .status = AXL_ERR, .done = false, .loop = loop };

    if (axl_tcp_connect_async_via(host, port, source_ip, loop, cancel,
                                  on_sync_complete, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return AXL_ERR;
    }

    axl_loop_add_timeout(loop, 10000, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    /* axl_tcp_connect_async stamped sock->async_loop with this
       ephemeral loop. Clear it before the loop frees so a subsequent
       axl_tcp_close on the returned sock does not dereference freed
       memory while deciding sync-vs-async finalization. */
    if (r.sock != NULL) {
        r.sock->async_loop = NULL;
    }

    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    /* On cancel/error the async op fires cb with NULL sock and closes
       the partial socket internally. Only hand back on clean success. */
    if (r.status == AXL_OK) {
        *out_sock = r.sock;
        return AXL_OK;
    }
    return AXL_ERR;
}

int
axl_tcp_connect(const char *host, uint16_t port, AxlTcp **out_sock)
{
    /* Legacy entry point — auto-pick the source interface. */
    return axl_tcp_connect_via(host, port, NULL, out_sock);
}

// ---------------------------------------------------------------------------
// axl_tcp_listen
// ---------------------------------------------------------------------------

int
axl_tcp_listen_via(uint16_t port, const AxlIPv4Address *source_ip,
                   AxlTcp **out_listener)
{
    EFI_STATUS                    status;
    EFI_SERVICE_BINDING_PROTOCOL  *sb;
    EFI_HANDLE                    sb_handle;
    EFI_HANDLE                    child_handle;
    EFI_TCP4_PROTOCOL             *tcp4;
    EFI_TCP4_CONFIG_DATA          tcp_config;
    EFI_TCP4_ACCESS_POINT         access_point;
    AxlTcp                       *sock;

    if (out_listener == NULL) {
        return AXL_ERR;
    }

    /* Listener: no destination IP. If source_ip is non-NULL non-zero,
       pin to that interface; else first valid handle (skip 0.0.0.0). */
    EFI_IPv4_ADDRESS  efi_src;
    EFI_IPv4_ADDRESS *forced = NULL;
    if (source_ip != NULL) {
        bool nonzero = source_ip->addr[0] || source_ip->addr[1]
                    || source_ip->addr[2] || source_ip->addr[3];
        if (nonzero) {
            axl_memcpy(efi_src.Addr, source_ip->addr, 4);
            forced = &efi_src;
        }
    }
    status = tcp_find_service_binding(NULL, forced, &sb, &sb_handle);
    if (EFI_ERROR(status)) {
        if (forced != NULL) {
            axl_error("no interface with station IP %u.%u.%u.%u",
                source_ip->addr[0], source_ip->addr[1],
                source_ip->addr[2], source_ip->addr[3]);
        } else {
            axl_error("no TCP4 service binding");
        }
        return AXL_ERR;
    }

    status = tcp_create_child(sb, &child_handle, &tcp4);
    if (EFI_ERROR(status)) {
        axl_error("TCP4 CreateChild: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    //
    // Configure as passive (listener)
    //
    axl_memset(&access_point, 0, sizeof(access_point));
    access_point.ActiveFlag       = false;
    access_point.UseDefaultAddress = true;
    access_point.StationPort      = port;

    axl_memset(&tcp_config, 0, sizeof(tcp_config));
    tcp_config.TimeToLive  = 64;
    tcp_config.AccessPoint = access_point;

    //
    // Retry on EFI_NO_MAPPING (DHCP may still be in progress)
    //
    for (size_t retry = 0; retry <= TCP_MAPPING_RETRIES; retry++) {
        status = axl_efi_call(tcp4->Configure, 2, tcp4, &tcp_config);
        if (status != EFI_NO_MAPPING || retry == TCP_MAPPING_RETRIES) {
            break;
        }

        axl_debug("TCP4 Configure(listen): no mapping, waiting for DHCP(%llu/%u)",
                  (unsigned long long)(retry + 1), TCP_MAPPING_RETRIES);
        axl_msleep(TCP_MAPPING_DELAY / 1000);
    }

    if (EFI_ERROR(status)) {
        axl_error("TCP4 Configure(listen port %u): %llx", port, (unsigned long long)status);
        axl_efi_call(sb->DestroyChild, 2, sb, child_handle);
        return AXL_ERR;
    }

    sock = axl_calloc(1, sizeof(AxlTcp));
    if (sock == NULL) {
        axl_efi_call(tcp4->Configure, 2, tcp4, NULL);
        axl_efi_call(sb->DestroyChild, 2, sb, child_handle);
        return AXL_ERR;
    }

    sock->tcp4       = tcp4;
    sock->tcp_handle = child_handle;
    sock->tcp_sb     = sb;
    sock->sb_handle  = sb_handle;
    sock->is_listener = true;

    *out_listener = sock;
    axl_info("listening on port %u", port);
    return AXL_OK;
}

int
axl_tcp_listen(uint16_t port, AxlTcp **out_listener)
{
    /* Legacy entry point — auto-pick the listening interface. */
    return axl_tcp_listen_via(port, NULL, out_listener);
}

// ---------------------------------------------------------------------------
// axl_tcp_accept
// ---------------------------------------------------------------------------

int
axl_tcp_accept(AxlTcp *listener, AxlTcp **out_client, size_t timeout_ms)
{
    if (listener == NULL || out_client == NULL || !listener->is_listener) {
        return AXL_ERR;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    SyncResult r = { .sock = NULL, .status = AXL_ERR, .done = false, .loop = loop };

    if (axl_tcp_accept_async(listener, loop, cancel,
                             on_sync_complete, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return AXL_ERR;
    }

    if (timeout_ms > 0) {
        axl_loop_add_timeout(loop, timeout_ms, on_sync_cancel_timeout, cancel);
    }
    axl_loop_run(loop);

    /* Tear down the per-call accept state before freeing the loop.
       on_accept_complete's success path re-arms and returns CONTINUE
       (accept is persistent by design), which would leave
       accept_source, accept_cancel_source, and the acc_token event
       registered against the ephemeral loop — a later close() or
       axl_tcp_accept() reuse would dereference the freed loop.
       axl_tcp_accept_drop_sources() (exported by axl-tcp-async.c)
       removes both sources and closes the event. */
    axl_tcp_accept_drop_sources(listener);
    listener->async_loop = NULL;

    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    if (r.status == AXL_OK) {
        *out_client = r.sock;
        return AXL_OK;
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// axl_tcp_send
// ---------------------------------------------------------------------------

int
axl_tcp_send(AxlTcp *sock, const void *data, size_t size, size_t timeout_ms)
{
    if (sock == NULL || data == NULL || size == 0) {
        return AXL_ERR;
    }

    if (timeout_ms == 0) {
        timeout_ms = 10000;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    SyncResult r = { .sock = NULL, .status = AXL_ERR, .done = false, .loop = loop };

    /* Save and restore the sock's loop association across the
       ephemeral wrapper loop. axl_tcp_send_async overwrites
       sock->async_loop with the ephemeral loop; if the caller had
       previously parked the sock on its own outer loop (e.g. a
       server's main loop holding an armed accept/recv on the same
       sock), we must put the original pointer back so subsequent
       axl_tcp_close on this sock can finalize on that loop instead
       of falling back to the bounded sync wait. */
    AxlLoop *saved_loop = sock->async_loop;

    if (axl_tcp_send_async(sock, data, size, loop, cancel,
                           on_sync_complete, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        sock->async_loop = saved_loop;
        return AXL_ERR;
    }

    axl_loop_add_timeout(loop, timeout_ms, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);
    sock->async_loop = saved_loop;

    return r.status;
}

// ---------------------------------------------------------------------------
// axl_tcp_recv
// ---------------------------------------------------------------------------

int
axl_tcp_recv(AxlTcp *sock, void *buf, size_t *size, size_t timeout_ms)
{
    if (sock == NULL || buf == NULL || size == NULL || *size == 0) {
        return AXL_ERR;
    }

    if (timeout_ms == 0) {
        timeout_ms = 10000;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        *size = 0;
        return AXL_ERR;
    }

    SyncResult r = { .sock = NULL, .status = AXL_ERR, .done = false, .loop = loop };

    /* See axl_tcp_send for the save/restore rationale. */
    AxlLoop *saved_loop = sock->async_loop;

    if (axl_tcp_recv_async(sock, buf, *size, loop, cancel,
                           on_sync_complete, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        sock->async_loop = saved_loop;
        *size = 0;
        return AXL_ERR;
    }

    axl_loop_add_timeout(loop, timeout_ms, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);
    sock->async_loop = saved_loop;

    if (r.status == AXL_OK) {
        *size = axl_tcp_recv_get_size(sock);
    } else {
        *size = 0;
    }
    return r.status;
}

// ---------------------------------------------------------------------------
// axl_tcp_poll
// ---------------------------------------------------------------------------

int
axl_tcp_poll(AxlTcp *sock)
{
    if (sock == NULL || sock->tcp4 == NULL) {
        return AXL_ERR;
    }

    return EFI_ERROR(axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4)) ? AXL_ERR : AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_tcp_close
//
// Heap-owned, async-finalize close.
//
// EDK2 parks `&close_token` in Sock->CloseToken and dereferences it
// when SockConnClosed signals (post-TIME_WAIT for active close, ~2 s
// later; immediately for passive close). Earlier shapes either
// stack-allocated the token (UAF on TIME_WAIT overrun — the Reg A
// hang) or heap-allocated but waited synchronously (pegged CPU for
// ~2 s on every active close and dropped the queued FIN if the
// bounded wait expired before the firmware was done).
//
// This shape splits cleanup into two phases:
//
//   PHASE 1 (sync, this function):
//     * Cancel each pending op token, remove its loop source, and
//       close its per-op event handle. Same sequence the original
//       sync close did — firmware sees Cancel before CloseEvent so
//       no completion attempt lands on a freed handle.
//     * Submit Close() (AbortOnClose=false) with a heap-allocated
//       close_token so it outlives this call.
//     * Either register close_event on the caller's running loop
//       and return immediately, OR fall back to a bounded sync wait
//       if no loop is running (shutdown after axl_loop_run returned).
//
//   PHASE 2 (finalize_close_ctx, fired from on_close_event):
//     * Configure(NULL) — safe now because the close has fully
//       completed, so TcpFlushPcb's NetbufQueFlush has nothing left
//       to drop (vs. the old shape that dropped FIN if the bounded
//       wait expired early).
//     * DestroyChild on the service binding.
//     * Free sock + close_event + ctx.
//
// The async path leaves up to one loop source per outstanding close
// for ~TIME_WAIT seconds; AXL_MAX_SOURCES = 64 has the headroom for
// an http-server bursting at curl-storm rates.
// ---------------------------------------------------------------------------

static void
finalize_sock(AxlTcp *sock)
{
    if (sock->tcp4 != NULL) {
        axl_efi_call(sock->tcp4->Configure, 2, sock->tcp4, NULL);
    }
    if (sock->tcp_sb != NULL && sock->tcp_handle != NULL) {
        axl_efi_call(sock->tcp_sb->DestroyChild, 2, sock->tcp_sb,
                     sock->tcp_handle);
    }
    axl_free(sock);
}

static void
finalize_close_ctx(AxlTcpCloseCtx *ctx)
{
    finalize_sock(ctx->sock);
    if (ctx->close_token.CompletionToken.Event != NULL) {
        axl_backend_event_close(
            (AxlEventHandle)ctx->close_token.CompletionToken.Event);
    }
    axl_free(ctx);
}

static bool
on_close_event(void *data)
{
    AxlTcpCloseCtx *ctx = (AxlTcpCloseCtx *)data;
    axl_loop_remove_source(ctx->loop, ctx->source_id);
    finalize_close_ctx(ctx);
    return AXL_SOURCE_REMOVE;
}

void
axl_tcp_close(AxlTcp *sock)
{
    if (sock == NULL) {
        return;
    }

    //
    // PHASE 1: cancel pending ops, remove their loop sources, close
    // per-op event handles. The Cancel-then-CloseEvent sequence on
    // each token matches the firmware's expectation: Cancel removes
    // the token from internal queues so no further completion attempts
    // happen, then CloseEvent releases the handle. The original sync
    // close did this same thing — we keep it to avoid the long
    // window between Close() and finalize where a stale event handle
    // could be referenced (faults in CoreCheckEvent).
    //
    if (sock->accept_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->accept_source);
        sock->accept_source = 0;
    }
    if (sock->accept_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->accept_cancel_source);
        sock->accept_cancel_source = 0;
    }
    if (sock->tcp4 != NULL && sock->acc_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->acc_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->acc_token.CompletionToken.Event);
        sock->acc_token.CompletionToken.Event = NULL;
    }

    if (sock->recv_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->recv_source);
        sock->recv_source = 0;
    }
    if (sock->recv_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->recv_cancel_source);
        sock->recv_cancel_source = 0;
    }
    if (sock->tcp4 != NULL && sock->rx_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->rx_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->rx_token.CompletionToken.Event);
        sock->rx_token.CompletionToken.Event = NULL;
    }

    if (sock->send_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->send_source);
        sock->send_source = 0;
    }
    if (sock->send_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->send_cancel_source);
        sock->send_cancel_source = 0;
    }
    if (sock->tcp4 != NULL && sock->tx_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->tx_token.CompletionToken.Event);
        sock->tx_token.CompletionToken.Event = NULL;
    }

    if (sock->connect_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->connect_source);
        sock->connect_source = 0;
    }
    if (sock->connect_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->connect_cancel_source);
        sock->connect_cancel_source = 0;
    }
    if (sock->tcp4 != NULL && sock->conn_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->conn_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->conn_token.CompletionToken.Event);
        sock->conn_token.CompletionToken.Event = NULL;
    }
    if (sock->tcp4 == NULL) {
        /* No TCP4 protocol bound (e.g. CreateChild succeeded but
           Configure failed before any op was submitted). The Cancel
           + CloseEvent blocks above are tcp4-guarded, so any cached
           per-op event handles weren't released. Close them here so
           the unconfigured-sock path doesn't leak. */
        if (sock->acc_token.CompletionToken.Event != NULL) {
            axl_backend_event_close(
                (AxlEventHandle)sock->acc_token.CompletionToken.Event);
            sock->acc_token.CompletionToken.Event = NULL;
        }
        if (sock->rx_token.CompletionToken.Event != NULL) {
            axl_backend_event_close(
                (AxlEventHandle)sock->rx_token.CompletionToken.Event);
            sock->rx_token.CompletionToken.Event = NULL;
        }
        if (sock->tx_token.CompletionToken.Event != NULL) {
            axl_backend_event_close(
                (AxlEventHandle)sock->tx_token.CompletionToken.Event);
            sock->tx_token.CompletionToken.Event = NULL;
        }
        if (sock->conn_token.CompletionToken.Event != NULL) {
            axl_backend_event_close(
                (AxlEventHandle)sock->conn_token.CompletionToken.Event);
            sock->conn_token.CompletionToken.Event = NULL;
        }
        finalize_sock(sock);
        return;
    }

    //
    // Heap-allocate the close context so the token outlives this call.
    // EDK2 parks &ctx->close_token in Sock->CloseToken and dereferences
    // it when SockConnClosed runs.
    //
    AxlTcpCloseCtx *ctx = axl_calloc(1, sizeof(*ctx));
    EFI_EVENT       close_event = NULL;
    if (ctx == NULL ||
        axl_backend_event_create((AxlEventHandle *)&close_event) != AXL_OK)
    {
        axl_warning("close: ctx/event alloc failed - abrupt teardown");
        if (ctx != NULL) {
            axl_free(ctx);
        }
        if (close_event != NULL) {
            axl_backend_event_close((AxlEventHandle)close_event);
        }
        /* Best-effort: skip Close() entirely. DestroyChild will tear
           down the firmware state without a graceful FIN. */
        finalize_sock(sock);
        return;
    }

    ctx->sock                              = sock;
    ctx->close_token.CompletionToken.Event = close_event;
    ctx->close_token.AbortOnClose          = false;

    EFI_STATUS status = axl_efi_call(sock->tcp4->Close, 2,
                                     sock->tcp4, &ctx->close_token);
    if (EFI_ERROR(status)) {
        axl_debug("close: Close() returned %llx - abrupt teardown",
                  (unsigned long long)status);
        finalize_close_ctx(ctx);
        return;
    }

    /* Async path: register close_event on the caller's running loop
       and return. on_close_event runs finalize_close_ctx when the
       firmware signals SockConnClosed (post-TIME_WAIT for active
       close), with no synchronous wait or CPU spin. */
    if (sock->async_loop != NULL && axl_loop_is_running(sock->async_loop)) {
        ctx->loop      = sock->async_loop;
        ctx->source_id = axl_loop_add_event(
            ctx->loop,
            (AxlEventHandle)close_event,
            on_close_event,
            ctx
            );
        if (ctx->source_id != 0) {
            return;
        }
        axl_warning("close: cannot register close event on loop - sync fallback");
    }

    /* Sync fallback (loop not running, or registration failed). 3 s
       covers TIME_WAIT for active close; passive close signals
       immediately so the wait usually returns much sooner. */
    (void)_axl_tcp_wait(sock->tcp4, close_event, 3000ULL * 1000ULL);
    finalize_close_ctx(ctx);
}

// ---------------------------------------------------------------------------
// axl_tcp_get_local_addr / axl_tcp_get_remote_addr / axl_tcp_recv_get_size
// ---------------------------------------------------------------------------

static EFI_STATUS
get_mode_addr(
    AxlTcp  *sock,
    char    *addr_buf,
    size_t   buf_size,
    uint16_t *port_out,
    bool     local
    )
{
    EFI_STATUS              status;
    EFI_TCP4_CONFIG_DATA    tcp_config;
    EFI_IPv4_ADDRESS        *addr;

    if (sock == NULL || sock->tcp4 == NULL || addr_buf == NULL || port_out == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    axl_memset(&tcp_config, 0, sizeof(tcp_config));
    status = axl_efi_call(sock->tcp4->GetModeData, 6,
                            sock->tcp4,
                            NULL,
                            &tcp_config,
                            NULL,
                            NULL,
                            NULL
                            );
    if (EFI_ERROR(status)) {
        return status;
    }

    if (local) {
        addr = (EFI_IPv4_ADDRESS *)&tcp_config.AccessPoint.StationAddress;
        *port_out = tcp_config.AccessPoint.StationPort;
    } else {
        addr = (EFI_IPv4_ADDRESS *)&tcp_config.AccessPoint.RemoteAddress;
        *port_out = tcp_config.AccessPoint.RemotePort;
    }

    axl_snprintf(addr_buf, buf_size, "%d.%d.%d.%d",
        addr->Addr[0], addr->Addr[1], addr->Addr[2], addr->Addr[3]);

    return EFI_SUCCESS;
}

int
axl_tcp_get_local_addr(AxlTcp *sock, char *addr, size_t size,
                       uint16_t *out_port)
{
    return EFI_ERROR(get_mode_addr(sock, addr, size, out_port, true)) ? AXL_ERR : AXL_OK;
}

int
axl_tcp_get_remote_addr(AxlTcp *sock, char *addr, size_t size,
                        uint16_t *out_port)
{
    return EFI_ERROR(get_mode_addr(sock, addr, size, out_port, false)) ? AXL_ERR : AXL_OK;
}

size_t
axl_tcp_recv_get_size(AxlTcp *sock)
{
    if (sock == NULL) {
        return 0;
    }
    return sock->recv_size;
}
