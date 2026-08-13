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
    AxlSourceId     saved_recv_src   = sock->recv_source;
    AxlSourceId     saved_cancel_src = sock->recv_cancel_source;

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

/* Bound on a single UEFI Transmit. A larger async send is split into a chain
   of Transmits of at most this many bytes so no one Transmit is unbounded —
   an unbounded Transmit that a slow / stalled client can't drain monopolizes
   the shared one-send-in-flight path and wedges the single-threaded loop. */
#define TCP_SEND_CHUNK_MAX  (32u * 1024u)

/* Arm the next Transmit for the active send: submit
   min(TCP_SEND_CHUNK_MAX, remaining) bytes from where the last chunk stopped,
   reusing tx_token/tx_event. Returns the EFI Transmit status. Does NOT touch
   the loop source — the caller owns source lifecycle. */
static EFI_STATUS
tcp_send_arm_chunk(AxlTcp *sock)
{
    AxlTcpToken *tok   = sock->send_active;
    size_t       chunk = (tok->remaining < TCP_SEND_CHUNK_MAX)
                         ? tok->remaining : TCP_SEND_CHUNK_MAX;

    sock->tx_frag.FragmentLength = (uint32_t)chunk;
    sock->tx_frag.FragmentBuffer = (void *)(tok->buf + (tok->len - tok->remaining));

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

    return axl_efi_call(sock->tcp4->Transmit, 2, sock->tcp4, &sock->tx_token);
}

static bool on_send_complete(void *data);
static bool on_send_token_cancel(void *data);
static void tcp_send_promote(AxlTcp *sock);

/* Append an accepted send. Port of EDK2 SockBufferToken into SndTokenList.
   The caller's buffer is BORROWED, not copied — EDK2's NetbufFromExt wraps
   rather than copies (design §3.1), so a send costs one small token and no
   data copy. Every send lands here first, including the one that is about to
   be promoted straight out again: that is what keeps the retirement path
   allocation-free (see AxlTcpToken). */
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
    tok->remaining     = size;
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
        tcp_send_free_token(tok);
        cb(sock, status, udata);  /* send is one-shot; return value ignored */
    }

    if (!outer) {
        return;      /* a nested delivery leaves the flags to the outermost */
    }
    sock->send_draining = false;
    if (sock->free_deferred) {
        /* A close ran from one of the callbacks above and finished its
           teardown, but could not free the socket while this walk was still
           handing it out. finalize_sock left that to us. */
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

    if (sock->send_done_tail != NULL) {
        sock->send_done_tail->next = tok;
    } else {
        sock->send_done_head = tok;
    }
    sock->send_done_tail = tok;
}

/* Retire @p tok: park it, then ask ITS OWN loop to deliver the callback on the
   loop's next iteration. */
static void
tcp_send_retire(AxlTcp *sock, AxlTcpToken *tok, AxlStatus status)
{
    tcp_send_park(sock, tok, status);

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
   caller, and must not park every send behind it (design §6b defect 5). */
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

    if (EFI_ERROR(tcp_send_arm_chunk(sock))) {
        axl_debug("async Transmit failed to arm %zu byte(s)", tok->len);
        goto arm_failed;
    }

    sock->send_source = axl_loop_add_event(
        tok->loop,
        (AxlEventHandle)sock->tx_token.CompletionToken.Event,
        on_send_complete, sock);
    if (sock->send_source == 0) {
        axl_error("async send: cannot register event with loop");
        /* The firmware still holds the token and would DMA the borrowed
           buffer after we have declared the send dead, and the next send
           would reuse tx_token while it is still outstanding. */
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        goto arm_failed;
    }
    return true;

arm_failed:
    sock->send_active = NULL;
    tcp_send_retire(sock, tok, AXL_ERR);
    return false;
}

/* Hand the head of the queue to the transport. Port of EDK2
   SockProcessSndToken. AXL keeps exactly one EFI token in flight (design §2),
   so at most one send is armed here and the next promotion happens from that
   send's own completion; the loop only spins while tokens are failing to arm. */
static void
tcp_send_promote(AxlTcp *sock)
{
    while (sock->send_active == NULL && !sock->closed) {
        AxlTcpToken *tok = tcp_send_dequeue(sock);

        if (tok == NULL || tcp_send_arm_token(sock, tok)) {
            return;
        }
    }
}

static bool
on_send_complete(void *data)
{
    AxlTcp      *sock = data;
    AxlTcpToken *tok  = sock->send_active;
    EFI_STATUS   status;

    axl_efi_call(sock->tcp4->Poll, 1, sock->tcp4);

    status = sock->tx_token.CompletionToken.Status;
    if (!EFI_ERROR(status)) {
        /* UEFI Transmit tokens are all-or-nothing per submission: this
           completion means the whole submitted chunk was accepted. Retire
           exactly that chunk's length — identical to what tcp_send_arm_chunk
           just submitted. */
        size_t sent = (tok->remaining < TCP_SEND_CHUNK_MAX)
                      ? tok->remaining : TCP_SEND_CHUNK_MAX;
        tok->remaining -= sent;

        if (tok->remaining > 0) {
            /* More to go — re-arm the next chunk on the SAME tx_event and keep
               this loop source (AXL_SOURCE_CONTINUE); the user callback fires
               only after the final chunk completes. */
            status = tcp_send_arm_chunk(sock);
            if (!EFI_ERROR(status)) {
                return AXL_SOURCE_CONTINUE;
            }
            axl_error("async send: chunk re-arm failed at %zu/%zu: %llx",
                      tok->len - tok->remaining, tok->len,
                      (unsigned long long)status);
            /* fall through: report the re-arm failure as a send failure */
        }
    } else {
        axl_error("async send failed: %llx", (unsigned long long)status);
    }

    sock->send_active = NULL;
    axl_tcp_send_drop_sources(sock);
    tcp_send_retire(sock, tok, EFI_ERROR(status) ? AXL_ERR : AXL_OK);
    /* Promote immediately: only the NOTIFICATION is deferred, so the wire
       keeps moving at full rate while the callback waits for the next tick. */
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
        axl_loop_remove_source(sock->async_loop, sock->send_source);
        sock->send_source = 0;
    }
}

/* A cancellable fired on a send. Port of SockCancelToken, which handles a
   token on either list: cancel the transport token if this send is the one on
   the wire, then retire it with AXL_CANCELLED. Same contract as recv cancel —
   the socket stays valid and connected. */
static bool
on_send_token_cancel(void *data)
{
    AxlTcpToken *tok  = data;
    AxlTcp      *sock = tok->sock;

    if (sock->send_active == tok) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        sock->send_active = NULL;
        axl_tcp_send_drop_sources(sock);
        tcp_send_retire(sock, tok, AXL_CANCELLED);
        /* Without this the queue would stall: promotion otherwise only ever
           happens from a normal completion, so cancelling the ACTIVE send left
           every token behind it waiting for a callback that could not come. */
        tcp_send_promote(sock);
    } else if (tcp_send_unlink(sock, tok)) {
        tcp_send_retire(sock, tok, AXL_CANCELLED);
    }
    /* Neither list holds it: already retired, and its callback is queued or
       has run. Nothing to do. */
    return AXL_SOURCE_REMOVE;
}

bool
axl_tcp_send_drop_token(AxlTcp *sock, AxlTcpCallback cb, void *cb_data)
{
    AxlTcpToken *tok = sock->send_active;

    /* On the wire: cancel the transport token and start the next send — the
       same sequence a cancellable takes, minus the callback. */
    if (tok != NULL && tok->cb == cb && tok->cb_data == cb_data) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        sock->send_active = NULL;
        axl_tcp_send_drop_sources(sock);
        tcp_send_free_token(tok);
        tcp_send_promote(sock);
        return true;
    }
    for (tok = sock->send_queued_head; tok != NULL; tok = tok->next) {
        if (tok->cb == cb && tok->cb_data == cb_data) {
            (void)tcp_send_unlink(sock, tok);
            tcp_send_free_token(tok);
            return true;
        }
    }
    /* Retired but not yet delivered — the completion was dispatched and the
       loop stopped before its callback came up. Same dead context, same
       treatment; freeing the token cancels the delivery it had scheduled. */
    for (tok = sock->send_done_head; tok != NULL; tok = tok->next) {
        if (tok->cb == cb && tok->cb_data == cb_data) {
            (void)tcp_send_done_unlink(sock, tok);
            tcp_send_free_token(tok);
            return true;
        }
    }
    return false;
}

void
axl_tcp_send_flush(AxlTcp *sock, AxlStatus status)
{
    AxlTcpToken *tok = sock->send_active;

    /* PARK rather than retire: these callbacks are delivered below, in this
       call, so scheduling each on its loop first would only mean cancelling it
       again a line later — and on a socket with a deep queue that is a burst of
       defer-ring churn for nothing.

       The ACTIVE send first — FIFO, it was accepted before everything queued
       behind it. Its transport token is cancelled by the caller. */
    if (tok != NULL) {
        sock->send_active = NULL;
        tcp_send_park(sock, tok, status);
    }
    while ((tok = tcp_send_dequeue(sock)) != NULL) {
        tcp_send_park(sock, tok, status);
    }

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
