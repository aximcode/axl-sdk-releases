/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tcp-async.c
    AxlTcp — event-driven (non-blocking) TCP API over EFI_TCP4_PROTOCOL.

    All four _async entry points follow the same pattern: install (or
    reuse) a completion-token event on the EFI TCP4 child, submit the
    operation, register the event with the caller's AxlLoop, and fire
    the user callback from the event handler. When an AxlCancellable
    is supplied, a second loop source is registered on the
    cancellable's event — on signal, the op's on_*_cancel handler
    tears everything down via the shared axl_tcp_*_drop_sources helper
    and fires the user callback with status=AXL_CANCELLED. The sync
    wrappers in axl-tcp-sync.c sit on top of these.

    Contract: on_*_complete and on_*_cancel BOTH route through the
    same _drop_sources helper so completion- and cancel-source
    teardown are symmetric — a slip in one path does not leave the
    loop with an orphaned source.

    Callback return value: user callbacks return bool. For accept/recv
    the value controls re-arming (true = keep armed, false = stop).
    Connect and send are inherently one-shot so completion handlers
    discard the return with (void).
**/

#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-wait.h>

#include "axl-tcp-internal.h"
#include "../event/axl-cancellable-internal.h"

AXL_LOG_DOMAIN("tcp");

// ---- accept async ----------------------------------------------------------

static bool
on_accept_complete(void *data)
{
    AxlTcp             *listener = data;
    EFI_STATUS          status;
    EFI_HANDLE          new_handle;
    EFI_TCP4_PROTOCOL  *new_tcp4;
    AxlTcp             *sock;
    AxlTcpCallback      cb;
    void               *cb_data;
    /* Snapshot IDs so we can drop the loop sources without reading
       listener fields after cb — if cb returns false, it may have
       called axl_tcp_close on the listener (freeing it). */
    AxlLoop            *saved_loop       = listener->async_loop;
    uint32_t            saved_accept_src = listener->accept_source;
    uint32_t            saved_cancel_src = listener->accept_cancel_source;

    axl_efi_call(listener->tcp4->Poll, 1, listener->tcp4);

    status = listener->acc_token.CompletionToken.Status;

    //
    // Save callback before invoking — the callback may close the
    // listener (freeing L).
    //
    cb = listener->on_accept;
    cb_data = listener->accept_data;

    if (EFI_ERROR(status)) {
        axl_error("async accept failed: %llx", (unsigned long long)status);
        /* Tear down ALL accept state (sources + event) so a retry via
           axl_tcp_accept_async starts cleanly — prior code only removed
           accept_source, leaving accept_cancel_source registered
           pointing at stale callback data and leaking the acc event
           on close(). */
        axl_tcp_accept_drop_sources(listener);
        (void)cb(NULL, AXL_ERR, cb_data);  /* fundamental accept failure — tearing down regardless */
        return AXL_SOURCE_REMOVE;
    }

    new_handle = listener->acc_token.NewChildHandle;
    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    new_handle,
                    &gEfiTcp4ProtocolGuid,
                    (void **)&new_tcp4
                    );
    if (EFI_ERROR(status)) {
        axl_error("async accept HandleProtocol: %llx", (unsigned long long)status);
        axl_efi_call(listener->tcp_sb->DestroyChild, 2, listener->tcp_sb, new_handle);
        /* Library-internal per-accept failure. Notify the user, then
           unconditionally re-arm — the listener state is still valid
           for the next connection and tearing down the UEFI token
           here would double-free if the user closed the listener in
           response. Match pre-bool behavior for these paths. */
        (void)cb(NULL, AXL_ERR, cb_data);
        goto rearm;
    }

    sock = axl_calloc(1, sizeof(AxlTcp));
    if (sock == NULL) {
        axl_efi_call(new_tcp4->Configure, 2, new_tcp4, NULL);
        axl_efi_call(listener->tcp_sb->DestroyChild, 2, listener->tcp_sb, new_handle);
        /* Same reasoning as above — treat as transient. */
        (void)cb(NULL, AXL_ERR, cb_data);
        goto rearm;
    }

    sock->tcp4       = new_tcp4;
    sock->tcp_handle = new_handle;
    sock->tcp_sb     = listener->tcp_sb;
    sock->sb_handle  = listener->sb_handle;
    sock->is_listener = false;

    //
    // Call user callback. Return value controls re-arming: true keeps
    // the listener armed, false stops accepting.
    //
    // Contract:
    //   - Return true  => cb did NOT close the listener; we access
    //                     listener below to re-issue Accept.
    //   - Return false => cb may have called axl_tcp_close on the
    //                     listener; we only touch the saved source
    //                     IDs below, never listener.
    //
    if (!cb(sock, AXL_OK, cb_data)) {
        if (saved_accept_src > 0) {
            axl_loop_remove_source(saved_loop, saved_accept_src);
        }
        if (saved_cancel_src > 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }

rearm:
    {
        EFI_EVENT  ev = listener->acc_token.CompletionToken.Event;
        axl_memset(&listener->acc_token, 0, sizeof(listener->acc_token));
        listener->acc_token.CompletionToken.Event  = ev;
        listener->acc_token.CompletionToken.Status = EFI_ABORTED;

        status = axl_efi_call(listener->tcp4->Accept, 2,
                              listener->tcp4, &listener->acc_token);
        if (EFI_ERROR(status)) {
            axl_error("async accept re-arm failed: %llx", (unsigned long long)status);
        }
    }

    return AXL_SOURCE_CONTINUE;
}

/*
 * Stop an armed accept. The listener itself stays valid — caller can
 * accept again later. Also called from axl_tcp_close and from the
 * sync axl_tcp_accept wrapper; see axl-tcp-internal.h.
 */
void
axl_tcp_accept_drop_sources(AxlTcp *listener)
{
    if (listener->accept_source > 0) {
        axl_loop_remove_source(listener->async_loop,
                               listener->accept_source);
        listener->accept_source = 0;
        axl_efi_call(listener->tcp4->Cancel, 2, listener->tcp4,
                     &listener->acc_token.CompletionToken);
    }
    if (listener->accept_cancel_source > 0) {
        axl_loop_remove_source(listener->async_loop,
                               listener->accept_cancel_source);
        listener->accept_cancel_source = 0;
    }
    if (listener->acc_token.CompletionToken.Event != NULL) {
        axl_backend_event_close(
            (AxlEventHandle)listener->acc_token.CompletionToken.Event);
        listener->acc_token.CompletionToken.Event = NULL;
    }
}

static bool
on_accept_cancel(void *data)
{
    AxlTcp         *listener = data;
    AxlTcpCallback  cb       = listener->on_accept;
    void           *udata    = listener->accept_data;

    axl_tcp_accept_drop_sources(listener);
    (void)cb(NULL, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_accept_async(
    AxlTcp         *listener,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlTcpCallback  cb,
    void           *data
    )
{
    EFI_STATUS  status;
    EFI_EVENT   acc_event;

    if (listener == NULL || loop == NULL || cb == NULL || !listener->is_listener) {
        return AXL_ERR;
    }

    //
    // Cancel any pending accept before starting a new one. Mirrors
    // the recv/send preempt path. Caller is changing the callback or
    // resuming after a return-false; either way we want the UEFI op
    // and loop source reset to a clean state.
    //
    if (listener->accept_source > 0) {
        axl_loop_remove_source(listener->async_loop, listener->accept_source);
        listener->accept_source = 0;
        axl_efi_call(listener->tcp4->Cancel, 2, listener->tcp4,
                     &listener->acc_token.CompletionToken);
    }
    if (listener->accept_cancel_source > 0) {
        axl_loop_remove_source(listener->async_loop,
                               listener->accept_cancel_source);
        listener->accept_cancel_source = 0;
    }

    listener->on_accept   = cb;
    listener->accept_data = data;
    listener->async_loop  = loop;

    //
    // Create the accept event (once, reused across re-arms)
    //
    if (listener->acc_token.CompletionToken.Event == NULL) {
        acc_event = NULL;
        if (axl_backend_event_create((AxlEventHandle *)&acc_event) != AXL_OK) {
            axl_error("async accept: cannot create event");
            return AXL_ERR;
        }

        listener->acc_token.CompletionToken.Event = acc_event;
    }

    listener->acc_token.CompletionToken.Status = EFI_ABORTED;
    listener->acc_token.NewChildHandle         = NULL;

    status = axl_efi_call(listener->tcp4->Accept, 2, listener->tcp4,
                           &listener->acc_token);
    if (EFI_ERROR(status)) {
        axl_error("async Accept: %llx", (unsigned long long)status);
        axl_backend_event_close(
            (AxlEventHandle)listener->acc_token.CompletionToken.Event);
        listener->acc_token.CompletionToken.Event = NULL;
        return AXL_ERR;
    }

    listener->accept_source = axl_loop_add_event(
        loop,
        (void *)listener->acc_token.CompletionToken.Event,
        on_accept_complete,
        listener
        );
    if (listener->accept_source == 0) {
        axl_error("async accept: cannot register event with loop");
        axl_efi_call(listener->tcp4->Cancel, 2, listener->tcp4,
                     &listener->acc_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)listener->acc_token.CompletionToken.Event);
        listener->acc_token.CompletionToken.Event = NULL;
        return AXL_ERR;
    }

    if (cancel != NULL) {
        listener->accept_cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_accept_cancel,
            listener
            );
    }

    return AXL_OK;
}

// ---- recv async ------------------------------------------------------------

static bool
on_recv_complete(void *data)
{
    AxlTcp         *sock = data;
    EFI_STATUS      status;
    AxlTcpCallback  cb;
    void           *cb_data;
    AxlStatus       cb_status;
    bool            keep;
    /* Snapshot IDs so we can drop the loop sources without reading
       sock fields after cb — if cb returns false, it may have called
       axl_tcp_close on sock (freeing it). Contract for true: cb must
       NOT close sock. */
    AxlLoop        *saved_loop       = sock->async_loop;
    uint32_t        saved_recv_src   = sock->recv_source;
    uint32_t        saved_cancel_src = sock->recv_cancel_source;

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    status = sock->rx_token.CompletionToken.Status;
    if (EFI_ERROR(status)) {
        sock->recv_size = 0;
        cb_status = AXL_ERR;
    } else {
        sock->recv_size = (size_t)sock->rx_data.DataLength;
        cb_status = AXL_OK;
    }

    cb = sock->on_recv;
    cb_data = sock->recv_data;

    //
    // Call user callback. Return value controls re-arming: true
    // re-issues Receive with the same buffer, false tears down.
    //
    // Contract:
    //   - Return true  => cb did NOT close sock; we access sock below
    //                     to re-issue Receive.
    //   - Return false => cb may have called axl_tcp_close; we only
    //                     touch the saved source IDs below, never sock.
    //
    // Only re-arm on cb_status == AXL_OK. Errors (-1) and AXL_CANCELLED
    // never re-arm regardless of cb return — the UEFI recv op is
    // already torn down on those paths.
    //
    keep = cb(sock, cb_status, cb_data);

    if (!keep || cb_status != AXL_OK) {
        /* sock may be freed — only touch saved IDs. If cb called
           axl_tcp_close, axl_tcp_close has already removed the source
           via sock->recv_source; axl_loop_remove_source on a
           non-existent ID is a harmless no-op. */
        if (saved_recv_src > 0) {
            axl_loop_remove_source(saved_loop, saved_recv_src);
        }
        if (saved_cancel_src > 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }

    //
    // Re-arm: reset token and re-issue Receive into the same buffer.
    // The loop source stays registered; the rx event will fire again
    // when new data arrives. Uses recv_capacity (original buffer
    // size) — recv_size was just overwritten with bytes-received.
    //
    axl_memset(&sock->rx_data, 0, sizeof(sock->rx_data));
    sock->rx_data.UrgentFlag                    = false;
    sock->rx_data.DataLength                    = (uint32_t)sock->recv_capacity;
    sock->rx_data.FragmentCount                 = 1;
    sock->rx_data.FragmentTable[0].FragmentLength = (uint32_t)sock->recv_capacity;
    sock->rx_data.FragmentTable[0].FragmentBuffer = sock->recv_buf;

    sock->rx_token.CompletionToken.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData          = &sock->rx_data;

    status = axl_efi_call(sock->tcp4->Receive, 2, sock->tcp4, &sock->rx_token);
    if (EFI_ERROR(status)) {
        /* Same race as the one axl_tcp_recv_async handles — peer FIN
         * arrived between the previous recv completing and this re-arm,
         * so SockNoMoreData ran with an empty token list and the next
         * Receive() returns EFI_CONNECTION_FIN synchronously. We CAN'T
         * drop the source and return AXL_SOURCE_REMOVE: that would
         * silently swallow the EOF. The recv_source is still
         * registered with the loop and the cached event is still live;
         * signal it ourselves so the loop dispatches us again on the
         * next tick with rx_token.Status == EFI_ABORTED, which our
         * own error branch above translates to cb_status=-1.
         * Caller sees the EOF and can axl_tcp_close cleanly. */
        axl_debug("recv re-arm Receive: %llx", (unsigned long long)status);
        axl_backend_event_signal(
            (AxlEventHandle)sock->rx_token.CompletionToken.Event);
    }

    return AXL_SOURCE_CONTINUE;
}

/*
 * Cancel an armed recv. Fires the user callback with AXL_CANCELLED
 * but does NOT close the sock — sock is a connected socket; caller
 * may want to start a fresh recv or close it themselves.
 */
void
axl_tcp_recv_drop_sources(AxlTcp *sock)
{
    if (sock->recv_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->recv_source);
        sock->recv_source = 0;
    }
    if (sock->recv_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop,
                               sock->recv_cancel_source);
        sock->recv_cancel_source = 0;
    }
}

static bool
on_recv_cancel(void *data)
{
    AxlTcp         *sock  = data;
    AxlTcpCallback  cb    = sock->on_recv;
    void           *udata = sock->recv_data;

    axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                 &sock->rx_token.CompletionToken);
    axl_tcp_recv_drop_sources(sock);
    (void)cb(sock, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_recv_async(
    AxlTcp         *sock,
    void           *buf,
    size_t          size,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlTcpCallback  cb,
    void           *data
    )
{
    EFI_STATUS  status;
    EFI_EVENT   rx_event;

    if (sock == NULL || buf == NULL || size == 0 || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    //
    // Cancel any pending recv before starting a new one
    //
    if (sock->recv_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->recv_source);
        sock->recv_source = 0;
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->rx_token.CompletionToken);
    }

    sock->on_recv       = cb;
    sock->recv_data     = data;
    sock->async_loop    = loop;
    sock->recv_buf      = buf;
    sock->recv_capacity = size;
    sock->recv_size     = size;  /* rewritten to bytes-received by on_recv_complete */

    //
    // Create the recv event if not already created. Some teardown
    // paths leave the cached handle pointing at a closed/freed
    // event (a stale handle survives across the in-handler sync
    // axl_tcp_send + ephemeral-loop teardown that the WS upgrade and
    // every send_response use). Detect via CheckEvent and recreate
    // — otherwise the next Receive succeeds against an invalid event
    // and the loop never wakes for incoming data.
    //
    if (sock->rx_token.CompletionToken.Event != NULL &&
        axl_backend_event_check(sock->rx_token.CompletionToken.Event) == -1) {
        sock->rx_token.CompletionToken.Event = NULL;
    }
    if (sock->rx_token.CompletionToken.Event == NULL) {
        rx_event = NULL;
        if (axl_backend_event_create((AxlEventHandle *)&rx_event) != AXL_OK) {
            axl_error("async recv: cannot create event");
            return AXL_ERR;
        }

        sock->rx_token.CompletionToken.Event = rx_event;
    }

    //
    // Set up rx_data with fragment pointing to user buffer
    //
    axl_memset(&sock->rx_data, 0, sizeof(sock->rx_data));
    sock->rx_data.UrgentFlag                    = false;
    sock->rx_data.DataLength                    = (uint32_t)size;
    sock->rx_data.FragmentCount                 = 1;
    sock->rx_data.FragmentTable[0].FragmentLength = (uint32_t)size;
    sock->rx_data.FragmentTable[0].FragmentBuffer = buf;

    sock->rx_token.CompletionToken.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData          = &sock->rx_data;

    status = axl_efi_call(sock->tcp4->Receive, 2, sock->tcp4, &sock->rx_token);

    /* Synchronous-failure path: most commonly EFI_CONNECTION_FIN if the
     * peer FIN'd between the previous recv completing and this re-arm
     * (the firmware processes the FIN via SockNoMoreData when the recv
     * token list is empty, so subsequent Receive() returns
     * EFI_CONNECTION_FIN immediately — see SockRcv in
     * NetworkPkg/TcpDxe/SockInterface.c). Without delivery to the
     * user, the server never observes the EOF, never closes, and
     * never sends its FIN — peer hangs in FIN-WAIT-2 forever.
     *
     * Register the source first, then signal the cached event so the
     * loop dispatches on_recv_complete on the next tick. The token's
     * Status field stays at the EFI_ABORTED we set above (Receive
     * doesn't update it on synchronous error), so on_recv_complete
     * sees EFI_ERROR and translates to cb_status=-1 — exactly what
     * the user's callback expects for "peer closed". This preserves
     * the "callback always fires from loop dispatch" contract.
     */
    sock->recv_source = axl_loop_add_event(
        loop,
        (void *)sock->rx_token.CompletionToken.Event,
        on_recv_complete,
        sock
        );
    if (sock->recv_source == 0) {
        axl_error("async recv: cannot register event with loop");
        if (!EFI_ERROR(status)) {
            axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                         &sock->rx_token.CompletionToken);
        }
        /* Drain any latent signal off the cached event so a later
           recv_async on this sock doesn't fire a stale completion.
           CheckEvent consumes the signaled bit when set. */
        axl_backend_event_check(sock->rx_token.CompletionToken.Event);
        return AXL_ERR;
    }

    if (cancel != NULL) {
        sock->recv_cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_recv_cancel,
            sock
            );
    }

    /* If Receive failed synchronously (EFI_CONNECTION_FIN etc.), the
     * firmware will never signal the event. Signal it ourselves so the
     * loop dispatches on_recv_complete with the existing EFI_ABORTED
     * status — see comment on the Receive call above. The most
     * common cause is a peer FIN delivered between the previous
     * recv completing and this re-arm (normal end-of-stream), so
     * trace at debug verbosity rather than warn. */
    if (EFI_ERROR(status)) {
        axl_debug("async Receive sync-completed (peer EOF likely): %llx",
                  (unsigned long long)status);
        axl_backend_event_signal(
            (AxlEventHandle)sock->rx_token.CompletionToken.Event);
    }

    return AXL_OK;
}

// ---- send async ------------------------------------------------------------

static bool
on_send_complete(void *data)
{
    AxlTcp         *sock = data;
    EFI_STATUS      status;
    AxlTcpCallback  cb;
    void           *cb_data;
    AxlStatus       cb_status;

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    status = sock->tx_token.CompletionToken.Status;
    if (EFI_ERROR(status)) {
        axl_error("async send failed: %llx", (unsigned long long)status);
        cb_status = AXL_ERR;
    } else {
        cb_status = AXL_OK;
    }

    //
    // Remove loop source BEFORE calling user callback
    //
    cb = sock->on_send;
    cb_data = sock->send_data;
    /* Drop BOTH completion and cancel sources — same reason as recv. */
    axl_tcp_send_drop_sources(sock);

    (void)cb(sock, cb_status, cb_data);  /* send is one-shot; return value ignored */
    return AXL_SOURCE_REMOVE;
}

/*
 * Cancel an armed send. Same contract as recv cancel — sock stays
 * valid, caller's callback fires with AXL_CANCELLED.
 */
void
axl_tcp_send_drop_sources(AxlTcp *sock)
{
    if (sock->send_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->send_source);
        sock->send_source = 0;
    }
    if (sock->send_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop,
                               sock->send_cancel_source);
        sock->send_cancel_source = 0;
    }
}

static bool
on_send_cancel(void *data)
{
    AxlTcp         *sock  = data;
    AxlTcpCallback  cb    = sock->on_send;
    void           *udata = sock->send_data;

    axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                 &sock->tx_token.CompletionToken);
    axl_tcp_send_drop_sources(sock);
    (void)cb(sock, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_send_async(
    AxlTcp         *sock,
    const void     *buf,
    size_t          size,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlTcpCallback  cb,
    void           *data
    )
{
    EFI_STATUS  status;
    EFI_EVENT   tx_event;

    if (sock == NULL || buf == NULL || size == 0 || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    //
    // Reject preemption: if a send is already pending, the caller
    // must cancel it (via the AxlCancellable passed to the original
    // send, or via axl_tcp_close) and wait for the cancel callback
    // to fire before starting a new send. Firing the old callback
    // inline here was UAF-prone: old_cb could call axl_tcp_close,
    // freeing sock before the subsequent `sock->on_send = cb`
    // assignment.
    //
    if (sock->send_source > 0) {
        axl_error("async send: previous send still pending — cancel first");
        return AXL_ERR;
    }

    sock->on_send    = cb;
    sock->send_data  = data;
    sock->async_loop = loop;

    //
    // Create the send event if not already created
    //
    if (sock->tx_token.CompletionToken.Event == NULL) {
        tx_event = NULL;
        if (axl_backend_event_create((AxlEventHandle *)&tx_event) != AXL_OK) {
            axl_error("async send: cannot create event");
            return AXL_ERR;
        }

        sock->tx_token.CompletionToken.Event = tx_event;
    }

    //
    // Set up tx_frag, tx_data, tx_token
    //
    sock->tx_frag.FragmentLength = (uint32_t)size;
    sock->tx_frag.FragmentBuffer = (void *)buf;

    axl_memset(&sock->tx_data, 0, sizeof(sock->tx_data));
    sock->tx_data.Push            = true;
    sock->tx_data.DataLength      = (uint32_t)size;
    sock->tx_data.FragmentCount   = 1;
    sock->tx_data.FragmentTable[0] = sock->tx_frag;

    sock->tx_token.CompletionToken.Status = EFI_ABORTED;
    sock->tx_token.Packet.TxData          = &sock->tx_data;

    /* Clear any stale signal from a previous synchronous send so the
       loop does not fire the callback before this Transmit completes. */
    axl_backend_event_check(
        (AxlEventHandle)sock->tx_token.CompletionToken.Event);

    status = axl_efi_call(sock->tcp4->Transmit, 2, sock->tcp4, &sock->tx_token);
    if (EFI_ERROR(status)) {
        axl_debug("async Transmit: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    sock->send_source = axl_loop_add_event(
        loop,
        (void *)sock->tx_token.CompletionToken.Event,
        on_send_complete,
        sock
        );
    if (sock->send_source == 0) {
        axl_error("async send: cannot register event with loop");
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        return AXL_ERR;
    }

    if (cancel != NULL) {
        sock->send_cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_send_cancel,
            sock
            );
    }

    return AXL_OK;
}

// ---- connect async ---------------------------------------------------------

/*
 * Symmetric source teardown shared by on_connect_complete and
 * on_connect_cancel. Removes whichever of {connect, cancel} sources
 * are still registered on the async loop and clears the fields. The
 * connect event is also closed here — we're done observing it.
 */
void
axl_tcp_connect_drop_sources(AxlTcp *sock)
{
    if (sock->connect_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->connect_source);
        sock->connect_source = 0;
    }
    if (sock->connect_cancel_source > 0) {
        axl_loop_remove_source(sock->async_loop, sock->connect_cancel_source);
        sock->connect_cancel_source = 0;
    }
    if (sock->conn_token.CompletionToken.Event != NULL) {
        axl_backend_event_close(
            (AxlEventHandle)sock->conn_token.CompletionToken.Event);
        sock->conn_token.CompletionToken.Event = NULL;
    }
}

static bool
on_connect_complete(void *data)
{
    AxlTcp      *sock = data;
    EFI_STATUS   status;

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    status = sock->conn_token.CompletionToken.Status;

    axl_tcp_connect_drop_sources(sock);

    if (EFI_ERROR(status)) {
        axl_error("async connect failed: %llx", (unsigned long long)status);
        AxlTcpCallback  cb   = sock->on_connect;
        void           *udata = sock->connect_data;
        /* Force sync close path. The connect failure on the sync
           wrapper's path means we're called from an ephemeral loop
           that's about to be freed; if we let axl_tcp_close take
           the async path the close_event source would be left
           dangling on the soon-to-be-freed loop. Clearing
           async_loop here makes the close finalize inline. */
        sock->async_loop = NULL;
        axl_tcp_close(sock);
        (void)cb(NULL, AXL_ERR, udata);  /* connect is one-shot; return value ignored */
        return AXL_SOURCE_REMOVE;
    }

    (void)sock->on_connect(sock, 0, sock->connect_data);  /* connect is one-shot */
    return AXL_SOURCE_REMOVE;
}

static bool
on_connect_cancel(void *data)
{
    AxlTcp         *sock  = data;
    AxlTcpCallback  cb    = sock->on_connect;
    void           *udata = sock->connect_data;

    /* Cancel the pending UEFI connect so the driver stops trying. */
    axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                 &sock->conn_token.CompletionToken);

    axl_tcp_connect_drop_sources(sock);

    /* Connect cancel is terminal: the sock was never fully opened, so
       we close it (releasing the tcp4 child + service-binding). Any
       other armed op on this sock gets torn down by axl_tcp_close's
       sweep — safe because the caller only gets sock in the cb on
       connect success, so they can't have started recv/send yet. The
       cb(NULL, AXL_CANCELLED, data) shape matches on_connect_complete's
       error path. Force sync close (see on_connect_complete) — the
       sync wrapper's ephemeral loop frees right after this returns,
       so an async close_event source would be left dangling. */
    sock->async_loop = NULL;
    axl_tcp_close(sock);
    (void)cb(NULL, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_connect_async_via(
    const char            *host,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data
    )
{
    EFI_STATUS                    status;
    EFI_SERVICE_BINDING_PROTOCOL  *sb;
    EFI_HANDLE                    sb_handle;
    EFI_HANDLE                    child_handle;
    EFI_TCP4_PROTOCOL             *tcp4;
    EFI_TCP4_CONFIG_DATA          tcp_config;
    EFI_TCP4_ACCESS_POINT         access_point;
    EFI_EVENT                     conn_event;
    AxlIPv4Address                remote_addr;
    AxlTcp                       *sock;

    if (host == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    //
    // Resolve host to IP address
    //
    if (axl_net_resolve(host, &remote_addr) != AXL_OK) {
        axl_error("async connect: cannot resolve '%s'", host);
        return AXL_ERR;
    }

    //
    // Find TCP4 service binding. Auto-pick prefers an interface whose
    // subnet contains `remote_addr`; explicit `source_ip` (if non-NULL
    // and non-zero) pins to that station address.
    //
    EFI_IPv4_ADDRESS efi_dest;
    axl_memcpy(efi_dest.Addr, remote_addr.addr, 4);

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

    status = tcp_find_service_binding(&efi_dest, forced, &sb, &sb_handle);
    if (EFI_ERROR(status)) {
        if (forced != NULL) {
            axl_error("async connect: no interface with station IP %u.%u.%u.%u",
                source_ip->addr[0], source_ip->addr[1],
                source_ip->addr[2], source_ip->addr[3]);
        } else {
            axl_error("async connect: no TCP4 service binding");
        }
        return AXL_ERR;
    }

    //
    // Create child
    //
    status = tcp_create_child(sb, &child_handle, &tcp4);
    if (EFI_ERROR(status)) {
        axl_error("async connect CreateChild: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    //
    // Configure as active (client)
    //
    axl_memset(&access_point, 0, sizeof(access_point));
    access_point.ActiveFlag       = true;
    access_point.UseDefaultAddress = true;
    access_point.StationPort      = 0;
    axl_memcpy(&access_point.RemoteAddress, &remote_addr, sizeof(EFI_IPv4_ADDRESS));
    access_point.RemotePort = port;

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

        axl_debug("async connect Configure: no mapping, waiting for DHCP(%llu/%u)",
                  (unsigned long long)(retry + 1), TCP_MAPPING_RETRIES);
        /* NOTE: API-contract violation — an async-start function
           should queue work on the loop, not block. Commit 7c98082
           swapped axl_backend_stall for axl_wait_ms so the CPU idles
           and Ctrl-C still works, but the caller's loop remains
           blocked for up to TCP_MAPPING_RETRIES * TCP_MAPPING_DELAY
           (~10 s) during the DHCP window. In practice no one hits
           this: the only caller that exercises Configure during the
           DHCP window is the sync wrapper, and the sync wrapper
           wants to block. Fix when a real caller runs
           axl_tcp_connect_async on a shared loop and observes other
           sources going silent during the Configure-retry window.
           Proper shape: on EFI_NO_MAPPING store pending_config in
           AxlTcp, register axl_loop_add_timeout for re-Configure,
           return 0 from the async-start call, and report final
           success/failure through the user callback. Tracked in
           ROADMAP. */
        axl_msleep(TCP_MAPPING_DELAY / 1000);
    }

    if (EFI_ERROR(status)) {
        axl_error("async connect Configure: %llx", (unsigned long long)status);
        axl_efi_call(sb->DestroyChild, 2, sb, child_handle);
        return AXL_ERR;
    }

    //
    // Allocate socket
    //
    sock = axl_calloc(1, sizeof(AxlTcp));
    if (sock == NULL) {
        axl_efi_call(tcp4->Configure, 2, tcp4, NULL);
        axl_efi_call(sb->DestroyChild, 2, sb, child_handle);
        return AXL_ERR;
    }

    sock->tcp4         = tcp4;
    sock->tcp_handle   = child_handle;
    sock->tcp_sb       = sb;
    sock->sb_handle    = sb_handle;
    sock->is_listener  = false;
    sock->on_connect   = cb;
    sock->connect_data = data;
    sock->async_loop   = loop;

    //
    // Create connect event
    //
    conn_event = NULL;
    if (axl_backend_event_create((AxlEventHandle *)&conn_event) != AXL_OK) {
        axl_error("async connect: cannot create event");
        axl_tcp_close(sock);
        return AXL_ERR;
    }

    sock->conn_token.CompletionToken.Event  = conn_event;
    sock->conn_token.CompletionToken.Status = EFI_ABORTED;

    status = axl_efi_call(tcp4->Connect, 2, tcp4, &sock->conn_token);
    if (EFI_ERROR(status)) {
        axl_error("async Connect: %llx", (unsigned long long)status);
        axl_tcp_close(sock);
        return AXL_ERR;
    }

    sock->connect_source = axl_loop_add_event(
        loop,
        (void *)conn_event,
        on_connect_complete,
        sock
        );
    if (sock->connect_source == 0) {
        axl_error("async connect: cannot register event with loop");
        axl_efi_call(tcp4->Cancel, 2, tcp4, &sock->conn_token.CompletionToken);
        axl_tcp_close(sock);
        return AXL_ERR;
    }

    if (cancel != NULL) {
        sock->connect_cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_connect_cancel,
            sock
            );
        /* Non-fatal if this fails — op runs uncancellable. */
    }

    return AXL_OK;
}

int
axl_tcp_connect_async(
    const char     *host,
    uint16_t        port,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlTcpCallback  cb,
    void           *data
    )
{
    /* Legacy entry point — auto-pick the source interface. */
    return axl_tcp_connect_async_via(host, port, NULL, loop, cancel, cb, data);
}
