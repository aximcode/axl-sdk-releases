/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tcp-async.c
    AxlTcp — event-driven (non-blocking) TCP API over EFI_TCP4_PROTOCOL.

    All four _async entry points follow the same pattern: install (or
    reuse) a completion-token event on the EFI TCP4 child, submit the
    operation, register the event with the caller's AxlLoop, and fire
    the user callback from the event handler. When an AxlCancellable
    is supplied, a second loop source is registered on the
    cancellable's event — on signal, the op's cancel handler tears
    everything down via the shared axl_tcp_*_drop_sources helper and
    fires the user callback with status=AXL_CANCELLED. The sync
    wrappers in axl-tcp-sync.c sit on top of these.

    Contract: on_*_complete and the cancel handlers BOTH route through
    the same _drop_sources helper so completion- and cancel-source
    teardown are symmetric — a slip in one path does not leave the
    loop with an orphaned source.

    SEND IS THE EXCEPTION, in two ways. Sends are QUEUED (one EFI token
    in flight, the rest waiting in FIFO order), and a send's callback is
    not called from its completion handler at all: it is DEFERRED to the
    loop's next iteration, the port of EDK2's SIGNAL_TOKEN. See the
    retirement section below and docs/AXL-Tcp-Queue-Design.md.

    Callback return value: user callbacks return bool. For accept/recv
    the value controls re-arming (true = keep armed, false = stop).
    Connect and send are inherently one-shot so completion handlers
    discard the return with (void).
**/

#include <axl/axl-defer.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-time.h>
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
    AxlSourceId         saved_accept_src = listener->accept_source;
    AxlSourceId         saved_cancel_src = listener->accept_cancel_source;

    _axl_net_latency_activity();

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
        cb(NULL, AXL_ERR, cb_data);  /* fundamental accept failure — tearing down regardless */
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
        cb(NULL, AXL_ERR, cb_data);
        goto rearm;
    }

    sock = axl_calloc(1, sizeof(AxlTcp));
    if (sock == NULL) {
        axl_efi_call(new_tcp4->Configure, 2, new_tcp4, NULL);
        axl_efi_call(listener->tcp_sb->DestroyChild, 2, listener->tcp_sb, new_handle);
        /* Same reasoning as above — treat as transient. */
        cb(NULL, AXL_ERR, cb_data);
        goto rearm;
    }

    sock->tcp4       = new_tcp4;
    sock->tcp_handle = new_handle;
    sock->tcp_sb     = listener->tcp_sb;
    sock->sb_handle  = listener->sb_handle;
    sock->is_listener = false;
    sock->listener_id = listener->listener_id;   /* for scoped deferred-close finalize */
    _axl_net_latency_link(sock);

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

        _axl_net_latency_activity();
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
    cb(NULL, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
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
    if (listener->closed) {
        return AXL_ERR;    /* torn down — see axl_tcp_recv_async */
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

    _axl_net_latency_activity();
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

/* The transmit pacer (below): a lingering member leaves with its receive. */
static void tcp_pace_leave(AxlTcp *sock);

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
    AxlLoop        *saved_loop       = sock->recv_loop;
    AxlSourceId     saved_recv_src   = sock->recv_source;
    AxlSourceId     saved_cancel_src = sock->recv_cancel_source;

    _axl_net_latency_activity();

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    /* A linger waits for exactly this (AxlTcpPacing.linger_us): it ends here,
       before the callback, which may close the socket or not re-arm. A reply
       sent from the callback joins the pacer as an ordinary send. */
    sock->stat_recvs++;
    if (sock->pace_linger_us != 0) {
        sock->stat_linger_recvs++;
        tcp_pace_leave(sock);
    }

    status = sock->rx_token.CompletionToken.Status;
    sock->recv_end = status;
    if (EFI_ERROR(status)) {
        sock->recv_size = 0;
        cb_status = AXL_ERR;
    } else {
        sock->recv_size = (size_t)sock->rx_data.DataLength;
        cb_status = AXL_OK;
    }

    cb = sock->on_recv;
    cb_data = sock->recv_data;

    /* After a false return sock may be freed, so the ids are cleared NOW:
       left set, a socket the callback kept open would carry ids of sources
       removed below, and its next close or re-arm would remove them again
       from a loop the caller may have finished with (axl_tcp_recv's
       ephemeral loop does the same for the same reason). A kept receive
       gets them back below. */
    sock->recv_source        = 0;
    sock->recv_cancel_source = 0;

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

    /* Kept: the same sources go on serving this receive -- unless the
       callback armed a new one (axl_tcp_recv_async), which replaces it. */
    if (sock->recv_source != 0) {
        if (saved_cancel_src > 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }
    /* The saved ids go back only onto the socket that still names their loop.
       A failed axl_tcp_recv_async leaves recv_loop alone, so this holds today;
       were it ever not to, restoring them would record ids on saved_loop under
       another loop's name, where a later close or drop could never remove them
       (source ids are unique across loops: removal from the wrong one is a
       no-op). Stop the receive instead of leaking its sources. */
    if (sock->recv_loop != saved_loop) {
        if (saved_cancel_src > 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }
    sock->recv_source        = saved_recv_src;
    sock->recv_cancel_source = saved_cancel_src;

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

    _axl_net_latency_activity();
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
         * next tick with rx_token.Status set to the error, which our
         * own error branch above translates to cb_status=-1.
         * Caller sees the EOF and can axl_tcp_close cleanly.
         * The token was never queued, so its Status is ours to set: set
         * it to the Receive's own status, so recv_end tells this FIN
         * from a reset (_axl_tcp_recv_fin). */
        axl_debug("recv re-arm Receive: %llx", (unsigned long long)status);
        sock->rx_token.CompletionToken.Status = status;
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
    /* A linger exists only for a receive armed on its loop: it ends with it,
       so the pacer slot never outlives a loop the caller frees once the
       receive is cancelled. */
    if (sock->pace_linger_us != 0) {
        tcp_pace_leave(sock);
    }
    if (sock->recv_source > 0) {
        axl_loop_remove_source(sock->recv_loop, sock->recv_source);
        sock->recv_source = 0;
    }
    if (sock->recv_cancel_source > 0) {
        axl_loop_remove_source(sock->recv_loop, sock->recv_cancel_source);
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
    cb(sock, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
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
    /* Refuse rather than arm firmware state this socket has already released.
       Reachable from a send callback the teardown itself fired — a keep-alive
       server re-arms its recv from exactly there (start_conn_recv). */
    if (sock->closed) {
        return AXL_ERR;
    }

    /* A linger exists only for a receive armed on its loop: one moving to
       another loop ends it (the old receive's source is removed directly
       below, not through axl_tcp_recv_drop_sources). */
    if (sock->pace_linger_us != 0 && sock->pacer != NULL && sock->pacer->loop != loop) {
        tcp_pace_leave(sock);
    }

    //
    // Cancel any pending recv before starting a new one -- its cancel
    // source too: left registered, it would cancel the NEW receive, and on
    // its old loop no later drop could find it.
    //
    if (sock->recv_source > 0) {
        axl_loop_remove_source(sock->recv_loop, sock->recv_source);
        sock->recv_source = 0;
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->rx_token.CompletionToken);
    }
    if (sock->recv_cancel_source > 0) {
        axl_loop_remove_source(sock->recv_loop, sock->recv_cancel_source);
        sock->recv_cancel_source = 0;
    }

    /* The socket's receive state (callback, buffer, loop) is written only
       once the receive is registered, so a call that fails leaves it as it
       was. That matters from inside a receive callback: a re-arm that fails
       and is followed by `return true` keeps the receive that just completed,
       with its own buffer, callback and loop, and on_recv_complete restores
       its source ids onto the loop that holds them. */

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

    _axl_net_latency_activity();
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
     * Status field is set to the Receive's own error below (Receive
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

    sock->recv_loop     = loop;
    sock->async_loop    = loop;
    sock->on_recv       = cb;
    sock->recv_data     = data;
    sock->recv_buf      = buf;
    sock->recv_capacity = size;
    sock->recv_size     = size;  /* rewritten to bytes-received by on_recv_complete */
    sock->recv_end      = EFI_SUCCESS;   /* nothing has ended this recv yet */

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
     * loop dispatches on_recv_complete with the Receive's own error
     * status — see comment on the Receive call above. The most
     * common cause is a peer FIN delivered between the previous
     * recv completing and this re-arm (normal end-of-stream), so
     * trace at debug verbosity rather than warn. */
    if (EFI_ERROR(status)) {
        axl_debug("async Receive sync-completed (peer EOF likely): %llx",
                  (unsigned long long)status);
        /* Never queued: carry the Receive's own status to the
           completion, so recv_end tells a FIN from a reset. */
        sock->rx_token.CompletionToken.Status = status;
        axl_backend_event_signal(
            (AxlEventHandle)sock->rx_token.CompletionToken.Event);
    }

    return AXL_OK;
}

// ---- send async ------------------------------------------------------------

/* The largest piece of a send per UEFI Transmit is AxlTcpPacing.chunk_bytes
   (axl-tcp.h has the measured reasons for its default and range). A larger
   async send is split into a chain of Transmits of at most that size, for two
   reasons: no one Transmit is unbounded (one a stalled client cannot drain
   would monopolize the one-send-in-flight path), and it bounds the depth of
   one output burst below iPXE's 15-frames-between-polls limit. It is read
   when each piece is ARMED, and the piece is retired by the length it was
   armed with (tx_armed), so a change mid-send is safe. */

// ---- transmit pacing ---------------------------------------------------------
//
// A TcpDxe Transmit token completes only when the chunk's LAST byte is
// segmentised, and the sub-MSS tail is held (SWS/Nagle, or cwnd) until the
// host's ACKs for the rest have been PROCESSED. EDK2 MnpDxe processes one
// received frame per Snp->Receive, driven by a 10 ms timer, so a loop asleep
// in WaitForEvent drains a burst's ACKs at one per 10 ms: every chunk cost
// ~10 ms x (segments - 1), and a 1 MiB send took 7-10 s on both arches with
// every segment on the wire within a millisecond of its Transmit.
//
// Each Tcp4->Poll() drains one frame. So while a send is on the wire, the
// loop polls every socket that has one, on every pass, from a BUSY idle
// source (axl_loop_add_idle_busy: an idle source that also extends a
// driver-mode tick, up to the loop's budget). It is not a spin inside a
// callback -- every other source is still checked and dispatched each
// pass, in the loop's normal round-robin -- and it goes away with the last
// pending send, so a loop with nothing to transmit sleeps exactly as before.
//
// ONE source per loop, not per socket. AXL_MAX_SOURCES is 64 per loop and
// every socket already spends its own on completions and cancellables; a
// source per sending socket would scale with connections for no benefit,
// because a single idle callback can poll every member. It is per LOOP, not
// global, because a send's progress is driven from the loop its submitter
// chose -- a sync axl_tcp_send nests an ephemeral loop, and a source on the
// outer loop would not run while that one does.
//
// Back-off. A send can also be pending with nothing to drain: a peer that has
// stopped reading holds it at a zero window for as long as it likes. A pacer
// whose members have made no progress (no Transmit armed) for
// AxlTcpPacing.idle_backoff_ms removes its source, and the loop sleeps again;
// the next Transmit re-adds it. While backed off a send advances at the
// firmware's own rate, which is exactly the pre-pacing behaviour. A clock
// that cannot be read counts as no progress.
//
// Linger (AxlTcpPacing.linger_us, off by default). A socket whose last send
// retires with a receive armed on the same loop stays a member that much
// longer, so a request/response peer's next request is drained and answered
// while the loop still polls. Its members leave the pacer when their time
// is up, and the source goes with the last member.
// ---------------------------------------------------------------------------

/* Loops that can have a send pending AT ONCE: the server's own loop plus any
   nested sync-send loops. A socket that finds the table full is sent
   unpaced -- correct, only slow -- and says so once per process. */
#define TCP_PACE_LOOPS_MAX  8u

static AxlTcpPacer  g_tcp_pacers[TCP_PACE_LOOPS_MAX];
static AxlTcpPacing g_tcp_pacing = {
    .chunk_bytes     = AXL_TCP_PACING_CHUNK_DEFAULT_BYTES,
    .idle_backoff_ms = AXL_TCP_PACING_IDLE_BACKOFF_DEFAULT_MS,
    .linger_us       = AXL_TCP_PACING_LINGER_DEFAULT_US,
};
/* One warning per fallback per PROCESS (not per pacer slot, which is
   recycled with every burst). */
static bool         g_tcp_pacers_full_warned;
static bool         g_tcp_pace_source_warned;
/* The monotonic clock, probed once. A failed calibration is not memoised by
   the backend and would cost a 10 ms stall on every read, so after one
   failure pacing never reads it again (and never extends a spin). */
static enum { TCP_CLOCK_UNKNOWN, TCP_CLOCK_OK, TCP_CLOCK_BROKEN } g_tcp_clock;

void
axl_tcp_pacing_defaults(AxlTcpPacing *out)
{
    if (out == NULL) {
        return;
    }
    out->chunk_bytes     = AXL_TCP_PACING_CHUNK_DEFAULT_BYTES;
    out->idle_backoff_ms = AXL_TCP_PACING_IDLE_BACKOFF_DEFAULT_MS;
    out->linger_us       = AXL_TCP_PACING_LINGER_DEFAULT_US;
}

bool
_axl_tcp_pacer_bound(const AxlLoop *loop)
{
    for (size_t i = 0; i < TCP_PACE_LOOPS_MAX; i++) {
        if (loop != NULL && g_tcp_pacers[i].loop == loop) {
            return true;
        }
    }
    return false;
}

bool
_axl_tcp_lingering(const AxlTcp *sock)
{
    return sock != NULL && sock->pacer != NULL && sock->pace_linger_us != 0;
}

void
axl_tcp_get_pacing(AxlTcpPacing *out)
{
    if (out != NULL) {
        *out = g_tcp_pacing;
    }
}

int
axl_tcp_set_pacing(const AxlTcpPacing *cfg)
{
    if (cfg == NULL) {
        axl_tcp_pacing_defaults(&g_tcp_pacing);
        return AXL_OK;
    }
    if (cfg->chunk_bytes < AXL_TCP_PACING_CHUNK_MIN_BYTES
        || cfg->chunk_bytes > AXL_TCP_PACING_CHUNK_MAX_BYTES
        || cfg->idle_backoff_ms > AXL_TCP_PACING_IDLE_BACKOFF_MAX_MS
        || cfg->linger_us > AXL_TCP_PACING_LINGER_MAX_US) {
        return AXL_INVALID;
    }
    g_tcp_pacing = *cfg;
    return AXL_OK;
}

static bool
tcp_pace_now_us(uint64_t *out)
{
    AxlTimespec ts;

    if (g_tcp_clock == TCP_CLOCK_BROKEN) {
        return false;
    }
    if (axl_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        g_tcp_clock = TCP_CLOCK_BROKEN;
        return false;
    }
    g_tcp_clock = TCP_CLOCK_OK;
    *out = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    return true;
}

static bool
tcp_pace_now_ms(uint64_t *out)
{
    uint64_t us;

    if (!tcp_pace_now_us(&us)) {
        return false;
    }
    *out = us / 1000u;
    return true;
}

/* Take @p sock off its pacer's member list; the caller decides what an
   empty list means. */
static void
tcp_pace_unlink(AxlTcp *sock)
{
    AxlTcpPacer  *p = sock->pacer;
    AxlTcp      **pp;

    for (pp = &p->head; *pp != NULL; pp = &(*pp)->pace_next) {
        if (*pp == sock) {
            *pp = sock->pace_next;
            break;
        }
    }
    sock->pacer          = NULL;
    sock->pace_next      = NULL;
    sock->pace_linger_us = 0;
}

static bool
tcp_pace_idle(void *data)
{
    AxlTcpPacer *p = data;
    AxlTcp      *s;
    uint64_t     now;

    /* Assumes Tcp4->Poll does not call back into AxlTcp synchronously:
       AxlTcp registers its completion events without notify functions, so
       what Poll completes is only SIGNALLED here and dispatched by the loop
       afterwards. If another image's notify ran AxlTcp code from inside
       Poll, the member list could change underneath this walk. */
    for (s = p->head; s != NULL; s = s->pace_next) {
        axl_efi_call(s->tcp4->Poll, 1, s->tcp4);
    }

    /* No progress for idle_backoff_ms: this source goes, and the loop sleeps. */
    bool backoff = !(p->progress_known && tcp_pace_now_ms(&now)
                     && now - p->progress_ms < g_tcp_pacing.idle_backoff_ms);

    /* Lingering members leave when their time is up -- all of them if the
       clock cannot say, or when the pacer backs off: only this source
       expires them, so a member left behind by it would stay linked, and
       the slot bound to its loop, until that socket's next send or close. */
    uint64_t now_us = 0;
    bool     have   = tcp_pace_now_us(&now_us);
    for (s = p->head; s != NULL;) {
        AxlTcp *next = s->pace_next;
        if (s->pace_linger_us != 0 && (!have || backoff || now_us >= s->pace_linger_us)) {
            tcp_pace_unlink(s);
        }
        s = next;
    }
    if (p->head == NULL) {
        axl_memset(p, 0, sizeof(*p));
        return AXL_SOURCE_REMOVE;
    }
    if (!backoff) {
        return AXL_SOURCE_CONTINUE;
    }
    p->idle = 0;      /* backed off; the next Transmit re-adds it */
    return AXL_SOURCE_REMOVE;
}

/* A member armed a Transmit: record the progress, and resume polling if the
   pacer had backed off. */
static void
tcp_pace_kick(AxlTcpPacer *p)
{
    p->progress_known = tcp_pace_now_ms(&p->progress_ms);
    if (p->idle != 0 || g_tcp_pacing.idle_backoff_ms == 0) {
        return;     /* already polling, or loop-driven polling is off */
    }
    p->idle = axl_loop_add_idle_busy(p->loop, tcp_pace_idle, p);
    if (p->idle == 0 && !g_tcp_pace_source_warned) {
        g_tcp_pace_source_warned = true;
        axl_warning("async send: cannot add the transmit-pacing source "
                    "(loop source table full) - sends on this loop drain "
                    "at the firmware's 10 ms rate");
    }
}

static void
tcp_pace_leave(AxlTcp *sock)
{
    AxlTcpPacer *p = sock->pacer;

    if (p == NULL) {
        return;
    }
    tcp_pace_unlink(sock);

    if (p->head == NULL) {
        /* Last member gone: the loop has nothing left to pace, so it must go
           back to sleeping. Removing the source here, rather than letting it
           notice on its next run, also means the slot never outlives a loop
           the caller frees right after its last send. A lingering member
           (AxlTcpPacing.linger_us) keeps that true: it leaves no later than
           the receive that justifies it -- dropped or cancelled
           (axl_tcp_recv_drop_sources), or completed (on_recv_complete) --
           and at the pacer's back-off, its expiry, or its close. */
        if (p->idle != 0) {
            axl_loop_remove_source(p->loop, p->idle);
        }
        axl_memset(p, 0, sizeof(*p));
    }
}

static void
tcp_pace_join(AxlTcp *sock, AxlLoop *loop)
{
    AxlTcpPacer *p = NULL;
    size_t       i;

    for (i = 0; i < TCP_PACE_LOOPS_MAX; i++) {
        if (g_tcp_pacers[i].loop == loop) {
            p = &g_tcp_pacers[i];
            break;
        }
        if (p == NULL && g_tcp_pacers[i].loop == NULL) {
            p = &g_tcp_pacers[i];     /* first free slot, if loop has none */
        }
    }
    if (p == NULL) {
        if (!g_tcp_pacers_full_warned) {
            g_tcp_pacers_full_warned = true;
            axl_warning("async send: more than %u loops sending at once - "
                        "this send drains at the firmware's 10 ms rate",
                        TCP_PACE_LOOPS_MAX);
        }
        return;
    }
    p->loop         = loop;
    sock->pace_next = p->head;
    p->head         = sock;
    sock->pacer     = p;
}

/* Make @p sock's pacer membership match its active send: a member of the
   active token's loop's pacer while a send is on the wire, of none otherwise.
   Called wherever send_active changes. */
static void
tcp_pace_update(AxlTcp *sock)
{
    AxlLoop *want = NULL;

    /* A send waiting behind an orphan piece needs the ACKs drained just as
       much as one on the wire: the orphan completes only as TCP segments it. */
    if (!sock->closed) {
        if (sock->send_active != NULL) {
            want = sock->send_active->loop;
        } else if (sock->tx_orphan && sock->send_queued_head != NULL) {
            want = sock->send_queued_head->loop;
        }
    }

    if (sock->pacer != NULL && sock->pacer->loop != want) {
        /* Its last send retired: linger (AxlTcpPacing.linger_us) while its
           peer's next request may be on the way. A request/response peer
           sends it the moment the reply arrives, and with nothing polling
           the stack it would wait for MnpDxe's 10 ms timer, and under
           driver mode for the next tick too. Only for a receive on this
           same loop, which already ties the socket to it, and only while
           the loop polls at all (an idle_backoff_ms of 0 means it never
           does). A deadline already set is kept. */
        uint64_t now_us = 0;
        if (want == NULL && g_tcp_pacing.linger_us != 0 && sock->pace_linger_us == 0
            && !sock->closed && sock->recv_source != 0
            && sock->recv_loop == sock->pacer->loop && sock->pacer->idle != 0
            && tcp_pace_now_us(&now_us)) {
            sock->pace_linger_us = now_us + g_tcp_pacing.linger_us;
            sock->stat_lingers++;
        }
        if (want != NULL || sock->pace_linger_us == 0 || sock->closed) {
            tcp_pace_leave(sock);
        }
    }
    if (want != NULL && sock->pacer == NULL) {
        tcp_pace_join(sock, want);
    }
    if (want != NULL) {
        sock->pace_linger_us = 0;    /* a send again: an ordinary member */
    }
}

/* Arm the next Transmit for the active send: copy
   min(AxlTcpPacing.chunk_bytes, uncommitted) bytes from where the last piece
   stopped into the socket's bounce buffer and hand THAT to the firmware,
   reusing tx_token/tx_event. On success the piece counts as committed.
   Returns the EFI Transmit status. Does NOT touch the loop source — the
   caller owns source lifecycle. */
static EFI_STATUS
tcp_send_arm_chunk(AxlTcp *sock)
{
    AxlTcpToken *tok   = sock->send_active;
    size_t       max   = g_tcp_pacing.chunk_bytes;
    size_t       left  = tok->len - tok->committed;
    size_t       chunk = (left < max) ? left : max;
    EFI_STATUS   status;

    /* The copy is what lets AxlTcp hand a caller its buffer back the moment
       its send is cancelled or its connection reset, without ever cancelling
       a Transmit the firmware is still processing (see tx_orphan). Grown only
       here, between Transmits -- one is in flight at a time, so the old
       buffer is no longer the firmware's. Sized to the pacing maximum in
       force, so a later chunk_bytes change at most regrows it once. */
    if (sock->tx_bounce_cap < chunk) {
        axl_free(sock->tx_bounce);
        sock->tx_bounce_cap = 0;
        sock->tx_bounce     = axl_malloc(max);
        if (sock->tx_bounce == NULL) {
            axl_error("async send: cannot allocate the %zu-byte transmit buffer",
                      max);
            return EFI_OUT_OF_RESOURCES;
        }
        sock->tx_bounce_cap = max;
    }
    axl_memcpy(sock->tx_bounce, tok->buf + tok->committed, chunk);

    /* Arming a Transmit IS progress: it follows either a new send or the
       completion of the previous chunk. */
    tcp_pace_update(sock);
    if (sock->pacer != NULL) {
        tcp_pace_kick(sock->pacer);
    }

    /* Poll once before every Transmit. The 15-frame limit above is per NIC,
       not per connection: several sockets transmitting in one loop turn can
       sum past it even with small chunks, and a poll reaps the TX ring. */
    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    sock->tx_armed               = chunk;
    sock->tx_frag.FragmentLength = (uint32_t)chunk;
    sock->tx_frag.FragmentBuffer = sock->tx_bounce;

    axl_memset(&sock->tx_data, 0, sizeof(sock->tx_data));
    sock->tx_data.Push             = true;
    sock->tx_data.DataLength       = (uint32_t)chunk;
    sock->tx_data.FragmentCount    = 1;
    sock->tx_data.FragmentTable[0] = sock->tx_frag;

    sock->tx_token.CompletionToken.Status = EFI_ABORTED;
    sock->tx_token.Packet.TxData          = &sock->tx_data;

    /* Clear any stale signal so the loop won't fire on_send_complete before
       this Transmit actually completes. */
    axl_backend_event_check(
        (AxlEventHandle)sock->tx_token.CompletionToken.Event);

    _axl_net_latency_activity();
    status = axl_efi_call(sock->tcp4->Transmit, 2, sock->tcp4, &sock->tx_token);
    if (!EFI_ERROR(status)) {
        /* Handed over: it cannot be recalled, so it counts from now on even
           if the send is cancelled or the connection fails before TCP has
           taken all of it. A Transmit refused outright does not count. */
        tok->committed    += chunk;
        sock->tx_inflight  = true;
    }
    return status;
}

static bool on_send_complete(void *data);
static bool on_send_token_cancel(void *data);
static void tcp_send_promote(AxlTcp *sock);

/* Append an accepted send. Port of EDK2 SockBufferToken into SndTokenList.
   The caller's buffer is BORROWED until the send's callback, not copied here:
   each piece is copied into the socket's bounce buffer only as it is armed
   (tcp_send_arm_chunk), so a queued send costs one small token and the
   firmware never references the caller's memory. Every send lands here
   first, including the one that is about to be promoted straight out again:
   that is what keeps the retirement path allocation-free (see AxlTcpToken). */
static int
tcp_send_enqueue(
    AxlTcp         *sock,
    const void     *buf,
    size_t          size,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlTcpCallback  cb,
    void           *data
    )
{
    AxlTcpToken *tok = axl_malloc(sizeof(*tok));

    if (tok == NULL) {
        return AXL_ERR;
    }
    tok->next          = NULL;
    tok->sock          = sock;
    tok->buf           = (const uint8_t *)buf;
    tok->len           = size;
    tok->committed     = 0;
    tok->finished      = false;
    tok->cb            = cb;
    tok->cb_data       = data;
    tok->cancel        = cancel;
    tok->loop          = loop;
    tok->cancel_source = 0;
    tok->status        = AXL_OK;
    tok->done_defer    = 0;
    tok->done_source   = 0;

    /* Arm cancellation NOW, before the token can be promoted, and keep the one
       source until retirement — port of EDK2 SockCancelToken, which walks
       SndTokenList (the deferred list) as well as the processing one.
       Load-bearing: axl_tcp_send (sync) passes a stack SyncResult, an
       ephemeral loop and a cancellable, and frees all three on return. Its
       timeout can only retire this token — and run cb while that frame is
       still alive — if the cancellable is live while QUEUED. Design §3.6 / §7.
       One source for both states also removes the re-arm-at-promotion step,
       and with it a failure path that could leave a promoted send
       uncancellable. */
    if (cancel != NULL) {
        tok->cancel_source = axl_loop_add_event(
            loop, _axl_cancellable_event(cancel),
            on_send_token_cancel, tok);
        if (tok->cancel_source == 0) {
            /* Refuse rather than accept an uncancellable token: the caller
               would have no way to reclaim its context. */
            axl_free(tok);
            return AXL_ERR;
        }
    }

    if (sock->send_queued_tail != NULL) {
        sock->send_queued_tail->next = tok;
    } else {
        sock->send_queued_head = tok;
    }
    sock->send_queued_tail  = tok;
    sock->send_queued_bytes += size;
    return AXL_OK;
}

/* Unlink @p tok from the queue. Returns false if it was not on it. */
static bool
tcp_send_unlink(AxlTcp *sock, AxlTcpToken *tok)
{
    AxlTcpToken **pp = &sock->send_queued_head;
    AxlTcpToken  *prev = NULL;

    while (*pp != NULL) {
        if (*pp == tok) {
            *pp = tok->next;
            if (sock->send_queued_tail == tok) {
                sock->send_queued_tail = prev;
            }
            sock->send_queued_bytes -= tok->len;
            tok->next = NULL;
            return true;
        }
        prev = *pp;
        pp   = &(*pp)->next;
    }
    return false;
}

/* Pop the head token, or NULL. */
static AxlTcpToken *
tcp_send_dequeue(AxlTcp *sock)
{
    AxlTcpToken *tok = sock->send_queued_head;

    if (tok == NULL) {
        return NULL;
    }
    sock->send_queued_head = tok->next;
    if (sock->send_queued_head == NULL) {
        sock->send_queued_tail = NULL;
    }
    sock->send_queued_bytes -= tok->len;
    tok->next = NULL;
    return tok;
}

// ---- retirement: the port of EDK2's SIGNAL_TOKEN ---------------------------
//
// EDK2 retires a token with SIGNAL_TOKEN (SockImpl.h:23), which is
// gBS->SignalEvent: it QUEUES the consumer's notify rather than calling it, so
// no consumer code ever runs inside the transport's own call stack. AXL's
// equivalent is axl_defer, whose queue the loop drains at the TOP of its next
// iteration — before it waits on or dispatches anything else.
//
// That is what makes the rest of this file safe to write. A send callback may
// axl_tcp_close and free the socket, and while callbacks ran inline every
// ordering of "retire, promote, signal" had either a use-after-free or a
// dropped callback in it: promote-then-signal loses the callback of a send
// promoted behind the caller's back, signal-then-promote frees the socket
// before the promote, and barring promotion during teardown only helps when
// the close starts first (design §6b). Nothing runs inline now, so there is no
// ordering left to get wrong.
// ---------------------------------------------------------------------------

/* Release @p tok, cancelling everything still pointing AT it: its cancel
   source and its scheduled delivery. Both sit on loops that may outlive this
   socket, so neither may outlive the token. */
static void
tcp_send_free_token(AxlTcpToken *tok)
{
    if (tok->cancel_source > 0) {
        axl_loop_remove_source(tok->loop, tok->cancel_source);
    }
    if (tok->done_defer != 0) {
        axl_defer_cancel(tok->loop, tok->done_defer);
    }
    if (tok->done_source != 0) {
        axl_loop_remove_source(tok->loop, tok->done_source);
    }
    axl_free(tok);
}

/* Unlink @p tok from the retired list. False if it was not on it. */
static bool
tcp_send_done_unlink(AxlTcp *sock, AxlTcpToken *tok)
{
    AxlTcpToken *prev = NULL;
    AxlTcpToken *cur;

    for (cur = sock->send_done_head; cur != NULL; prev = cur, cur = cur->next) {
        if (cur != tok) {
            continue;
        }
        if (prev != NULL) {
            prev->next = tok->next;
        } else {
            sock->send_done_head = tok->next;
        }
        if (sock->send_done_tail == tok) {
            sock->send_done_tail = prev;
        }
        tok->next = NULL;
        return true;
    }
    return false;
}

/* Run the callbacks of an ALREADY-DETACHED list of retired tokens. `sock` is
   only ever passed to a callback here, never dereferenced afterwards, so a
   callback that closes the socket cannot pull the walk out from under itself —
   and finalize_sock defers its free until the outermost delivery is done. */
static void
tcp_send_deliver(AxlTcp *sock, AxlTcpToken *list)
{
    bool outer = !sock->send_draining;

    sock->send_draining = true;
    while (list != NULL) {
        AxlTcpToken    *tok    = list;
        AxlTcpCallback  cb     = tok->cb;
        void           *udata  = tok->cb_data;
        AxlStatus       status = tok->status;

        list = tok->next;
        /* What axl_tcp_send_get_sent reports from inside this callback. Not
           restored afterwards: the contract is "until the callback's next
           axl_tcp_* call on the socket", and a nested delivery is one. */
        sock->send_last_sent     = tok->committed;
        sock->send_last_finished = tok->finished;
        tcp_send_free_token(tok);
        cb(sock, status, udata);  /* send is one-shot; return value ignored */
    }

    if (!outer) {
        return;      /* a nested delivery leaves the flags to the outermost */
    }
    sock->send_draining = false;
    if (sock->free_deferred && !sock->firmware_holds) {
        /* A close ran from one of the callbacks above and finished its
           teardown, but could not free the socket while this walk was still
           handing it out. finalize_sock left that to us -- unless the
           firmware may still hold parts of it (see firmware_holds). */
        axl_free(sock);
    }
}

/* Deliver ONE retired token — what its own loop calls when the schedule made
   at retirement comes up. */
static void
tcp_send_deliver_one(AxlTcpToken *tok)
{
    AxlTcp *sock = tok->sock;

    if (!tcp_send_done_unlink(sock, tok)) {
        return;   /* close got there first; the token is long gone */
    }
    tcp_send_deliver(sock, tok);
}

static void
tcp_send_done_defer_cb(void *data)
{
    AxlTcpToken *tok = (AxlTcpToken *)data;

    tok->done_defer = 0;   /* being consumed right now: no longer cancellable */
    tcp_send_deliver_one(tok);
}

static bool
tcp_send_done_timer_cb(void *data)
{
    AxlTcpToken *tok = (AxlTcpToken *)data;

    tok->done_source = 0;  /* one-shot: the loop drops it on return */
    tcp_send_deliver_one(tok);
    return AXL_SOURCE_REMOVE;
}

/* A send retired part-way, whatever its status, left the peer holding a
   message cut short -- remembered for the close (see stream_torn). A send
   that never reached the firmware (0) did not. */
static void
tcp_send_note_torn(AxlTcp *sock, const AxlTcpToken *tok)
{
    if (tok->committed > 0 && tok->committed < tok->len) {
        sock->stream_torn = true;
    }
}

/* Park @p tok on the retired list with @p status. Never calls the callback,
   and never schedules one — the close path delivers what it parks itself. The
   token must already be off both the queue and the active slot. */
static void
tcp_send_park(AxlTcp *sock, AxlTcpToken *tok, AxlStatus status)
{
    if (tok->cancel_source > 0) {
        /* Dropped here rather than at the free, so a cancellable firing
           between now and the delivery cannot re-retire a token that is
           already spoken for. */
        axl_loop_remove_source(tok->loop, tok->cancel_source);
        tok->cancel_source = 0;
    }
    tok->status = status;
    tok->next   = NULL;
    tcp_send_note_torn(sock, tok);

    if (sock->send_done_tail != NULL) {
        sock->send_done_tail->next = tok;
    } else {
        sock->send_done_head = tok;
    }
    sock->send_done_tail = tok;
}

/* axl_tcp_fail_next_send_schedule's counter. Process-wide, like
   axl_mem_fail_next_alloc's: 0 = disabled. */
static size_t mFailNextSendSchedule;

void
axl_tcp_fail_next_send_schedule(
    size_t n
    )
{
    mFailNextSendSchedule = n;
}

/* True when this retirement is the one axl_tcp_fail_next_send_schedule
   armed. EVERY retirement counts -- a finished send, a cancelled one, an arm
   failure -- because they all pass through tcp_send_retire. */
static bool
tcp_send_schedule_injected_fail(void)
{
    if (mFailNextSendSchedule == 0) {
        return false;
    }
    mFailNextSendSchedule--;
    return mFailNextSendSchedule == 0;
}

/* Retire @p tok: park it, then ask ITS OWN loop to deliver the callback on the
   loop's next iteration. */
static void
tcp_send_retire(AxlTcp *sock, AxlTcpToken *tok, AxlStatus status)
{
    tcp_send_park(sock, tok, status);

    if (tcp_send_schedule_injected_fail()) {
        /* Parked unscheduled, exactly as below when both pools are full --
           but said at DEBUG and in words of its own, so an injected hold is
           never read as the real exhaustion the error line reports (the
           same split axl_mem_fail_next_alloc keeps from a real OOM). */
        axl_debug("async send: completion held (injected by "
                  "axl_tcp_fail_next_send_schedule), status %d", (int)status);
        return;
    }
    tok->done_defer = axl_defer(tok->loop, tcp_send_done_defer_cb, tok);
    if (tok->done_defer != 0) {
        return;
    }
    /* The loop's defer ring is full (~42 entries, shared with every other user
       of it). Fall back to a one-shot timer, which draws on a different pool —
       something has to carry this callback, and there may be no later send to
       piggy-back on: axl_tcp_send's ephemeral loop can only be quit BY this
       delivery. 1ms, since the loop rejects a 0 delay. */
    tok->done_source = axl_loop_add_timeout(tok->loop, 1,
                                            tcp_send_done_timer_cb, tok);
    if (tok->done_source == 0) {
        /* Defer ring AND source table both full. The token stays parked, and
           axl_tcp_close delivers it before the socket is freed. Late, never
           lost — but late enough to be worth a line. */
        axl_error("async send: cannot schedule the completion callback "
                  "(defer queue and source table both full)");
    }
}

void
axl_tcp_send_drain_done(AxlTcp *sock)
{
    AxlTcpToken *list = sock->send_done_head;

    sock->send_done_head = NULL;
    sock->send_done_tail = NULL;
    tcp_send_deliver(sock, list);
}

/* Arm @p tok as the active send. On failure the token is retired with
   AXL_ERR — a transport that refuses one send must still tell that send's
   caller, and must not park every send behind it (design §6b defect 5).

   The completion source is registered BEFORE the Transmit, so no failure
   here ever leaves a Transmit armed with nothing watching it -- the one
   exit that used to need EFI_TCP4.Cancel on an armed Transmit, which is
   exactly what AxlTcp must never do (see tx_orphan). */
static bool
tcp_send_arm_token(AxlTcp *sock, AxlTcpToken *tok)
{
    sock->send_active = tok;
    sock->async_loop  = tok->loop;

    if (sock->tx_token.CompletionToken.Event == NULL) {
        EFI_EVENT tx_event = NULL;
        if (axl_backend_event_create((AxlEventHandle *)&tx_event) != AXL_OK) {
            axl_error("async send: cannot create event");
            goto arm_failed;
        }
        sock->tx_token.CompletionToken.Event = tx_event;
    }

    sock->send_source = axl_loop_add_event(
        tok->loop,
        (AxlEventHandle)sock->tx_token.CompletionToken.Event,
        on_send_complete, sock);
    if (sock->send_source == 0) {
        axl_error("async send: cannot register event with loop");
        goto arm_failed;
    }
    sock->send_source_loop = tok->loop;

    if (EFI_ERROR(tcp_send_arm_chunk(sock))) {
        axl_debug("async Transmit failed to arm %zu byte(s)", tok->len);
        axl_tcp_send_drop_sources(sock);
        goto arm_failed;
    }
    return true;

arm_failed:
    sock->send_active = NULL;
    tcp_send_retire(sock, tok, AXL_ERR);
    return false;
}

static bool on_orphan_complete(void *data);

/* The orphan piece's Transmit has signalled: the transport is free again. */
static void
tcp_send_orphan_finish(AxlTcp *sock)
{
    EFI_STATUS status = sock->tx_token.CompletionToken.Status;

    if (EFI_ERROR(status)) {
        /* Its send has already been reported; this failure belongs to no
           caller, and the next send or recv sees the connection's state. */
        axl_debug("async send: orphan piece of %zu byte(s) failed: %llx",
                  sock->tx_armed, (unsigned long long)status);
    }
    sock->tx_orphan   = false;
    sock->tx_inflight = false;
    /* A watch left on some loop would otherwise fire for the NEXT send's
       Transmit on the same event and steal its completion. */
    axl_tcp_send_drop_sources(sock);
}

/* Has the orphan piece's Transmit completed? Consumes the signal if so. */
static bool
tcp_send_orphan_done(AxlTcp *sock)
{
    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);
    if (axl_backend_event_check(
            (AxlEventHandle)sock->tx_token.CompletionToken.Event) != 0) {
        return false;
    }
    tcp_send_orphan_finish(sock);
    return true;
}

/* Watch the orphan piece from the loop of the send waiting behind it -- the
   loop that must run for that send to start -- and from no other. The loop the
   orphan's own send came from may be gone (a sync axl_tcp_send frees its
   ephemeral loop on return), and with nothing queued nobody needs to know:
   the next submit checks the event itself. */
static void
tcp_send_orphan_watch(AxlTcp *sock)
{
    AxlLoop *want = (sock->send_queued_head != NULL)
                    ? sock->send_queued_head->loop : NULL;

    if (sock->send_source > 0 && sock->send_source_loop == want) {
        return;
    }
    axl_tcp_send_drop_sources(sock);
    if (want == NULL) {
        return;
    }
    sock->send_source = axl_loop_add_event(
        want, (AxlEventHandle)sock->tx_token.CompletionToken.Event,
        on_orphan_complete, sock);
    if (sock->send_source == 0) {
        /* The completion itself is not lost -- the next submit or cancel on
           this socket re-checks the event directly -- but nothing else does:
           a send already queued behind the orphan, with no further submit,
           waits until its own timeout or cancel ends it. Say so. */
        axl_warning("async send: cannot watch the orphan piece (loop source "
                    "table full); the send queued behind it waits for the "
                    "next submit, its cancel or its timeout");
        return;
    }
    sock->send_source_loop = want;
}

/* Hand the head of the queue to the transport. Port of EDK2
   SockProcessSndToken. AXL keeps exactly one EFI token in flight (design §2),
   so at most one send is armed here and the next promotion happens from that
   send's own completion; the loop only spins while tokens are failing to arm. */
static void
tcp_send_promote(AxlTcp *sock)
{
    /* A piece left armed by a cancel still owns the transport. Arming over it
       would reuse tx_token while the firmware holds it; cancelling it is what
       corrupts TcpDxe. So the queue waits for its token to signal. */
    if (sock->tx_orphan && !sock->closed && !tcp_send_orphan_done(sock)) {
        tcp_send_orphan_watch(sock);
        tcp_pace_update(sock);
        return;
    }
    if (sock->tx_orphan) {
        return;     /* closed: the close owns what is left */
    }
    while (sock->send_active == NULL && !sock->closed) {
        AxlTcpToken *tok = tcp_send_dequeue(sock);

        if (tok == NULL || tcp_send_arm_token(sock, tok)) {
            break;
        }
    }
    /* Every path that empties the active slot comes through here (completion,
       cancel, drop, arm failure), so this is where a socket that has nothing
       left on the wire stops being paced. */
    tcp_pace_update(sock);
}

static bool
on_send_complete(void *data)
{
    AxlTcp      *sock = data;
    AxlTcpToken *tok  = sock->send_active;
    EFI_STATUS   status;

    _axl_net_latency_activity();

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    status            = sock->tx_token.CompletionToken.Status;
    sock->tx_inflight = false;
    if (!EFI_ERROR(status)) {
        /* UEFI Transmit tokens are all-or-nothing per submission: this
           completion means TCP has taken the whole piece. `committed` already
           advanced by exactly the length it was ARMED with when the Transmit
           was accepted, so the next piece starts there. */
        if (tok->committed < tok->len) {
            /* More to go — re-arm the next chunk on the SAME tx_event and keep
               this loop source (AXL_SOURCE_CONTINUE); the user callback fires
               only after the final chunk completes. */
            status = tcp_send_arm_chunk(sock);
            if (!EFI_ERROR(status)) {
                return AXL_SOURCE_CONTINUE;
            }
            axl_error("async send: chunk re-arm failed at %zu/%zu: %llx",
                      tok->committed, tok->len,
                      (unsigned long long)status);
            /* fall through: report the re-arm failure as a send failure */
        }
    } else {
        axl_error("async send failed: %llx", (unsigned long long)status);
    }

    tok->finished     = !EFI_ERROR(status);
    sock->send_active = NULL;
    axl_tcp_send_drop_sources(sock);
    tcp_send_retire(sock, tok, EFI_ERROR(status) ? AXL_ERR : AXL_OK);
    /* Promote immediately: only the NOTIFICATION is deferred, so the wire
       keeps moving at full rate while the callback waits for the next tick. */
    tcp_send_promote(sock);
    return AXL_SOURCE_REMOVE;
}

/* The orphan piece's Transmit signalled, seen from the loop of the send
   waiting behind it. */
static bool
on_orphan_complete(void *data)
{
    AxlTcp *sock = data;

    /* The loop consumed the signal to dispatch us, and removes this source
       on the REMOVE return -- forget it first so the finish does not remove
       it twice. */
    sock->send_source = 0;
    tcp_send_orphan_finish(sock);
    tcp_send_promote(sock);
    return AXL_SOURCE_REMOVE;
}

bool
axl_tcp_send_pending(const AxlTcp *sock)
{
    return sock != NULL
           && (sock->send_active != NULL || sock->send_queued_head != NULL);
}

void
axl_tcp_send_drop_sources(AxlTcp *sock)
{
    if (sock->send_source > 0) {
        axl_loop_remove_source(sock->send_source_loop, sock->send_source);
        sock->send_source = 0;
    }
    sock->send_source_loop = NULL;
}

bool
axl_tcp_send_close_drops(const AxlTcp *sock)
{
    return sock->send_queued_head != NULL
           || (sock->send_active != NULL
               && sock->send_active->committed < sock->send_active->len)
           || sock->stream_torn;
}

/* Stop the ACTIVE send where it stands, without touching the firmware: its
   armed piece becomes the socket's orphan and goes out on its own. Returns
   the status the send is retired with -- AXL_OK when every byte was already
   handed over (nothing is left to stop), else AXL_CANCELLED.

   The armed piece may ALREADY have signalled: its completion and the cancel
   can become ready in one loop turn, and the round-robin dispatch runs
   either first. Then there is no orphan -- the completion is taken here --
   and a send whose last piece TCP has taken is FINISHED, with the status that
   piece really completed with. Without this a sync axl_tcp_send whose final
   Transmit completed just as its timeout fired reported AXL_TIMEOUT. */
static AxlStatus
tcp_send_stop_active(AxlTcp *sock)
{
    AxlTcpToken *tok = sock->send_active;
    bool         signalled;

    sock->send_active = NULL;
    axl_tcp_send_drop_sources(sock);

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);
    signalled = axl_backend_event_check(
                    (AxlEventHandle)sock->tx_token.CompletionToken.Event) == 0;
    if (!signalled) {
        sock->tx_orphan = true;
        return (tok->committed == tok->len) ? AXL_OK : AXL_CANCELLED;
    }
    sock->tx_inflight = false;
    if (tok->committed < tok->len) {
        return AXL_CANCELLED;   /* the rest was never handed over */
    }
    tok->finished = !EFI_ERROR(sock->tx_token.CompletionToken.Status);
    return tok->finished ? AXL_OK : AXL_ERR;
}

/* A cancellable fired on a send. Port of SockCancelToken for a token still on
   SndTokenList -- retired AXL_CANCELLED, nothing of it sent. For the token on
   the wire it deliberately is NOT a port: SockCancelToken on a Transmit TcpDxe
   is still processing frees the token and leaves its data queued, and the
   next segmentation's SockDataSent ASSERTs on it (DEBUG) or signals freed
   memory (RELEASE). So the active send stops at a piece boundary instead
   (tcp_send_stop_active). Same contract as recv cancel — the socket stays
   valid and connected. */
static bool
on_send_token_cancel(void *data)
{
    AxlTcpToken *tok  = data;
    AxlTcp      *sock = tok->sock;

    if (sock->send_active == tok) {
        tcp_send_retire(sock, tok, tcp_send_stop_active(sock));
        /* Without this the queue would stall: promotion otherwise only ever
           happens from a normal completion, so cancelling the ACTIVE send left
           every token behind it waiting for a callback that could not come. */
        tcp_send_promote(sock);
    } else if (tcp_send_unlink(sock, tok)) {
        tcp_send_retire(sock, tok, AXL_CANCELLED);
        /* The queue head may have changed, and an orphan piece's watch and
           the pacer follow it (the cancelled send's loop may be about to go
           away -- a sync axl_tcp_send's is). */
        tcp_send_promote(sock);
    }
    /* Neither list holds it: already retired, and its callback is queued or
       has run. Nothing to do. */
    return AXL_SOURCE_REMOVE;
}

bool
axl_tcp_send_drop_token(AxlTcp *sock, AxlTcpCallback cb, void *cb_data)
{
    AxlTcpToken *tok = sock->send_active;

    /* On the wire: stop it where it stands and start the next send — the
       same sequence a cancellable takes, minus the callback. */
    if (tok != NULL && tok->cb == cb && tok->cb_data == cb_data) {
        (void)tcp_send_stop_active(sock);
        sock->send_last_sent     = tok->committed;
        sock->send_last_finished = tok->finished;
        tcp_send_note_torn(sock, tok);
        tcp_send_free_token(tok);
        tcp_send_promote(sock);
        return true;
    }
    for (tok = sock->send_queued_head; tok != NULL; tok = tok->next) {
        if (tok->cb == cb && tok->cb_data == cb_data) {
            (void)tcp_send_unlink(sock, tok);
            sock->send_last_sent     = 0;
            sock->send_last_finished = false;
            tcp_send_free_token(tok);
            /* Nothing may stay behind on the dying loop -- the orphan watch
               and the pacer follow the queue head, which just changed. */
            tcp_send_promote(sock);
            return true;
        }
    }
    /* Retired but not yet delivered — the completion was dispatched and the
       loop stopped before its callback came up. Same dead context, same
       treatment; freeing the token cancels the delivery it had scheduled. */
    for (tok = sock->send_done_head; tok != NULL; tok = tok->next) {
        if (tok->cb == cb && tok->cb_data == cb_data) {
            (void)tcp_send_done_unlink(sock, tok);
            sock->send_last_sent     = tok->committed;
            sock->send_last_finished = tok->finished;
            tcp_send_free_token(tok);
            return true;
        }
    }
    return false;
}

void
axl_tcp_send_flush(AxlTcp *sock, bool graceful)
{
    AxlTcpToken *tok = sock->send_active;

    /* PARK rather than retire: these callbacks are delivered below, in this
       call, so scheduling each on its loop first would only mean cancelling it
       again a line later — and on a socket with a deep queue that is a burst of
       defer-ring churn for nothing.

       The ACTIVE send first — FIFO, it was accepted before everything queued
       behind it. Its Transmit is never cancelled: an abortive close's
       Close(Abort) retires it, and a graceful close only happens when the
       active send is fully armed, so TCP sends that last piece ahead of the
       FIN and the send is done -- AXL_OK, or the failure its last Transmit
       already completed with. An abortive close discards that piece with the
       rest: AXL_CANCELLED, every byte still counted as committed. */
    if (tok != NULL) {
        AxlStatus status = AXL_CANCELLED;

        sock->send_active = NULL;
        if (graceful) {
            status = AXL_OK;
            if (axl_backend_event_check(
                    (AxlEventHandle)sock->tx_token.CompletionToken.Event) == 0) {
                tok->finished     = !EFI_ERROR(sock->tx_token.CompletionToken.Status);
                status            = tok->finished ? AXL_OK : AXL_ERR;
                sock->tx_inflight = false;
            }
        }
        tcp_send_park(sock, tok, status);
    }
    while ((tok = tcp_send_dequeue(sock)) != NULL) {
        tcp_send_park(sock, tok, AXL_CANCELLED);
    }
    /* Nothing is on the wire any more: stop pacing this socket before any
       callback below can free the loop it was paced on. */
    tcp_pace_update(sock);

    /* One pass covers everything: no callback delivered here can add another
       retirement, because the socket is already marked closed and a send from
       one of them is refused rather than accepted.

       `sock` is still alive on return — the only close a callback could have
       triggered is a nested one, which returns without finalizing (see
       tcp_close_impl). */
    axl_tcp_send_drain_done(sock);
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
    size_t queued_before;

    if (sock == NULL || buf == NULL || size == 0 || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }
    /* Refuse rather than arm firmware state this socket has already released.
       Reachable from a callback the teardown itself fired. */
    if (sock->closed) {
        return AXL_ERR;
    }

    /* ACCEPTED, never refused. Port of EDK2 SockSend (SockInterface.c: "process
       this sending token now or buffer it only?"), which below low-water
       buffers into SndTokenList and returns EFI_SUCCESS — its only failure is
       EFI_OUT_OF_RESOURCES from the token allocation. So every send is queued
       and then offered to the transport: if the slot is free this promotes
       straight through and Transmits before returning, and if it is not, these
       bytes wait their turn behind the sends accepted before them.

       An earlier cut of this returned AXL_BUSY at high-water. That single
       deviation is what kept AXL_BUSY handling alive everywhere above:
       axl_tls_write_async needed a capacity floor before encrypting, the WS
       layer needed a buffer-on-refusal path, and the four submit sites in
       axl-http-response.c had to interpret a status they got wrong (§1a). Not
       refusing at all deletes that whole class of caller complexity, which is
       the point of the design. docs/AXL-Tcp-Queue-Design.md §3.2. */
    queued_before = sock->send_queued_bytes;
    if (tcp_send_enqueue(sock, buf, size, loop, cancel, cb, data) != AXL_OK) {
        return AXL_ERR;
    }
    tcp_send_promote(sock);

    /* The queue is unbounded by design (§3.2), so nothing above fails for
       depth — but a queue growing without limit means the peer is not
       draining, and that is worth seeing rather than discovering as memory
       pressure. Measured AFTER the promotion, so a send that went straight to
       the wire never counts; logged once per crossing, not per send. */
    if (sock->send_queued_bytes >= TCP_SEND_QUEUE_WARN_BYTES &&
        queued_before < TCP_SEND_QUEUE_WARN_BYTES) {
        axl_warning("async send: outbound queue past %u KiB (%zu bytes, peer "
                    "not draining)",
                    TCP_SEND_QUEUE_WARN_BYTES / 1024u,
                    sock->send_queued_bytes);
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

    _axl_net_latency_activity();

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
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
        cb(NULL, AXL_ERR, udata);  /* connect is one-shot; return value ignored */
        return AXL_SOURCE_REMOVE;
    }

    sock->on_connect(sock, 0, sock->connect_data);  /* connect is one-shot */
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
    axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
    cb(NULL, AXL_CANCELLED, udata);  /* cancel is terminal; return value ignored */
    return AXL_SOURCE_REMOVE;
}

int
axl_tcp_connect_addr_async(
    const AxlIPv4Address  *dest,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data,
    AxlTcp               **out_pending
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

    if (dest == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    remote_addr = *dest;

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
    _axl_net_latency_link(sock);

    //
    // Create connect event
    //
    conn_event = NULL;
    if (axl_backend_event_create((AxlEventHandle *)&conn_event) != AXL_OK) {
        axl_error("async connect: cannot create event");
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
        return AXL_ERR;
    }

    sock->conn_token.CompletionToken.Event  = conn_event;
    sock->conn_token.CompletionToken.Status = EFI_ABORTED;

    _axl_net_latency_activity();
    status = axl_efi_call(tcp4->Connect, 2, tcp4, &sock->conn_token);
    if (EFI_ERROR(status)) {
        axl_error("async Connect: %llx", (unsigned long long)status);
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
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
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
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

    /* Hand the in-progress socket back so a sync wrapper can drive
       tcp4->Poll() while its ephemeral loop blocks at a raised TPL. Set
       only here, on the success path — every error return above already
       tore the socket down. */
    if (out_pending != NULL) {
        *out_pending = sock;
    }

    return AXL_OK;
}

int
axl_tcp_connect_async_via_ex(
    const char            *host,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data,
    AxlTcp               **out_pending
    )
{
    AxlIPv4Address remote_addr;

    if (host == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    /* Resolve synchronously, then connect to the address. This is the
       blocking shape the sync TCP wrappers want; the async HTTP client
       avoids the nested resolve by calling axl_net_resolve_async followed
       by axl_tcp_connect_addr_async directly. */
    if (axl_net_resolve(host, &remote_addr) != AXL_OK) {
        axl_error("async connect: cannot resolve '%s'", host);
        return AXL_ERR;
    }

    return axl_tcp_connect_addr_async(&remote_addr, port, source_ip, loop,
                                      cancel, cb, data, out_pending);
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
    return axl_tcp_connect_async_via_ex(host, port, source_ip, loop, cancel,
                                        cb, data, NULL);
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

size_t
axl_tcp_send_get_sent(const AxlTcp *sock)
{
    return (sock != NULL) ? sock->send_last_sent : 0;
}

size_t
axl_tcp_send_queued_bytes(const AxlTcp *sock)
{
    size_t n;

    if (sock == NULL || sock->closed || sock->is_listener) {
        return 0;
    }
    n = sock->send_queued_bytes;
    if (sock->send_active != NULL) {
        n += sock->send_active->len - sock->send_active->committed;
    }
    return n;
}
