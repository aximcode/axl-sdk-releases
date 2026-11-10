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
#include "../loop/axl-loop-internal.h"   /* _axl_loop_has_source */

AXL_LOG_DOMAIN("tcp");

/* Forward-declared exactly as src/event/axl-wait.c does — _axl_loop_in_callback
   lives in the src/loop-only axl-loop-internal.h, which src/net cannot include.
   True when a loop is dispatching somewhere in this (single-threaded) call
   stack; used to pick the loop-FREE close-completion below. */
bool _axl_loop_in_callback(void);

/* 10 ms Poll cadence between close-completion checks — matches the Tier-4
   per-protocol wait helpers (axl-net-wait.c). */
#define AXL_TCP_CLOSE_POLL_US  AXL_NET_POLL_TICK_US

/* Inline wait cap for an ABORTIVE close. A RST completes with no TIME_WAIT, so
   the completion event signals almost immediately; this only bounds a wedged
   firmware. Shorter than the graceful cap below. */
#define AXL_TCP_ABORT_WAIT_US  (1000ULL * 1000ULL)

/* Wait cap for a GRACEFUL close with no loop to defer to. The wait normally
   ends a round trip after the FIN (close_wire_done); this bounds a peer that
   never ACKs it. */
#define AXL_TCP_GRACEFUL_WAIT_US  (3000ULL * 1000ULL)

/* How long that wait gives a peer in FIN_WAIT_2 (our FIN ACKed) to send its
   own FIN before finalizing anyway (close_wire_done). A peer closing on our
   FIN answers within a round trip; the margin is for its application waking
   to read the EOF. A quarter of TIME_WAIT, so a peer that is not closing
   costs the caller 500 ms rather than the old 3 s bound. */
#define AXL_TCP_FIN_WAIT2_GRACE_US  (500ULL * 1000ULL)

/* Deferred graceful closes are finalized by ONE sweeper timer per loop, not
   one event source per close: a close holds its context until the firmware
   completes it or the FINs are exchanged (close_wire_done), and with a source
   each, a client opening a connection per request at ~1 ms a round trip had
   hundreds in flight -- when every close sat out TIME_WAIT -- and exhausted
   the loop's AXL_MAX_SOURCES. A completion is noticed within one sweep, which
   only delays releasing the firmware's resources, never a reply. The sweep
   is one poll tick, because close_wire_done counts its FIN_WAIT_2 grace in
   those. The sweeper's id lives in its AxlLoop (tcp_close_sweeper), so
   axl_loop_free can report closes still pending. */
#define AXL_TCP_CLOSE_SWEEP_MS   ((uint32_t)(AXL_TCP_CLOSE_POLL_US / 1000ULL))

typedef struct {
    AxlTcp    *sock;
    AxlStatus  status;
    bool       done;
    AxlLoop   *loop;
} SyncResult;

/* When a graceful close may be finalized. The firmware signals
   the close token only when the connection is fully closed -- for an active
   close, after TIME_WAIT (2 s, EDK2's TCP_TIME_WAIT_TIME) -- so every client
   that closed first sat out 2 s of bookkeeping after the last segment had
   crossed the wire: an HTTP client freed on a keep-alive connection paid it
   per request, and a server held every connection it closed first -- socket,
   firmware child, port -- for 2 s after its reply. The close owes the wire
   nothing once BOTH FINs are exchanged (TIME_WAIT) or the connection is gone
   (CLOSED), and both the synchronous wait and the loop's sweeper finalize
   there.

   FIN_WAIT_2 -- our FIN and every byte before it ACKed, the peer's FIN not yet
   in -- is given AXL_TCP_FIN_WAIT2_GRACE_US: a peer closing on our FIN sends
   its own a moment after the ACK (often in the same burst), and finalizing
   before it arrives makes us answer it with an RST. A peer that is not
   closing (a keep-alive server idling, one stalled mid-response) ends the
   wait when the grace runs out. CLOSING (a simultaneous close, our FIN not
   yet ACKed), LAST_ACK and anything earlier keep waiting, for the event or
   (the sync wait only) the bound; so does a failed GetModeData.

   Finalizing early drops the PCB silently (Configure(NULL) flushes it and
   signals the parked close token; nothing goes on the wire). What it gives up
   is what TIME_WAIT and FIN_WAIT_2 hold the PCB for, none of it data the
   caller sent: a retransmitted peer FIN (our last ACK lost), or what a peer
   still open sends after the grace, is answered with an RST -- as a stack
   answers data arriving for a socket its application has closed.

   Called once per poll tick -- AXL_TCP_CLOSE_POLL_US in close_wait_inline,
   AXL_NET_POLL_TICK_US (the same constant) in _axl_tcp_wait, and
   AXL_TCP_CLOSE_SWEEP_MS (defined from it) in on_close_sweep -- so the grace
   is counted in ticks, like the inline wait's own bound, never a wall clock
   that a failed read could stop. A late tick only lengthens it. */
typedef struct {
    EFI_TCP4_PROTOCOL *tcp4;
    uint32_t           fin_wait2_ticks;   ///< ticks seen in FIN_WAIT_2
} TcpCloseWait;


/* Heap-owned context for an in-flight TCP close. The Close()
   token must outlive axl_tcp_close because EDK2 parks
   &ctx->close_token in Sock->CloseToken and dereferences it from
   SockConnClosed long after the call returns. See axl_tcp_close. */
typedef struct AxlTcpCloseCtx {
    EFI_TCP4_CLOSE_TOKEN  close_token;
    AxlTcp               *sock;
    TcpCloseWait          wire;       /* close_wire_done's state, both paths */
    AxlLoop              *loop;       /* set on the async path: its sweeper finalizes it */
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
   lock). A ctx is linked when tcp_close_impl defers it to its loop's sweeper
   and unlinked in finalize_close_ctx — whether that runs from the sweeper or
   from the abortive listener-teardown sweep. */
static AxlTcpCloseCtx *g_deferred_closes;
/* Bumped by every link and unlink, so a sweep can tell whether the registry
   changed under it beyond its own finalize. */
static uint32_t        g_deferred_gen;

static void
deferred_close_link(AxlTcpCloseCtx *ctx)
{
    ctx->reg_prev = NULL;
    ctx->reg_next = g_deferred_closes;
    if (g_deferred_closes != NULL) {
        g_deferred_closes->reg_prev = ctx;
    }
    g_deferred_closes = ctx;
    g_deferred_gen++;
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
    g_deferred_gen++;
}

static bool
close_wire_done(void *ctx)
{
    TcpCloseWait              *w     = (TcpCloseWait *)ctx;
    EFI_TCP4_CONNECTION_STATE  state = Tcp4StateEstablished;

    if (EFI_ERROR(axl_efi_call(w->tcp4->GetModeData, 6, w->tcp4, &state,
                               NULL, NULL, NULL, NULL))) {
        return false;
    }
    if (state == Tcp4StateTimeWait || state == Tcp4StateClosed) {
        return true;
    }
    if (state == Tcp4StateFinWait2) {
        w->fin_wait2_ticks++;
        return (uint64_t)w->fin_wait2_ticks * AXL_TCP_CLOSE_POLL_US
               >= AXL_TCP_FIN_WAIT2_GRACE_US;
    }
    return false;
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
    _axl_net_latency_link(sock);

    *out_listener = sock;
    axl_debug("listening on port %u", port);
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
    if (sock == NULL) {
        return AXL_ERR;
    }
    /* A refusal below reports 0 bytes sent (axl_tcp_send_get_sent). */
    sock->send_last_sent     = 0;
    sock->send_last_finished = false;
    if (data == NULL || size == 0) {
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
       for. r.status stays AXL_ERR: the send did not complete. A piece it had
       armed is left to the firmware as the socket's orphan, and its count is
       left in send_last_sent. */
    if (!r.done) {
        (void)axl_tcp_send_drop_token(sock, on_sync_complete, &r);
    } else if (r.status == AXL_CANCELLED
               || (r.status == AXL_OK && !sock->send_last_finished)) {
        /* The only cancel this call arms is its timeout. A send cut short by
           it is AXL_CANCELLED; one whose last piece was already armed is
           retired AXL_OK by the async layer (nothing is left to stop), but
           its last Transmit has not completed -- and "finished when I
           return" is this wrapper's contract, so a peer that has stopped
           reading is still reported. */
        r.status = AXL_TIMEOUT;
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

    /* The loop can return with the receive still armed -- a Ctrl-C (a new
       loop intercepts the Shell's break), or any other source quitting it.
       The firmware's Receive token then still points into the caller's
       buffer, which is often a stack frame about to be reused: the next
       segment would be written into it. So the receive is taken down HERE,
       while the loop its sources are on still exists: its sources removed
       (and a linger ended) and the token cancelled, which takes it off
       TcpDxe's queue. The completion that signals is left on the event: the
       socket's next axl_tcp_recv_async consumes any pending signal (its
       CheckEvent before re-arming), and nothing else waits on it. On a
       completed or timed-out receive there is nothing to do:
       on_recv_complete and the cancel path already cleared the ids. */
    if (!r.done) {
        axl_tcp_recv_drop_sources(sock);
        if (sock->tcp4 != NULL && sock->rx_token.CompletionToken.Event != NULL) {
            axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                         &sock->rx_token.CompletionToken);
            /* The receive may have COMPLETED between the loop returning and
               the Cancel (TcpDxe runs at TPL_CALLBACK and can deliver in that
               window): the bytes are then in the caller's buffer and gone
               from the stream, so they are reported, not dropped.
               axl_tcp_recv_async armed the token with Status EFI_ABORTED and
               a Cancel leaves it EFI_ABORTED, so EFI_SUCCESS here can only
               mean it completed with data. That includes a Receive TcpDxe
               completed synchronously from data already buffered, whose
               completion the loop never dispatched because it saw Ctrl-C
               first. Counted like any completed receive (stat_recvs). */
            if (sock->rx_token.CompletionToken.Status == EFI_SUCCESS) {
                sock->stat_recvs++;
                sock->recv_size = (size_t)sock->rx_data.DataLength;
                sock->recv_end  = EFI_SUCCESS;
                r.status        = AXL_OK;
            }
        }
    }

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
//   PHASE 2 (finalize_close_ctx, run by the loop's close sweeper, or
//   inline at the end of the sync wait):
//     * Configure(NULL) — safe now because nothing un-ACKed remains:
//       the close completed, or the sweeper or the sync wait saw
//       TIME_WAIT, FIN_WAIT_2 or CLOSED (close_wire_done), so
//       TcpFlushPcb's NetbufQueFlush has nothing of ours left to drop
//       -- unless the sync wait's bound expired first.
//     * DestroyChild on the service binding.
//     * Free sock + close_event + ctx.
//
// The async path holds one registry entry (no loop source -- one sweeper
// serves them all) per outstanding close, normally for a round trip.
// ---------------------------------------------------------------------------

/* Returns false when NEITHER release call succeeded -- the one case in which
   the firmware may still hold a close token parked on this socket (either
   call flushes it: Configure(NULL) via SockConnFlush, DestroyChild via
   SockDestroy). */
static bool
finalize_sock(AxlTcp *sock)
{
    EFI_STATUS cfg_status     = EFI_SUCCESS;
    EFI_STATUS destroy_status = EFI_SUCCESS;
    bool       keep_tx        = false;

    _axl_net_latency_unlink(sock);   /* before the TCP instance it polls goes */
    if (sock->tcp4 != NULL) {
        cfg_status = axl_efi_call(sock->tcp4->Configure, 2, sock->tcp4, NULL);
    }
    if (sock->tcp_sb != NULL && sock->tcp_handle != NULL) {
        destroy_status = axl_efi_call(sock->tcp_sb->DestroyChild, 2,
                                      sock->tcp_sb, sock->tcp_handle);
    }
    if (EFI_ERROR(cfg_status) || EFI_ERROR(destroy_status)) {
        /* With a Transmit accepted and never completed, a failed release may
           mean the firmware kept the socket -- and with it pointers to
           tx_token, tx_data and tx_frag (all INSIDE this struct), the
           token's event and tx_bounce. Freeing any of them would hand the
           firmware freed memory, so all of them are deliberately LEAKED,
           the AxlTcp included: a bounded leak on a firmware failure beats a
           use-after-free. The send callbacks below still run; they never
           lent the firmware anything. */
        if (sock->tx_inflight) {
            keep_tx              = true;
            sock->firmware_holds = true;
            axl_error("close: firmware release failed with a transmit in "
                      "flight (Configure %llx, DestroyChild %llx); leaking "
                      "the socket and its %zu-byte transmit buffer",
                      (unsigned long long)cfg_status,
                      (unsigned long long)destroy_status,
                      sock->tx_bounce_cap);
        } else {
            axl_debug("close: firmware release failed (Configure %llx, "
                      "DestroyChild %llx)",
                      (unsigned long long)cfg_status,
                      (unsigned long long)destroy_status);
        }
    }
    /* The firmware has let go: Configure(NULL) flushes the socket's send
       queue (SockFlush -> SockConnFlush) and DestroyChild frees it, so the
       transmit copy buffer is referenced from nowhere any more, and the
       transmit token -- left outstanding by a close, see tcp_close_impl --
       has been signalled for the last time. Only now may its event close and
       the copy buffer go. Clear the handles too: a callback below asking this
       socket for, say, its peer address must get a clean failure rather than
       a call into a destroyed child. */
    sock->tcp4       = NULL;
    sock->tcp_handle = NULL;
    if (!keep_tx) {
        axl_free(sock->tx_bounce);
        if (sock->tx_token.CompletionToken.Event != NULL) {
            axl_backend_event_close(
                (AxlEventHandle)sock->tx_token.CompletionToken.Event);
        }
    }
    sock->tx_bounce                      = NULL;
    sock->tx_bounce_cap                  = 0;
    sock->tx_token.CompletionToken.Event = NULL;

    /* Deliver every send callback still owed -- AFTER the release above, so
       an abortive close's AXL_CANCELLED means the bytes were not delivered:
       the RST has discarded them. A no-op on the graceful path, which retired
       everything before submitting its Close. */
    axl_tcp_send_flush(sock, false);

    /* A retired-send drain is walking this socket's callbacks and still hands
       `sock` to each of them — freeing here would hand the rest a dangling
       pointer. The drain does the free once its last callback has returned
       (axl_tcp_send_drain_done). */
    bool released = !EFI_ERROR(cfg_status) || !EFI_ERROR(destroy_status);
    if (sock->send_draining) {
        sock->free_deferred = true;
        return released;
    }
    if (!sock->firmware_holds) {
        axl_free(sock);
    }
    return released;
}

static void
finalize_close_ctx(AxlTcpCloseCtx *ctx)
{
    /* Drop from the deferred-close registry first (no-op for a sync-path ctx
       that was never linked). Idempotent — a ctx is finalized exactly once. */
    deferred_close_unlink(ctx);
    if (!finalize_sock(ctx->sock)
        && ctx->close_token.CompletionToken.Event != NULL
        && axl_backend_event_check(
               (AxlEventHandle)ctx->close_token.CompletionToken.Event) > 0) {
        /* The firmware released nothing and never signalled the token: it may
           still hold &ctx->close_token (a sync close finalizes before the
           close completes). Leak ctx and its event rather than hand the
           firmware freed memory -- the same trade as finalize_sock's. */
        axl_error("close: firmware release failed with the close pending; "
                  "leaking its %zu-byte context", sizeof(*ctx));
        return;
    }
    if (ctx->close_token.CompletionToken.Event != NULL) {
        axl_backend_event_close(
            (AxlEventHandle)ctx->close_token.CompletionToken.Event);
    }
    axl_free(ctx);
}

/* Finalize @p loop's completed deferred closes. Finalizing runs send
   callbacks, which may close other sockets -- linking new closes, or (an
   abortive listener teardown) finalizing others -- so the walk keeps its
   saved next pointer only while the registry changed by nothing but its own
   finalize, and starts again from the head otherwise. The sweeper goes once
   its loop has no close left. */
static bool
on_close_sweep(void *data)
{
    AxlLoop        *loop = (AxlLoop *)data;
    AxlTcpCloseCtx *ctx  = g_deferred_closes;
    while (ctx != NULL) {
        AxlTcpCloseCtx *next = ctx->reg_next;
        /* Completed (0) or a bad handle (<0) -- or done on the wire, which
           for an active close is TIME_WAIT's 2 s sooner. The event check
           MUST come first: a child the firmware destroyed under us (driver
           Stop) has its token signalled, and close_wire_done would call
           GetModeData on the freed protocol. */
        if (ctx->loop == loop
            && (axl_backend_event_check(
                    (AxlEventHandle)ctx->close_token.CompletionToken.Event) <= 0
                || close_wire_done(&ctx->wire))) {
            uint32_t gen = g_deferred_gen;
            finalize_close_ctx(ctx);
            if (g_deferred_gen != gen + 1u) {
                next = g_deferred_closes;   /* changed under us: rescan */
            }
        }
        ctx = next;
    }
    for (ctx = g_deferred_closes; ctx != NULL; ctx = ctx->reg_next) {
        if (ctx->loop == loop) {
            return AXL_SOURCE_CONTINUE;
        }
    }
    loop->tcp_close_sweeper = 0;
    return AXL_SOURCE_REMOVE;
}

/* @p loop's sweeper removed now if it has no close left -- for closes
   finalized by an abortive listener teardown, so a loop freed right after
   does not see a sweeper still registered with nothing to do. That teardown
   can run inside the sweep itself (a send callback finalize ran); removing
   the source being dispatched is safe -- AxlLoop marks it inactive and
   ignores its return -- and a sweeper a nested close started meanwhile
   removes itself on its next pass. */
static void
close_sweeper_release_if_idle(AxlLoop *loop)
{
    if (loop == NULL || loop->tcp_close_sweeper == 0) {
        return;
    }
    for (AxlTcpCloseCtx *ctx = g_deferred_closes; ctx != NULL; ctx = ctx->reg_next) {
        if (ctx->loop == loop) {
            return;
        }
    }
    axl_loop_remove_source(loop, loop->tcp_close_sweeper);
    loop->tcp_close_sweeper = 0;
}

/* A sweeper on @p loop, started if it has none; false if none can be. */
static bool
close_sweeper_ensure(AxlLoop *loop)
{
    if (loop->tcp_close_sweeper != 0 && _axl_loop_has_source(loop, loop->tcp_close_sweeper)) {
        return true;
    }
    loop->tcp_close_sweeper = axl_loop_add_timer(loop, AXL_TCP_CLOSE_SWEEP_MS,
                                                 on_close_sweep, loop);
    return loop->tcp_close_sweeper != 0;
}

// Finalize every in-flight loop-deferred close spawned from @p listener,
// synchronously and loop-free — the third port-holder category an abortive
// listener teardown must clear (connections served then gracefully closed, whose
// close went async and whose slot the server already dropped). Each such close
// keeps a firmware PCB on the listen port and a caller-owned loop source until
// its loop's sweeper finalizes it (close_wire_done; needs a pump). Here we
// finalize now: Configure(NULL)+DestroyChild in finalize_sock force-
// drops the PCB even mid-graceful-close (dropping the un-ACKed FIN, which is the
// intended trade for an immediate teardown — same as an abortive close). Scoped
// by reg_listener so a sibling server on the same loop/NIC is untouched.
static void
finalize_listener_deferred_closes(AxlTcp *listener)
{
    AxlTcpCloseCtx *ctx = g_deferred_closes;
    while (ctx != NULL) {
        AxlTcpCloseCtx *next = ctx->reg_next;
        if (ctx->reg_listener_id == listener->listener_id) {
            /* Finalizing unlinks it, so the sweeper never sees it again; and
               a sweeper left with nothing goes now, not at its next pass.
               Finalizing runs send callbacks, which may close (link or
               finalize) others: keep `next` only if the registry changed by
               this unlink alone, as on_close_sweep does. */
            AxlLoop *loop = ctx->loop;
            uint32_t gen  = g_deferred_gen;
            finalize_close_ctx(ctx);
            close_sweeper_release_if_idle(loop);
            if (g_deferred_gen != gen + 1u) {
                next = g_deferred_closes;
            }
        }
        ctx = next;
    }
}

/* Loop-FREE close completion: drive tcp4->Poll() and poll the close_event
   directly until it signals, @p done_fn (when not NULL) says the close owes
   nothing more, or the deadline passes. Unlike _axl_tcp_wait (which
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
                  AxlCondFn done_fn, void *done_ctx, uint64_t timeout_us)
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
        if (done_fn != NULL && done_fn(done_ctx)) {
            return;
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
        _axl_net_latency_activity();
        axl_efi_call(listener->tcp4->Accept, 2, listener->tcp4,
                     &listener->acc_token);

        if (!EFI_ERROR(acc_status) && child_handle != NULL) {
            drain_reset_child(listener, child_handle);
        }
    }
}

/* Production's answer at every test point: never (see axl-tcp-internal.h). */
__attribute__((weak)) bool
_axl_tcp_test_point(const char *point)
{
    (void)point;
    return false;
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

    if (sock->stat_recvs != 0 && _axl_tcp_test_point("recv-stats")) {
        /* log-level: a test seam's measurement, logged only when a test image asks */
        axl_info("closed after %u receive(s); %u linger(s), %u receive(s) inside one",
                 sock->stat_recvs, sock->stat_lingers, sock->stat_linger_recvs);
    }

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

    /* A close that would DROP accepted bytes is abortive too: a send still
       queued, an active send only partly handed to the firmware, or a stream
       some earlier send already cut short (stream_torn). A FIN there would
       tell the peer the stream ended cleanly -- indistinguishable from a
       complete, shorter one -- and the RST says it did not. The owners of the
       dropped sends are told AXL_CANCELLED once the firmware has let go (see
       finalize_sock).

       It is no longer a memory-safety property: the firmware only ever holds
       AxlTcp's own copy of a piece (tx_bounce), never the caller's buffer, so
       nothing a callback frees can reach the wire. An active send that is
       FULLY armed drops nothing -- TCP sends its last piece ahead of the FIN
       -- so it stays graceful and is retired AXL_OK (axl_tcp_send_flush), as
       is an orphan piece on an untruncated stream. */
    if (!abortive && !sock->is_listener && axl_tcp_send_close_drops(sock)) {
        axl_debug("close: send(s) pending -- promoted to abortive (RST)");
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
        axl_loop_remove_source(sock->recv_loop, sock->recv_source);
        sock->recv_source = 0;
    }
    if (sock->recv_cancel_source > 0) {
        axl_loop_remove_source(sock->recv_loop, sock->recv_cancel_source);
        sock->recv_cancel_source = 0;
    }
    if (sock->tcp4 != NULL && sock->rx_token.CompletionToken.Event != NULL) {
        axl_efi_call(sock->tcp4->Cancel, 2, sock->tcp4,
                     &sock->rx_token.CompletionToken);
        axl_backend_event_close(
            (AxlEventHandle)sock->rx_token.CompletionToken.Event);
        sock->rx_token.CompletionToken.Event = NULL;
    }

    /* The transmit token is NEVER cancelled here, or anywhere: EFI_TCP4.Cancel
       on a Transmit TcpDxe is still processing frees the token and strands its
       data in the send queue, which SockDataSent then trips over. An abortive
       Close(Abort) retires it itself (SockConnFlush signals it); a graceful
       Close sends it ahead of the FIN. Its event is closed in finalize_sock,
       once nothing can signal it. */
    axl_tcp_send_drop_sources(sock);

    /* Retire every pending send — port of EDK2 SockConnFlush, which runs
       SockFlushPendingToken over the PROCESSING list as well as SndTokenList.
       Both halves matter, for the same reason: an accepted send's callback
       must always fire, or its caller waits forever with its borrowed buffer
       pinned — and the ciphertext copy axl_tls_write_async frees from that
       callback leaks with it. docs/AXL-Tcp-Queue-Design.md §3.5.

       The ACTIVE send is included because a send the caller merely QUEUED can
       have been promoted into that slot behind their back. The callbacks run
       from one drain, and the re-entrant close they trigger returns at the
       `closed` guard above rather than tearing this socket down twice.

       Synchronous, unlike every other retirement: the caller's loop may be
       freed the moment this returns (test_tcp_send_async_flush_on_close does
       exactly that), so a callback left for the next tick would never run.

       WHEN depends on the mode. An abortive close delivers them from
       finalize_sock, after the firmware has released the socket, so its
       AXL_CANCELLED means "not delivered". A graceful close drops nothing (it
       would have been promoted), so all it can hold is a FINISHED send whose
       callback has not run yet, or a FULLY-armed active one: both are done
       from their owner's point of view, and their callbacks run here, before
       Close is submitted. */
    if (!abortive) {
        axl_tcp_send_flush(sock, true);
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
    ctx->wire.tcp4                         = sock->tcp4;
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
           for TcpFlushPcb to drop -- the hazard that makes the default (graceful)
           path defer finalization to a later loop tick.

           THIS STAYS SYNCHRONOUS FOR CONNECTION SOCKETS TOO, and it was
           briefly not. Deferring a connection's abortive close onto the loop
           looks like an easy win -- close_wait_inline busy-polls, and a server
           dropping N connections from its own callbacks pays that N times --
           but it moves a guarantee THREE subsystems depend on. AxlHttpServer
           and Axl9pServer free their LISTENER FIRST and their live connections
           second, deliberately (94a603b5, f558bc5d, 9f960b7b): the listener's
           PHASE 0 sweep is what finalizes deferred closes of children accepted
           from it. Make the children defer and that sweep has already run --
           nothing finalizes them, their EFI_TCP4 children keep PCBs on the
           listen port, and the port-free-on-return promise in
           axl-http-server.h breaks. Reached in driver mode, where
           axl_loop_is_running() stays true.

           The busy-poll cost is real, and it belongs to the CALLER: a server
           that can drop many connections at once should bound how many it
           holds, which is what AxlSsh's MAX_CONNS does. */
        close_wait_inline(sock->tcp4, close_event, NULL, NULL, AXL_TCP_ABORT_WAIT_US);
        finalize_close_ctx(ctx);
        return;
    }

    /* Async path: hand the close to the caller's running loop and return.
       The loop's close sweeper runs finalize_close_ctx once the close owes
       the wire nothing (close_wire_done) or the firmware signals
       SockConnClosed, with no synchronous wait or CPU spin. Graceful closes only: see the abortive
       branch above for why that one stays synchronous. */
    if (sock->async_loop != NULL && axl_loop_is_running(sock->async_loop)) {
        if (close_sweeper_ensure(sock->async_loop)) {
            /* Tracked so the loop's sweeper finds it, and so an abortive
               teardown of the listener it was accepted from can finalize it
               synchronously. */
            ctx->loop            = sock->async_loop;
            ctx->reg_listener_id = sock->listener_id;
            deferred_close_link(ctx);
            return;
        }
        axl_warning("close: cannot start this loop's close sweeper - sync fallback");
    }

    /* Sync fallback (loop not running, or registration failed). The wait
       ends once the close owes the wire nothing (close_wire_done) -- a round
       trip after the FIN, not TIME_WAIT -- or when the firmware completes it
       (a passive close completes at once); AXL_TCP_GRACEFUL_WAIT_US bounds a
       peer that never ACKs the FIN. */
    if (_axl_loop_in_callback()) {
        /* A loop is already dispatching in this call stack — a sync request's
           ephemeral-loop callback (axl-http-client-async.c clears async_loop on
           the drop path), or the connect-fail/cancel teardown in
           axl-tcp-async.c. The loop-based _axl_tcp_wait would axl_loop_new() a
           SECOND loop nested in the first: the re-entrancy artifact the async
           effort removed, plus a close-event source that outlives the ephemeral
           loop (the leak). Complete the close loop-free instead. */
        close_wait_inline(sock->tcp4, close_event, close_wire_done, &ctx->wire,
                          AXL_TCP_GRACEFUL_WAIT_US);
    } else {
        /* No loop dispatching in this call stack (e.g. axl_http_client_free /
           an explicit axl_tcp_close after a sync call has already returned):
           the loop-based wait sleeps on WaitForEvent between Poll ticks rather
           than busy-polling. (A sync request's OWN drop-close runs inside its
           ephemeral loop's dispatch, so it takes the inline branch above.) */
        _axl_tcp_wait(sock->tcp4, close_event, close_wire_done, &ctx->wire,
                      AXL_TCP_GRACEFUL_WAIT_US);
    }
    finalize_close_ctx(ctx);
}

void
axl_tcp_close(AxlTcp *sock, AxlTeardown mode)
{
    tcp_close_impl(sock, mode == AXL_TEARDOWN_RESET);
}

size_t
axl_tcp_closes_pending(void)
{
    size_t n = 0;

    for (AxlTcpCloseCtx *ctx = g_deferred_closes; ctx != NULL; ctx = ctx->reg_next) {
        n++;
    }
    return n;
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

bool
_axl_tcp_recv_fin(
    const AxlTcp *sock
    )
{
    return sock != NULL && sock->recv_end == EFI_CONNECTION_FIN;
}
