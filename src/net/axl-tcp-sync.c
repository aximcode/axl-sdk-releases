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
    AxlTcp  *sock;
    int      status;
    bool     done;
    AxlLoop *loop;
} SyncResult;

// ---------------------------------------------------------------------------
// Locate TCP4 service binding
// ---------------------------------------------------------------------------

EFI_STATUS
tcp_find_service_binding(
    EFI_SERVICE_BINDING_PROTOCOL  **sb,
    EFI_HANDLE                    *out_handle
    )
{
    EFI_STATUS  status;
    EFI_HANDLE  *handles;
    size_t       handle_count;

    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiTcp4ServiceBindingProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles
                    );
    if (EFI_ERROR(status) || handle_count == 0) {
        return EFI_NOT_FOUND;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    handles[0],
                    &gEfiTcp4ServiceBindingProtocolGuid,
                    (void **)sb
                    );
    if (!EFI_ERROR(status)) {
        *out_handle = handles[0];
    }

    axl_backend_free(handles);
    return status;
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
on_sync_complete(AxlTcp *sock, int status, void *data)
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
axl_tcp_connect(const char *host, uint16_t port, AxlTcp **out_sock)
{
    if (host == NULL || out_sock == NULL) {
        return -1;
    }

    AxlLoop        *loop   = axl_loop_new();
    if (loop == NULL) {
        return -1;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return -1;
    }

    SyncResult r = { .sock = NULL, .status = -1, .done = false, .loop = loop };

    if (axl_tcp_connect_async(host, port, loop, cancel,
                              on_sync_complete, &r) != 0) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return -1;
    }

    axl_loop_add_timeout(loop, 10000, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    /* On cancel/error the async op fires cb with NULL sock and closes
       the partial socket internally. Only hand back on clean success. */
    if (r.status == 0) {
        *out_sock = r.sock;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// axl_tcp_listen
// ---------------------------------------------------------------------------

int
axl_tcp_listen(uint16_t port, AxlTcp **out_listener)
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
        return -1;
    }

    status = tcp_find_service_binding(&sb, &sb_handle);
    if (EFI_ERROR(status)) {
        axl_error("no TCP4 service binding");
        return -1;
    }

    status = tcp_create_child(sb, &child_handle, &tcp4);
    if (EFI_ERROR(status)) {
        axl_error("TCP4 CreateChild: %llx", (unsigned long long)status);
        return -1;
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
        return -1;
    }

    sock = axl_calloc(1, sizeof(AxlTcp));
    if (sock == NULL) {
        axl_efi_call(tcp4->Configure, 2, tcp4, NULL);
        axl_efi_call(sb->DestroyChild, 2, sb, child_handle);
        return -1;
    }

    sock->tcp4       = tcp4;
    sock->tcp_handle = child_handle;
    sock->tcp_sb     = sb;
    sock->sb_handle  = sb_handle;
    sock->is_listener = true;

    *out_listener = sock;
    axl_info("listening on port %u", port);
    return 0;
}

// ---------------------------------------------------------------------------
// axl_tcp_accept
// ---------------------------------------------------------------------------

int
axl_tcp_accept(AxlTcp *listener, AxlTcp **out_client, size_t timeout_ms)
{
    if (listener == NULL || out_client == NULL || !listener->is_listener) {
        return -1;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return -1;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return -1;
    }

    SyncResult r = { .sock = NULL, .status = -1, .done = false, .loop = loop };

    if (axl_tcp_accept_async(listener, loop, cancel,
                             on_sync_complete, &r) != 0) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return -1;
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

    if (r.status == 0) {
        *out_client = r.sock;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// axl_tcp_send
// ---------------------------------------------------------------------------

int
axl_tcp_send(AxlTcp *sock, const void *data, size_t size, size_t timeout_ms)
{
    if (sock == NULL || data == NULL || size == 0) {
        return -1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 10000;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return -1;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return -1;
    }

    SyncResult r = { .sock = NULL, .status = -1, .done = false, .loop = loop };

    if (axl_tcp_send_async(sock, data, size, loop, cancel,
                           on_sync_complete, &r) != 0) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return -1;
    }

    axl_loop_add_timeout(loop, timeout_ms, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    return r.status;
}

// ---------------------------------------------------------------------------
// axl_tcp_recv
// ---------------------------------------------------------------------------

int
axl_tcp_recv(AxlTcp *sock, void *buf, size_t *size, size_t timeout_ms)
{
    if (sock == NULL || buf == NULL || size == NULL || *size == 0) {
        return -1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 10000;
    }

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return -1;
    }

    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        *size = 0;
        return -1;
    }

    SyncResult r = { .sock = NULL, .status = -1, .done = false, .loop = loop };

    if (axl_tcp_recv_async(sock, buf, *size, loop, cancel,
                           on_sync_complete, &r) != 0) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        *size = 0;
        return -1;
    }

    axl_loop_add_timeout(loop, timeout_ms, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    if (r.status == 0) {
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
        return -1;
    }

    return EFI_ERROR(axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4)) ? -1 : 0;
}

// ---------------------------------------------------------------------------
// axl_tcp_close
// ---------------------------------------------------------------------------

void
axl_tcp_close(AxlTcp *sock)
{
    if (sock == NULL) {
        return;
    }

    //
    // Clean up each async op independently. Source removal (loop-side)
    // and token event close (UEFI-side) are decoupled — earlier code
    // guarded the event close on `source > 0`, so if add_event failed
    // between token creation and source registration the event would
    // leak. Each Cancel+CloseEvent now runs whenever Event != NULL,
    // regardless of the source state. Cancel on a completed token is
    // a documented no-op (EFI_NOT_FOUND) so this is safe for ops that
    // completed cleanly and already cleared their own event.
    //
    if (sock->accept_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->accept_source);
        sock->accept_source = 0;
    }
    if (sock->accept_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->accept_cancel_source);
        sock->accept_cancel_source = 0;
    }
    if (sock->acc_token.CompletionToken.Event != NULL) {
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
    if (sock->rx_token.CompletionToken.Event != NULL) {
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
    if (sock->tx_token.CompletionToken.Event != NULL) {
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
    if (sock->conn_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->conn_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->conn_token.CompletionToken.Event);
        sock->conn_token.CompletionToken.Event = NULL;
    }

    if (sock->tcp4 != NULL) {
        //
        // Try graceful close
        //
        EFI_EVENT  close_event = NULL;
        if (axl_backend_event_create((AxlEventHandle *)&close_event) == 0) {
            EFI_TCP4_CLOSE_TOKEN  close_token;
            axl_memset(&close_token, 0, sizeof(close_token));
            close_token.CompletionToken.Event = close_event;
            close_token.AbortOnClose          = false;

            EFI_STATUS  status = axl_efi_call(sock->tcp4->Close, 2,
                                              sock->tcp4, &close_token);
            if (!EFI_ERROR(status)) {
                (void)_axl_tcp_wait(sock->tcp4, close_event, 500 * 1000);
            }

            axl_backend_event_close((AxlEventHandle)close_event);
        }

        axl_efi_call(sock->tcp4->Configure, 2, sock->tcp4, NULL);
    }

    if (sock->tcp_sb != NULL && sock->tcp_handle != NULL) {
        axl_efi_call(sock->tcp_sb->DestroyChild, 2, sock->tcp_sb, sock->tcp_handle);
    }

    axl_free(sock);
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
    return EFI_ERROR(get_mode_addr(sock, addr, size, out_port, true)) ? -1 : 0;
}

int
axl_tcp_get_remote_addr(AxlTcp *sock, char *addr, size_t size,
                        uint16_t *out_port)
{
    return EFI_ERROR(get_mode_addr(sock, addr, size, out_port, false)) ? -1 : 0;
}

size_t
axl_tcp_recv_get_size(AxlTcp *sock)
{
    if (sock == NULL) {
        return 0;
    }
    return sock->recv_size;
}
