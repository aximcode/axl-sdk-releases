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

/* Forward-declared exactly as src/event/axl-wait.c does — _axl_loop_in_callback
   lives in the src/loop-only axl-loop-internal.h, which src/net cannot include.
   True when a loop is dispatching somewhere in this (single-threaded) call
   stack; used to pick the loop-FREE close-completion below. */
bool _axl_loop_in_callback(void);

/* 10 ms Poll cadence between close-completion checks — matches the Tier-4
   per-protocol wait helpers (axl-net-wait.c). */
#define AXL_TCP_CLOSE_POLL_US  10000ULL

/* Inline wait cap for an ABORTIVE close. A RST completes with no TIME_WAIT, so
   the completion event signals almost immediately; this only bounds a wedged
   firmware. Much shorter than the graceful 3 s (which had to cover TIME_WAIT). */
#define AXL_TCP_ABORT_WAIT_US  (1000ULL * 1000ULL)

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
typedef struct AxlTcpCloseCtx {
    EFI_TCP4_CLOSE_TOKEN  close_token;
    AxlTcp               *sock;
    AxlLoop              *loop;       /* set on the async path */
    AxlSourceId           source_id;  /* set on the async path */
    /* Deferred-close registry links (async path only). Every in-flight
       loop-owned close is on g_deferred_closes so an abortive listener teardown
       can find and finalize its own children's deferred closes synchronously.
       reg_listener_id is the accepting listener's id (from sock->listener_id),
       captured here because finalize frees sock. */
    uint64_t                     reg_listener_id;
    struct AxlTcpCloseCtx        *reg_prev;
    struct AxlTcpCloseCtx        *reg_next;
} AxlTcpCloseCtx;

/* Monotonic listener id source (single-threaded UEFI, no lock). 0 is reserved
   for "no listener" (client sockets), so ids start at 1. 64-bit — never wraps
   in a realistic process, so a listener id can never alias a freed one. */
static uint64_t g_next_listener_id = 1;

/* Intrusive list of in-flight loop-deferred closes (single-threaded UEFI, no
   lock). A ctx is linked when tcp_close_impl registers on_close_event and
   unlinked in finalize_close_ctx — whether that runs from on_close_event or
   from the abortive listener-teardown sweep. */
static AxlTcpCloseCtx *g_deferred_closes;

static void
deferred_close_link(AxlTcpCloseCtx *ctx)
{
    ctx->reg_prev = NULL;
    ctx->reg_next = g_deferred_closes;
    if (g_deferred_closes != NULL) {
        g_deferred_closes->reg_prev = ctx;
    }
    g_deferred_closes = ctx;
}

static void
deferred_close_unlink(AxlTcpCloseCtx *ctx)
{
    if (ctx->reg_prev != NULL) {
        ctx->reg_prev->reg_next = ctx->reg_next;
    } else if (g_deferred_closes == ctx) {
        g_deferred_closes = ctx->reg_next;
    }
    if (ctx->reg_next != NULL) {
        ctx->reg_next->reg_prev = ctx->reg_prev;
    }
    ctx->reg_prev = ctx->reg_next = NULL;
}

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

/* 10 ms — matches the UDP sync wrapper's Poll-tick cadence (udp_sync_poll_tick). */
#define AXL_TCP_SYNC_POLL_TICK_MS  10u

/* Drive the TCP4 state machine while a sync wrapper's ephemeral loop is
   blocked. In the foreground the firmware notify advances connect/send/recv
   below TPL_APPLICATION and this tick is a cheap no-op (Poll() on an idle
   socket). But when a sync TCP op runs at a raised TPL — called from inside
   a driver-pump notify dispatched at TPL_CALLBACK — the loop's blocking wait
   holds TPL_CALLBACK continuously and starves that notify, so nothing would
   advance tcp4 and the op would stall to its full timeout. The periodic
   Poll() drives it, exactly as the UDP sync wrapper does for UDP. The tick only
   exists for the duration of the blocking wait (the wrapper removes it
   before freeing the loop), so it adds no steady-state work. */
static bool
tcp_sync_poll_tick(void *data)
{
    EFI_TCP4_PROTOCOL *tcp4 = (EFI_TCP4_PROTOCOL *)data;
    axl_efi_call(tcp4->Poll, 1, tcp4);
    return AXL_SOURCE_CONTINUE;
}

/* Arm the Poll() tick on @p loop for @p tcp4 — but ONLY at a raised TPL,
   where it is actually needed. At TPL_APPLICATION the firmware notify drives
   the op for free, so installing a 10 ms timer there would just burn CPU
   (a long foreground recv would wake 100×/s for nothing). Returns the tick's
   source id, or 0 when not armed. Pair with tcp_sync_disarm_poll_tick. */
static AxlSourceId
tcp_sync_arm_poll_tick(AxlLoop *loop, EFI_TCP4_PROTOCOL *tcp4)
{
    if (!axl_backend_at_raised_tpl()) {
        return 0;
    }
    return axl_loop_add_timer(loop, AXL_TCP_SYNC_POLL_TICK_MS,
                              tcp_sync_poll_tick, tcp4);
}

static void
tcp_sync_disarm_poll_tick(AxlLoop *loop, AxlSourceId poll_src)
{
    if (poll_src != 0) {
        axl_loop_remove_source(loop, poll_src);
    }
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

#define AXL_TCP_CONNECT_TIMEOUT_DEFAULT_MS  10000u

int
axl_tcp_connect_timeout(const char *host, uint16_t port,
                        const AxlIPv4Address *source_ip,
                        size_t connect_timeout_ms,
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

    AxlTcp *pending = NULL;
    if (axl_tcp_connect_async_via_ex(host, port, source_ip, loop, cancel,
                                     on_sync_complete, &r, &pending) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return AXL_ERR;
    }

    /* Drive tcp4->Poll() while we block, so the connect advances even at a
       raised TPL (see tcp_sync_poll_tick). pending->tcp4 is captured now,
       while the socket is alive; the tick stops firing once the loop quits
       on completion (which is also where a failed connect frees the sock),
       so it never polls a freed tcp4. pending/tcp4 are non-NULL on the AXL_OK
       return above; the guard degrades to no tick (not a crash) should a
       future async-connect error path ever return OK without them. */
    AxlSourceId poll_src = (pending != NULL && pending->tcp4 != NULL)
                            ? tcp_sync_arm_poll_tick(loop, pending->tcp4)
                            : 0;

    /* 0 means "use the default", not "wait forever" — an unbounded connect
       is rarely wanted, and a hung SYN would otherwise stall the loop. */
    size_t ct = (connect_timeout_ms > 0)
                    ? connect_timeout_ms : AXL_TCP_CONNECT_TIMEOUT_DEFAULT_MS;
    axl_loop_add_timeout(loop, ct, on_sync_cancel_timeout, cancel);
    axl_loop_run(loop);

    tcp_sync_disarm_poll_tick(loop, poll_src);

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
axl_tcp_connect_via(const char *host, uint16_t port,
                    const AxlIPv4Address *source_ip,
                    AxlTcp **out_sock)
{
    /* Convenience form — the 10 s default connect timeout. */
    return axl_tcp_connect_timeout(host, port, source_ip, 0, out_sock);
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
    sock->listener_id = g_next_listener_id++;   /* unique; children inherit it */

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
    /* Drive the listener's TCP4 so a pending accept advances at a raised
       TPL too (see tcp_sync_poll_tick). */
    AxlSourceId poll_src = tcp_sync_arm_poll_tick(loop, listener->tcp4);
    axl_loop_run(loop);
    tcp_sync_disarm_poll_tick(loop, poll_src);

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

    /* This wrapper does not queue. Behind another caller's send it would be
       accepted, queued, and then never started: the promotion is driven by the
       ACTIVE send's completion source, which sits on that caller's loop, and
       the loop running here is this call's ephemeral one. The result was the
       full timeout burned with nothing sent (design §6b defect 4) — worse than
       the fail-fast this restores, because the caller learns 10 s late.

       Not a transport refusal: axl_tcp_send_async still accepts every send
       (design §3.2). It is the SYNCHRONOUS shell declining a job it cannot
       finish inside its own call, which is what §7 means by "the sync wrapper
       must opt OUT of queueing". A socket with more than one writer wants the
       async API. */
    if (axl_tcp_send_pending(sock)) {
        axl_debug("sync send: another send owns the transport - refusing");
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
    /* Drive tcp4->Poll() so the send advances at a raised TPL too
       (see tcp_sync_poll_tick). */
    AxlSourceId poll_src = tcp_sync_arm_poll_tick(loop, sock->tcp4);
    axl_loop_run(loop);
    tcp_sync_disarm_poll_tick(loop, poll_src);

    /* The loop can return with the send still outstanding — Ctrl-C, or any
       other source quitting it — and everything the token points at dies on
       the next three lines: the cancellable its cancel source is armed on, the
       loop that source and its drain sit on, and the `r` frame its callback
       would write into. The timeout path leaves nothing behind (the cancel
       retires it), so this is the abnormal exit only; drop the token without
       its callback, which is exactly the case axl_tcp_send_drop_token exists
       for. r.status stays AXL_ERR: the send did not complete. */
    if (!r.done) {
        (void)axl_tcp_send_drop_token(sock, on_sync_complete, &r);
    }

    axl_cancellable_free(cancel);
    axl_loop_free(loop);
    sock->async_loop = saved_loop;

    /* Every send path routes its source teardown through
       axl_tcp_send_drop_sources, so send_source is already zero. Even if one
       didn't, source ids are process-globally unique (axl-loop.c), so a stale
       id can't collide with another loop's source — no post-teardown clearing
       needed here. */

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
    /* Drive tcp4->Poll() so the recv advances at a raised TPL too
       (see tcp_sync_poll_tick). */
    AxlSourceId poll_src = tcp_sync_arm_poll_tick(loop, sock->tcp4);
    axl_loop_run(loop);
    tcp_sync_disarm_poll_tick(loop, poll_src);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);
    sock->async_loop = saved_loop;

    /* on_recv_complete's !keep branch can't zero recv_source/recv_cancel_source
       (it must not touch a sock the callback may have freed), so they still
       hold ids from the now-freed ephemeral loop. The cross-loop-removal hazard
       this used to create (axl_tcp_close deleting a colliding id off the
       restored outer loop) is gone — source ids are now process-globally unique
       (axl-loop.c), so a stale id matches nothing on another loop. We still zero
       them here (the sock is valid in this sync context) for a separate reason:
       the NEXT sync recv on this sock checks recv_source > 0 and would otherwise
       issue a needless tcp4->Cancel on the already-completed token
       (axl_tcp_recv_async re-arm path). */
    sock->recv_source        = 0;
    sock->recv_cancel_source = 0;

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
    /* A retired-send drain is walking this socket's callbacks and still hands
       `sock` to each of them — freeing here would hand the rest a dangling
       pointer. The drain does the free once its last callback has returned
       (axl_tcp_send_drain_done). Clear the firmware handles on the way out:
       the teardown above already released them, and a later callback asking
       this socket for, say, its peer address must get a clean failure rather
       than a call into a destroyed child. */
    if (sock->send_draining) {
        sock->tcp4          = NULL;
        sock->tcp_handle    = NULL;
        sock->free_deferred = true;
        return;
    }
    axl_free(sock);
}

static void
finalize_close_ctx(AxlTcpCloseCtx *ctx)
{
    /* Drop from the deferred-close registry first (no-op for a sync-path ctx
       that was never linked). Idempotent — a ctx is finalized exactly once. */
    deferred_close_unlink(ctx);
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

// Finalize every in-flight loop-deferred close spawned from @p listener,
// synchronously and loop-free — the third port-holder category an abortive
// listener teardown must clear (connections served then gracefully closed, whose
// close went async and whose slot the server already dropped). Each such close
// keeps a firmware PCB on the listen port and a caller-owned loop source until
// its on_close_event fires (~TIME_WAIT, needs a pump). Here we remove the loop
// source and finalize now: Configure(NULL)+DestroyChild in finalize_sock force-
// drops the PCB even mid-graceful-close (dropping the un-ACKed FIN, which is the
// intended trade for an immediate teardown — same as an abortive close). Scoped
// by reg_listener so a sibling server on the same loop/NIC is untouched.
static void
finalize_listener_deferred_closes(AxlTcp *listener)
{
    AxlTcpCloseCtx *ctx = g_deferred_closes;
    while (ctx != NULL) {
        AxlTcpCloseCtx *next = ctx->reg_next;   /* snapshot: finalize unlinks ctx */
        if (ctx->reg_listener_id == listener->listener_id) {
            /* Drop the loop source before finalizing so on_close_event can never
               fire against the freed ctx, and so it is gone from the loop (the
               "zero caller-owned sources at loop_free" invariant). */
            if (ctx->loop != NULL && ctx->source_id != 0) {
                axl_loop_remove_source(ctx->loop, ctx->source_id);
            }
            finalize_close_ctx(ctx);
        }
        ctx = next;
    }
}

/* Loop-FREE close completion: drive tcp4->Poll() and poll the close_event
   directly until it signals or the deadline passes. Unlike _axl_tcp_wait (which
   spins up an ephemeral axl_loop_new() to multiplex event+timeout+tick), this
   nests NO loop — so it is safe from inside a loop dispatch (a sync request's
   ephemeral-loop callback, the connect-fail/cancel teardown) at a raised TPL:
   it neither nests a second loop (the re-entrancy artifact the async effort
   removed) nor leaves a close-event source on a loop that is freed before the
   close completes (the leak). It dispatches no loop sources, so it cannot
   re-enter the caller's loop either. The close is short-lived — a passive
   Connection: close signals almost immediately; @p timeout_us only guards a
   stuck active close. */
static void
close_wait_inline(EFI_TCP4_PROTOCOL *tcp4, EFI_EVENT close_event,
                  uint64_t timeout_us)
{
    /* Bound by iteration count (timeout / poll cadence), not a wall clock, so a
       failed monotonic read can never turn this into an unbounded spin. */
    uint64_t max_iters = (timeout_us / AXL_TCP_CLOSE_POLL_US) + 1;
    for (uint64_t i = 0; i < max_iters; i++) {
        axl_efi_call(tcp4->Poll, 1, tcp4);
        int rc = axl_backend_event_check((AxlEventHandle)close_event);
        if (rc <= 0) {
            return;  /* 0 = close completed; <0 = bad handle, don't spin */
        }
        axl_backend_stall(AXL_TCP_CLOSE_POLL_US);
    }
}

// ---------------------------------------------------------------------------
// Synchronous accept-backlog drain (abortive listener teardown only).
//
// A listen socket that is torn down under load holds the port through its
// firmware ACCEPT BACKLOG: connections whose 3-way handshake the firmware
// completed on its own periodic timer but that the app never pulled via Accept
// (no loop pump between free and rebind). Those established children hold PCBs
// on the listen port, and the plain listener close closes them GRACEFULLY (FIN
// -> ~2 s TIME_WAIT) — so the port is not free on return, defeating the whole
// point of the abortive path under concurrent load.
//
// Drain them here: pull each queued connection through the listener's already-
// armed Accept token (tcp4->Poll drives the stack; event_check detects a
// completion without the app loop) and RST it via the proven abortive close.
// Bounded so a peer that keeps connecting during teardown cannot spin us
// forever; the still-armed trailing Accept token is cancelled by the Phase-1
// accept teardown in tcp_close_impl below.
//
// The caller (tcp_close_impl PHASE 0) Configure(NULL)s the listener BEFORE
// calling this, so the firmware has stopped completing new handshakes and the
// backlog is a frozen, finite set — this loop drains that set and converges,
// rather than racing an ongoing inbound-SYN storm (which is what wedged the
// RESET free before the quiesce was added). The <=DRAIN_MAX bound is a
// belt-and-suspenders cap so the free returns even if some firmware keeps the
// Accept path partly live after Configure(NULL).
// ---------------------------------------------------------------------------

#define AXL_TCP_BACKLOG_DRAIN_MAX   64u   ///< max backlog children pulled per teardown
#define AXL_TCP_BACKLOG_POLL_TRIES  8u    ///< Poll attempts per pull before "backlog empty"

// RST + DestroyChild one pulled backlog child. Wraps the bare firmware handle in
// a throwaway AxlTcp and reuses axl_tcp_close(., AXL_TEARDOWN_RESET) (RST + inline finalize +
// DestroyChild + free) so the child gets exactly the same port-releasing
// teardown as a tracked connection, with none of its guarantees re-derived.
static void
drain_reset_child(AxlTcp *listener, EFI_HANDLE child_handle)
{
    EFI_TCP4_PROTOCOL *child_tcp4 = NULL;
    if (axl_efi_call(axl_bs()->HandleProtocol, 3, child_handle,
                     &gEfiTcp4ProtocolGuid, (void **)&child_tcp4) == EFI_SUCCESS
        && child_tcp4 != NULL) {
        AxlTcp *child = axl_calloc(1, sizeof(AxlTcp));
        if (child != NULL) {
            child->tcp4        = child_tcp4;
            child->tcp_handle  = child_handle;
            child->tcp_sb      = listener->tcp_sb;
            child->sb_handle   = listener->sb_handle;
            child->is_listener = false;
            axl_tcp_close(child, AXL_TEARDOWN_RESET);
            return;
        }
    }
    /* Wrap failed (OOM / no protocol) — at least destroy the child so it does
       not leak; DestroyChild alone still tears down its firmware PCB. */
    axl_efi_call(listener->tcp_sb->DestroyChild, 2, listener->tcp_sb,
                 child_handle);
}

static void
drain_accept_backlog(AxlTcp *listener)
{
    if (listener == NULL || !listener->is_listener
        || listener->tcp4 == NULL || listener->tcp_sb == NULL
        || listener->acc_token.CompletionToken.Event == NULL) {
        return;
    }
    EFI_EVENT ev = listener->acc_token.CompletionToken.Event;

    for (unsigned drained = 0; drained < AXL_TCP_BACKLOG_DRAIN_MAX; drained++) {
        bool completed = false;
        for (unsigned p = 0; p < AXL_TCP_BACKLOG_POLL_TRIES; p++) {
            axl_efi_call(listener->tcp4->Poll, 1, listener->tcp4);
            /* event_check: 0 = signalled (a connection was accepted),
               >0 = not ready, <0 = bad handle. */
            if (axl_backend_event_check((AxlEventHandle)ev) == 0) {
                completed = true;
                break;
            }
            /* No stall after the last poll — we are about to give up on this
               slot, so the final sleep would just delay teardown. */
            if (p + 1 < AXL_TCP_BACKLOG_POLL_TRIES) {
                axl_backend_stall(AXL_TCP_CLOSE_POLL_US);
            }
        }
        if (!completed) {
            break;  /* backlog drained (the trailing Accept token is cancelled
                       by the Phase-1 accept teardown in tcp_close_impl). */
        }

        EFI_HANDLE child_handle = listener->acc_token.NewChildHandle;
        EFI_STATUS acc_status   = listener->acc_token.CompletionToken.Status;

        /* Re-arm the same token/event so the next queued connection can bind. */
        axl_memset(&listener->acc_token, 0, sizeof(listener->acc_token));
        listener->acc_token.CompletionToken.Event  = ev;
        listener->acc_token.CompletionToken.Status = EFI_ABORTED;
        axl_efi_call(listener->tcp4->Accept, 2, listener->tcp4,
                     &listener->acc_token);

        if (!EFI_ERROR(acc_status) && child_handle != NULL) {
            drain_reset_child(listener, child_handle);
        }
    }
}

static void
tcp_close_impl(AxlTcp *sock, bool abortive)
{
    if (sock == NULL) {
        return;
    }

    /* A socket is torn down ONCE. The second call is not hypothetical: this
       close retires every pending send and runs their callbacks before it
       returns, and closing the socket from a send callback is ordinary
       consumer code (on_response_sent -> reset_connection -> axl_tcp_close,
       s9p_on_send, sdk/examples/tcp-echo-server.c). Re-running the teardown
       would Cancel and Close firmware state this call has already released,
       submit a second close token, and — on the paths that finalize inline —
       free the socket while this call is still using it.
       docs/AXL-Tcp-Queue-Design.md §6b defect 2. */
    if (sock->closed) {
        return;
    }
    sock->closed = true;

    /* A GRACEFUL EFI_TCP4.Close() at a raised TPL hard-wedges the loop when the
       connection still has un-flushed outbound TCP data. Close() (AbortOnClose
       FALSE) flushes the send buffer and drives the active-close FIN handshake
       before completing; that transmit + its ACK need the MNP periodic timer to
       fire BELOW TPL_CALLBACK. From a driver-pump notify (axl_loop_attach_driver
       dispatches at TPL_CALLBACK) we hold that level, so the timer never fires,
       the flush never completes, and Close() spins in firmware forever — one
       core pegged, the loop dead, a reboot the only exit. (Phase 1 below cancels
       our own tx TOKEN, but the bytes already handed to the TCP send buffer are
       what Close() must still flush.)

       An ABORTIVE close (RST) discards the send buffer and skips the handshake,
       so it returns without a flush wait. A connection reset at a raised TPL is
       an abandon anyway (reset_connection frees the outbound queue), so RST is
       the correct teardown, not a downgrade. Listeners carry no outbound data
       and must keep their graceful path (its Phase 0 backlog drain is gated on
       an EXPLICIT abortive request), so scope this to connection sockets. The
       foreground (TPL_APPLICATION) path is unchanged — there the MNP timer fires
       and graceful Close() completes normally. */
    if (!abortive && !sock->is_listener && axl_backend_at_raised_tpl()) {
        abortive = true;
    }

    //
    // PHASE 0 (abortive listener only): clear the two port-holder categories a
    // plain listener close leaves behind, so the port is free on return even
    // under churny load. BEFORE Phase 1 cancels the accept token:
    //   (a) drain the firmware ACCEPT BACKLOG — queued-but-undelivered
    //       connections with no app handle (see drain_accept_backlog);
    //   (b) finalize this listener's in-flight loop-DEFERRED closes —
    //       connections served then gracefully closed, whose close went async
    //       and whose server slot is already gone (see
    //       finalize_listener_deferred_closes).
    // Together with Phase 1/2 (the listener itself) and the server's abortive
    // reset of its live connections, that is every place a firmware PCB on this
    // port can hide. No-op for a connection socket or when both are empty.
    //
    if (abortive && sock->is_listener) {
        /* Quiesce NEW accepts FIRST: Configure(NULL) on the still-listening
           socket stops the firmware completing fresh handshakes (post-reset SYNs
           get RST, not queued), so the backlog stops refilling. Without this, a
           peer hammering connections through the teardown refills the backlog as
           fast as the drain empties it and the RESET free never converges (it
           races the inbound SYN rate). With the accept path quiesced the backlog
           is a FROZEN finite set the drain then clears; the drain is bounded, so
           the free always returns even if a firmware keeps Accept partly live. */
        axl_efi_call(sock->tcp4->Configure, 2, sock->tcp4, NULL);
        drain_accept_backlog(sock);
        finalize_listener_deferred_closes(sock);
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

    axl_tcp_send_drop_sources(sock);
    if (sock->tcp4 != NULL && sock->tx_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->tx_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->tx_token.CompletionToken.Event);
        sock->tx_token.CompletionToken.Event = NULL;
    }
    /* Retire every pending send — port of EDK2 SockConnFlush, which runs
       SockFlushPendingToken over the PROCESSING list as well as SndTokenList.
       Both halves matter, for the same reason: an accepted send's callback
       must always fire, or its caller waits forever with its borrowed buffer
       pinned — and the ciphertext copy axl_tls_write_async frees from that
       callback leaks with it. docs/AXL-Tcp-Queue-Design.md §3.5.

       The ACTIVE send is included because a send the caller merely QUEUED can
       have been promoted into that slot behind their back. Firing its callback
       was not implementable while callbacks ran inline: it re-entered teardown
       (on_response_sent -> reset_connection -> axl_tcp_close) and wedged the
       run. It is implementable now — the callbacks are retired first and run
       from one drain, and the re-entrant close they trigger returns at the
       `closed` guard above rather than tearing this socket down twice.

       Synchronous, unlike every other retirement: the caller's loop may be
       freed the moment this returns (test_tcp_send_async_flush_on_close does
       exactly that), so a callback left for the next tick would never run. */
    axl_tcp_send_flush(sock, AXL_CANCELLED);

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
    ctx->close_token.AbortOnClose          = abortive;

    EFI_STATUS status = axl_efi_call(sock->tcp4->Close, 2,
                                     sock->tcp4, &ctx->close_token);
    if (EFI_ERROR(status)) {
        axl_debug("close: Close() returned %llx - abrupt teardown",
                  (unsigned long long)status);
        finalize_close_ctx(ctx);
        return;
    }

    if (abortive) {
        /* Abortive teardown: the RST completes with no TIME_WAIT, so finalize
           synchronously and loop-free, then return with the port released.
           close_wait_inline nests no loop and dispatches no sources, so this is
           safe at a raised TPL and leaves no deferred close-event source on any
           loop. Configure(NULL) in finalize is safe even if the completion event
           lags: the RST already tore the PCB down, so there is no graceful FIN
           for TcpFlushPcb to drop — the hazard that makes the default (graceful)
           path defer finalization to a later loop tick. */
        close_wait_inline(sock->tcp4, close_event, AXL_TCP_ABORT_WAIT_US);
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
            /* Track this loop-deferred close so an abortive teardown of the
               listener it was accepted from can finalize it synchronously. */
            ctx->reg_listener_id = sock->listener_id;
            deferred_close_link(ctx);
            return;
        }
        axl_warning("close: cannot register close event on loop - sync fallback");
    }

    /* Sync fallback (loop not running, or registration failed). 3 s
       covers TIME_WAIT for active close; passive close signals
       immediately so the wait usually returns much sooner. */
    if (_axl_loop_in_callback()) {
        /* A loop is already dispatching in this call stack — a sync request's
           ephemeral-loop callback (axl-http-client-async.c clears async_loop on
           the drop path), or the connect-fail/cancel teardown in
           axl-tcp-async.c. The loop-based _axl_tcp_wait would axl_loop_new() a
           SECOND loop nested in the first: the re-entrancy artifact the async
           effort removed, plus a close-event source that outlives the ephemeral
           loop (the leak). Complete the close loop-free instead. */
        close_wait_inline(sock->tcp4, close_event, 3000ULL * 1000ULL);
    } else {
        /* No loop dispatching in this call stack (e.g. axl_http_client_free /
           an explicit axl_tcp_close after a sync call has already returned):
           the loop-based wait sleeps on WaitForEvent between Poll ticks rather
           than busy-polling. (A sync request's OWN drop-close runs inside its
           ephemeral loop's dispatch, so it takes the inline branch above.) */
        _axl_tcp_wait(sock->tcp4, close_event, 3000ULL * 1000ULL);
    }
    finalize_close_ctx(ctx);
}

void
axl_tcp_close(AxlTcp *sock, AxlTeardown mode)
{
    tcp_close_impl(sock, mode == AXL_TEARDOWN_RESET);
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
