/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-udp.c
    UDP datagram sockets — ported from SoftBMC UdpUtil.c.

    Lifecycle: service binding → create child → configure → send/recv → close.
    Blocking I/O uses poll-wait with 1ms intervals. Async receive integrates
    with AxlLoop via raw event sources.
**/

#include "axl-net-internal.h"
#include "../backend/axl-backend.h"
#include "../event/axl-cancellable-internal.h"
#include <axl/axl-cancellable.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-net.h>
#include <axl/axl-loop.h>   /* AxlSourceId */

AXL_LOG_DOMAIN("udp");

#define UDP_SEND_TIMEOUT_US  (2 * 1000 * 1000)  /* 2 seconds */

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

struct AxlUdp {
    EFI_SERVICE_BINDING_PROTOCOL  *sb;
    EFI_HANDLE                     sb_handle;
    EFI_HANDLE                     udp_handle;
    EFI_UDP4_PROTOCOL             *udp4;

    /* Saved configuration so axl_udp_connect / axl_udp_disconnect
       can re-Configure with the peer set/cleared while preserving
       the local-port choice + station settings. */
    EFI_UDP4_CONFIG_DATA           cfg;
    bool                           peer_set;

    /* Async receive state */
    AxlLoop                       *loop;
    AxlSourceId                    loop_source;
    AxlSourceId                    cancel_source;
    AxlUdpCallback                 on_recv;
    void                          *on_recv_data;
    EFI_UDP4_COMPLETION_TOKEN      rx_token;
    EFI_EVENT                      rx_event;

    /* Async send state — single in-flight enforced via tx_in_flight.
       The tx_session / tx_frag / tx_data are referenced by the EFI
       Transmit until completion, so they live on the struct and not
       on stack of the call. */
    bool                           tx_in_flight;
    AxlLoop                       *tx_loop;
    AxlSourceId                    tx_source;
    AxlSourceId                    tx_cancel_source;
    AxlUdpSendCallback             on_send;
    void                          *on_send_data;
    EFI_EVENT                      tx_event;
    EFI_UDP4_COMPLETION_TOKEN      tx_token;
    EFI_UDP4_SESSION_DATA          tx_session;
    EFI_UDP4_FRAGMENT_DATA         tx_frag;
    EFI_UDP4_TRANSMIT_DATA         tx_data;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void udp_recv_drop_sources(AxlUdp *sock);
static void udp_send_drop_sources(AxlUdp *sock);

// ---------------------------------------------------------------------------
// Service binding helpers
// ---------------------------------------------------------------------------

/* Locate a UDP4 service binding via the shared per-NIC picker so
   _via callers get source-IP / subnet-match selection for free. */
static int
find_udp_service_binding(
    AxlUdp           *sock,
    const AxlIPv4Address   *source_ip
    )
{
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

    EFI_STATUS status = axl_net_locate_sb(
        &gEfiUdp4ServiceBindingProtocolGuid,
        NULL, forced, &sock->sb, &sock->sb_handle);
    if (EFI_ERROR(status)) {
        if (forced != NULL) {
            axl_warning("no UDP4 interface with station IP %u.%u.%u.%u",
                source_ip->addr[0], source_ip->addr[1],
                source_ip->addr[2], source_ip->addr[3]);
        }
        return AXL_ERR;
    }

    return AXL_OK;
}

static int
create_udp_child(
    AxlUdp *sock
    )
{
    EFI_STATUS status = sock->sb->CreateChild(sock->sb, &sock->udp_handle);
    if (status != 0) {
        return -1;
    }

    EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
    status = axl_bs()->OpenProtocol(
        sock->udp_handle, &udp_guid, (void **)&sock->udp4,
        gImageHandle, sock->udp_handle,
        EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

    if (status != 0) {
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
        sock->udp_handle = NULL;
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

/* Open a UDP socket on a specific NIC (auto-pick when source_ip is
   NULL or all-zeros). Shared body for axl_udp_open + axl_udp_open_via. */
static int
udp_open_internal(
    AxlUdp          **out,
    uint16_t                local_port,
    const AxlIPv4Address   *source_ip
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;

    AxlUdp *sock = axl_new(AxlUdp);
    if (sock == NULL) {
        return AXL_ERR;
    }

    if (find_udp_service_binding(sock, source_ip) != AXL_OK) {
        axl_free(sock);
        return AXL_ERR;
    }

    if (create_udp_child(sock) != 0) {
        axl_debug("failed to create UDP4 child");
        axl_free(sock);
        return AXL_ERR;
    }

    /* Configure — use the picked NIC's address (DHCP or static), with
       the requested local port. UseDefaultAddress=1 is correct here:
       the per-NIC SB selection (via axl_net_locate_sb) already pinned
       us to the right interface; UseDefaultAddress just tells UDP4
       "use this NIC's configured station IP".
       Stash the config on sock so axl_udp_connect / _disconnect can
       re-Configure with peer set/cleared while preserving the rest. */
    axl_memset(&sock->cfg, 0, sizeof(sock->cfg));
    sock->cfg.AcceptBroadcast    = 0;
    sock->cfg.AcceptPromiscuous  = 0;
    sock->cfg.AcceptAnyPort      = 0;
    sock->cfg.AllowDuplicatePort = 0;
    sock->cfg.TimeToLive         = 64;
    sock->cfg.TypeOfService      = 0;
    sock->cfg.DoNotFragment      = 0;
    sock->cfg.ReceiveTimeout     = 0;
    sock->cfg.TransmitTimeout    = 0;
    sock->cfg.UseDefaultAddress  = 1;
    sock->cfg.StationPort        = local_port;

    EFI_STATUS status = axl_efi_call(sock->udp4->Configure, 2,
                                     sock->udp4, &sock->cfg);
    if (status != 0) {
        axl_debug("UDP4 configure failed: 0x%llx",
                 (unsigned long long)status);
        EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
        axl_bs()->CloseProtocol(sock->udp_handle, &udp_guid,
                                gImageHandle, sock->udp_handle);
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
        axl_free(sock);
        return AXL_ERR;
    }

    axl_debug("opened (port=%u)", (unsigned)local_port);
    *out = sock;
    return AXL_OK;
}

int
axl_udp_open(
    AxlUdp **out,
    uint16_t       local_port
    )
{
    return udp_open_internal(out, local_port, NULL);
}

int
axl_udp_open_via(
    AxlUdp          **out,
    uint16_t                local_port,
    const AxlIPv4Address   *source_ip
    )
{
    return udp_open_internal(out, local_port, source_ip);
}

int
axl_udp_connect(
    AxlUdp                *sock,
    const AxlIPv4Address  *peer,
    uint16_t               port
    )
{
    if (sock == NULL || sock->udp4 == NULL || peer == NULL) {
        return AXL_ERR;
    }

    /* Re-Configure with the peer set. UDP4 requires Configure(NULL)
       between configurations, then a fresh Configure with the new
       data. */
    axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);

    axl_memcpy(&sock->cfg.RemoteAddress, peer, 4);
    sock->cfg.RemotePort = port;

    EFI_STATUS status = axl_efi_call(sock->udp4->Configure, 2,
                                     sock->udp4, &sock->cfg);
    if (EFI_ERROR(status)) {
        /* Roll back the cfg state — the EFI side rejected our pin so
           the consumer should see "not connected" rather than a
           half-set state. UDP4 requires Configure(NULL) before
           Configure(non-NULL), same as the apply path above. If a
           failed Configure left the instance unconfigured, the
           Configure(NULL) is a no-op; if it left it configured, we
           need it. Best-effort throughout — if even the rollback
           fails the socket is unusable; consumer should close. */
        axl_memset(&sock->cfg.RemoteAddress, 0, 4);
        sock->cfg.RemotePort = 0;
        axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);
        axl_efi_call(sock->udp4->Configure, 2,
                           sock->udp4, &sock->cfg);
        return AXL_ERR;
    }

    sock->peer_set = true;
    return AXL_OK;
}

int
axl_udp_disconnect(
    AxlUdp *sock
    )
{
    if (sock == NULL || sock->udp4 == NULL) {
        return AXL_ERR;
    }
    if (!sock->peer_set) {
        return AXL_OK;  /* idempotent */
    }

    axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);
    axl_memset(&sock->cfg.RemoteAddress, 0, 4);
    sock->cfg.RemotePort = 0;

    EFI_STATUS status = axl_efi_call(sock->udp4->Configure, 2,
                                     sock->udp4, &sock->cfg);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    sock->peer_set = false;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API: multicast + broadcast
// ---------------------------------------------------------------------------

int
axl_udp_join_multicast(
    AxlUdp                *sock,
    const AxlIPv4Address  *group
    )
{
    if (sock == NULL || sock->udp4 == NULL || group == NULL) {
        return AXL_ERR;
    }
    /* IPv4 multicast space is 224.0.0.0/4 — high nibble of first
       octet must be 0xE (1110xxxx). Reject obvious non-multicast
       addresses up front. */
    if ((group->addr[0] & 0xF0) != 0xE0) {
        return AXL_ERR;
    }

    EFI_IPv4_ADDRESS efi_group;
    axl_memcpy(&efi_group, group, 4);

    EFI_STATUS status = axl_efi_call(sock->udp4->Groups, 3,
                                     sock->udp4, true, &efi_group);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_udp_leave_multicast(
    AxlUdp                *sock,
    const AxlIPv4Address  *group
    )
{
    if (sock == NULL || sock->udp4 == NULL) {
        return AXL_ERR;
    }

    EFI_IPv4_ADDRESS  efi_group;
    EFI_IPv4_ADDRESS *arg = NULL;
    if (group != NULL) {
        axl_memcpy(&efi_group, group, 4);
        arg = &efi_group;
    }

    EFI_STATUS status = axl_efi_call(sock->udp4->Groups, 3,
                                     sock->udp4, false, arg);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_udp_set_broadcast(
    AxlUdp *sock,
    bool    enable
    )
{
    if (sock == NULL || sock->udp4 == NULL) {
        return AXL_ERR;
    }
    if ((bool)sock->cfg.AcceptBroadcast == enable) {
        return AXL_OK;  /* idempotent */
    }

    axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);
    bool prev = sock->cfg.AcceptBroadcast;
    sock->cfg.AcceptBroadcast = enable ? 1 : 0;

    EFI_STATUS status = axl_efi_call(sock->udp4->Configure, 2,
                                     sock->udp4, &sock->cfg);
    if (EFI_ERROR(status)) {
        /* Roll back so cfg matches actual EFI state — without this
           a subsequent set_broadcast(prev) would hit the idempotent
           early-return and leave EFI state unsynced with cfg. */
        sock->cfg.AcceptBroadcast = prev;
        axl_efi_call(sock->udp4->Configure, 2,
                           sock->udp4, &sock->cfg);
        return AXL_ERR;
    }
    return AXL_OK;
}

void
axl_udp_close(
    AxlUdp *sock
    )
{
    if (sock == NULL) {
        return;
    }

    /* Tear down any armed recv_async state (the old _stop function's
       job — _stop is gone in favor of bool-return / cancellable on
       axl_udp_recv_async; close remains the catch-all teardown).
       Idempotent: udp_recv_drop_sources NULLs sock->loop so a
       second call (e.g. cb returned false then called close) is a
       no-op via the guard below. */
    if (sock->loop != NULL) {
        udp_recv_drop_sources(sock);
    }
    if (sock->tx_in_flight) {
        udp_send_drop_sources(sock);
    }

    if (sock->udp4 != NULL) {
        axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);
        EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
        axl_bs()->CloseProtocol(sock->udp_handle, &udp_guid,
                                gImageHandle, sock->udp_handle);
    }
    if (sock->sb != NULL && sock->udp_handle != NULL) {
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
    }

    axl_free(sock);
}

// ---------------------------------------------------------------------------
// Public API: addressing
// ---------------------------------------------------------------------------

int
axl_udp_get_local_addr(
    AxlUdp *sock,
    char         *addr,
    size_t        size,
    uint16_t     *out_port
    )
{
    EFI_UDP4_CONFIG_DATA cfg;
    EFI_STATUS           status;

    if (sock == NULL || sock->udp4 == NULL || addr == NULL ||
        size < 16 || out_port == NULL) {
        return AXL_ERR;
    }

    axl_memset(&cfg, 0, sizeof(cfg));
    status = axl_efi_call(sock->udp4->GetModeData, 5,
                          sock->udp4,
                          &cfg,
                          NULL,   /* Ip4ModeData */
                          NULL,   /* MnpConfigData */
                          NULL    /* SnpModeData */
                          );
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    EFI_IPv4_ADDRESS *sa = (EFI_IPv4_ADDRESS *)&cfg.StationAddress;
    axl_snprintf(addr, size, "%d.%d.%d.%d",
                 sa->Addr[0], sa->Addr[1], sa->Addr[2], sa->Addr[3]);
    *out_port = cfg.StationPort;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Sync wrappers over the async cores (Option A — one I/O implementation).
// Each spins a private loop, runs axl_udp_send_async / axl_udp_recv_async, and
// harvests the result. A timeout cancellable bounds the wait; a Poll tick over
// the socket drives the UDP4 state machine when the loop blocks at a raised TPL
// (at TPL_APPLICATION the firmware notify advances the op for free). Mirrors the
// TCP sync wrappers in axl-tcp-sync.c.
// ---------------------------------------------------------------------------

#define UDP_SEND_TIMEOUT_MS    (UDP_SEND_TIMEOUT_US / 1000)
#define UDP_SYNC_POLL_TICK_MS  10u

static bool
on_udp_sync_cancel_timeout(void *data)
{
    axl_cancellable_cancel((AxlCancellable *)data);
    return AXL_SOURCE_REMOVE;
}

static bool
udp_sync_poll_tick(void *data)
{
    EFI_UDP4_PROTOCOL *udp4 = (EFI_UDP4_PROTOCOL *)data;
    axl_efi_call(udp4->Poll, 1, udp4);
    return AXL_SOURCE_CONTINUE;
}

/* Arm the Poll tick only at a raised TPL, where the blocking loop starves the
   firmware notify (see axl-tcp-sync.c tcp_sync_arm_poll_tick). Returns the tick
   source id, or 0 when not armed. */
static AxlSourceId
udp_sync_arm_poll_tick(AxlLoop *loop, EFI_UDP4_PROTOCOL *udp4)
{
    if (!axl_backend_at_raised_tpl()) {
        return 0;
    }
    return axl_loop_add_timer(loop, UDP_SYNC_POLL_TICK_MS, udp_sync_poll_tick, udp4);
}

static void
udp_sync_disarm_poll_tick(AxlLoop *loop, AxlSourceId poll_src)
{
    if (poll_src != 0) {
        axl_loop_remove_source(loop, poll_src);
    }
}

// ---------------------------------------------------------------------------
// Public API: blocking send (wraps axl_udp_send_async)
// ---------------------------------------------------------------------------

typedef struct {
    AxlStatus  st;
    AxlLoop   *loop;
} UdpSyncSendCtx;

static bool
on_udp_sync_send_done(AxlUdp *sock, AxlStatus st, void *user)
{
    (void)sock;
    UdpSyncSendCtx *r = (UdpSyncSendCtx *)user;
    r->st = st;
    axl_loop_quit(r->loop);
    return false;
}

AxlStatus
axl_udp_send(
    AxlUdp        *sock,
    const AxlIPv4Address *dest,
    uint16_t              port,
    const void           *data,
    size_t                len
    )
{
    /* dest=NULL is allowed iff the socket is connected; axl_udp_send_async
       validates that (and every other arg), so don't duplicate the checks. */
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }
    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    UdpSyncSendCtx r = { AXL_ERR, loop };
    if (axl_udp_send_async(sock, dest, port, data, len, loop, cancel,
                           on_udp_sync_send_done, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return AXL_ERR;
    }

    axl_loop_add_timeout(loop, UDP_SEND_TIMEOUT_MS, on_udp_sync_cancel_timeout, cancel);
    AxlSourceId poll = udp_sync_arm_poll_tick(loop, sock->udp4);
    axl_loop_run(loop);
    udp_sync_disarm_poll_tick(loop, poll);

    axl_cancellable_free(cancel);
    axl_loop_free(loop);
    return r.st;
}

// ---------------------------------------------------------------------------
// Public API: blocking send-receive
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Public API: blocking send-receive (send wraps axl_udp_send, receive wraps a
// one-shot axl_udp_recv_async).
// ---------------------------------------------------------------------------

typedef struct {
    AxlStatus  st;
    AxlLoop   *loop;
    void      *rx_buf;
    size_t     rx_size;
    size_t     rx_len;
} UdpSyncRecvCtx;

/* One-shot receive: copy the first datagram (truncated to rx_size, matching the
   old inline copy) and stop. axl_udp_recv_async already gathered the fragments
   into the contiguous @p data buffer for us. */
static bool
on_udp_sync_recv_done(AxlUdp *sock, AxlStatus st, const void *data, size_t len,
                      const AxlIPv4Address *from, uint16_t from_port, void *user)
{
    (void)sock;
    (void)from;
    (void)from_port;
    UdpSyncRecvCtx *r = (UdpSyncRecvCtx *)user;
    r->st = st;
    if (st == AXL_OK && data != NULL) {
        size_t copy = (len < r->rx_size) ? len : r->rx_size;
        axl_memcpy(r->rx_buf, data, copy);
        r->rx_len = copy;
    }
    axl_loop_quit(r->loop);
    return false;   /* one-shot — stop receiving */
}

int
axl_udp_sendrecv(
    AxlUdp        *sock,
    const AxlIPv4Address *dest,
    uint16_t              port,
    const void           *tx_data,
    size_t                tx_len,
    size_t                timeout_ms,
    void                 *rx_buf,
    size_t                rx_size,
    size_t               *rx_len
    )
{
    if (sock == NULL || sock->udp4 == NULL ||
        tx_data == NULL || tx_len == 0 ||
        rx_buf == NULL || rx_size == 0 || rx_len == NULL) {
        return AXL_ERR;
    }

    *rx_len = 0;

    /* --- Transmit phase --- */
    if (axl_udp_send(sock, dest, port, tx_data, tx_len) != AXL_OK) {
        return AXL_ERR;
    }

    /* --- Receive phase (one-shot async on a private loop) --- */
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }
    AxlCancellable *cancel = axl_cancellable_new();
    if (cancel == NULL) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    UdpSyncRecvCtx r = { AXL_ERR, loop, rx_buf, rx_size, 0 };
    if (axl_udp_recv_async(sock, loop, cancel, on_udp_sync_recv_done, &r) != AXL_OK) {
        axl_cancellable_free(cancel);
        axl_loop_free(loop);
        return AXL_ERR;
    }

    axl_loop_add_timeout(loop, timeout_ms, on_udp_sync_cancel_timeout, cancel);
    AxlSourceId poll = udp_sync_arm_poll_tick(loop, sock->udp4);
    axl_loop_run(loop);
    udp_sync_disarm_poll_tick(loop, poll);

    /* recv_async is a PERSISTENT re-arming receiver; our cb returned false, so
       on_udp_recv_event removed the loop sources but left the socket's recv
       state (sock->loop / rx_event / pending token) intact for an eventual
       axl_udp_close. Tear it down here (idempotent — NULLs sock->loop) before
       freeing the ephemeral loop, so the NEXT sendrecv's axl_udp_recv_async
       doesn't see "recv already started". On a timeout, on_udp_recv_cancel
       already cleared it; the second call is a safe no-op. */
    udp_recv_drop_sources(sock);
    axl_cancellable_free(cancel);
    axl_loop_free(loop);

    if (r.st == AXL_OK) {
        *rx_len = r.rx_len;
        return AXL_OK;
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Public API: async receive (loop-integrated)
//
// Mirrors axl_tcp_recv_async — callback receives status, returns bool
// (true = re-arm, false = stop), takes optional AxlCancellable. The
// pre-parity-sweep API was axl_udp_recv_start + axl_udp_recv_stop:
// no status, no cancel, void return — the consumer had to gate stop
// via a separate function call from inside the callback (cost: extra
// branch in axl-socket.c's bridge; no error visibility for recv
// failures).
// ---------------------------------------------------------------------------

/* Tear down both loop sources + the rx event. Used on stop (from
   the cb's false return) AND on cancel — keeps the cleanup path
   identical so the EFI Cancel call and event teardown can never
   double-fire. */
static void
udp_recv_drop_sources(AxlUdp *sock)
{
    if (sock->loop_source != 0) {
        axl_loop_remove_source(sock->loop, sock->loop_source);
        sock->loop_source = 0;
    }
    if (sock->cancel_source != 0) {
        axl_loop_remove_source(sock->loop, sock->cancel_source);
        sock->cancel_source = 0;
    }
    if (sock->udp4 != NULL) {
        axl_efi_call(sock->udp4->Cancel, 2, sock->udp4, &sock->rx_token);
    }
    if (sock->rx_event != NULL) {
        axl_bs()->CloseEvent(sock->rx_event);
        sock->rx_event = NULL;
    }
    sock->loop         = NULL;
    sock->on_recv      = NULL;
    sock->on_recv_data = NULL;
}

static bool
on_udp_recv_event(void *data)
{
    AxlUdp    *sock = (AxlUdp *)data;
    AxlUdpCallback   cb;
    void            *cb_data;
    AxlStatus        cb_status;
    bool             keep;
    /* Snapshot loop + source IDs BEFORE invoking the user cb.
       Mirrors the TCP recv path: if cb returns false, it MAY have
       called axl_udp_close on sock (freeing it), so post-cb cleanup
       must use these saved IDs and never deref sock. Contract for
       true: cb MUST NOT close sock (we re-arm Receive on it below). */
    AxlLoop  *saved_loop       = sock->loop;
    AxlSourceId  saved_loop_src   = sock->loop_source;
    AxlSourceId  saved_cancel_src = sock->cancel_source;

    axl_efi_call(sock->udp4->Poll, 1, sock->udp4);

    cb      = sock->on_recv;
    cb_data = sock->on_recv_data;

    if (sock->rx_token.Status != 0 || sock->rx_token.Packet.RxData == NULL) {
        /* UEFI-reported error — surface to consumer with empty
           datagram + AXL_ERR status. The UEFI op is already done;
           we always tear down regardless of cb return. cb may close
           sock — only touch the saved source IDs after. */
        cb_status = AXL_ERR;
        if (cb != NULL) {
            cb(sock, cb_status, NULL, 0, NULL, 0, cb_data);
        }
        if (saved_loop_src != 0) {
            axl_loop_remove_source(saved_loop, saved_loop_src);
        }
        if (saved_cancel_src != 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }

    EFI_UDP4_RECEIVE_DATA *rx = sock->rx_token.Packet.RxData;

    /* Gather fragments into a contiguous buffer */
    size_t total = 0;
    for (uint32_t i = 0; i < rx->FragmentCount; i++) {
        total += rx->FragmentTable[i].FragmentLength;
    }

    AxlIPv4Address from;
    axl_memcpy(&from, &rx->UdpSession.SourceAddress, 4);
    uint16_t from_port = rx->UdpSession.SourcePort;

    cb_status = AXL_OK;
    keep      = true;

    void *buf = NULL;
    if (total <= 2048) {
        /* Stack buffer for small datagrams */
        uint8_t stack_buf[2048];
        size_t pos = 0;
        for (uint32_t i = 0; i < rx->FragmentCount; i++) {
            axl_memcpy(stack_buf + pos,
                       rx->FragmentTable[i].FragmentBuffer,
                       rx->FragmentTable[i].FragmentLength);
            pos += rx->FragmentTable[i].FragmentLength;
        }

        axl_bs()->SignalEvent(rx->RecycleSignal);

        if (cb != NULL) {
            keep = cb(sock, cb_status, stack_buf, total,
                      &from, from_port, cb_data);
        }
    } else {
        buf = axl_malloc(total);
        if (buf != NULL) {
            size_t pos = 0;
            for (uint32_t i = 0; i < rx->FragmentCount; i++) {
                axl_memcpy((uint8_t *)buf + pos,
                           rx->FragmentTable[i].FragmentBuffer,
                           rx->FragmentTable[i].FragmentLength);
                pos += rx->FragmentTable[i].FragmentLength;
            }

            axl_bs()->SignalEvent(rx->RecycleSignal);

            if (cb != NULL) {
                keep = cb(sock, cb_status, buf, total,
                          &from, from_port, cb_data);
            }
            axl_free(buf);
        } else {
            axl_bs()->SignalEvent(rx->RecycleSignal);
        }
    }

    if (!keep) {
        /* sock may be freed by cb (axl_udp_close in the false branch
           is allowed). Only touch saved source IDs. axl_loop_remove_source
           on a stale ID is a harmless no-op (close has already removed
           the source via sock->loop_source if it ran). The rx_event +
           any pending Receive op live until the eventual axl_udp_close;
           same contract as TCP recv_async. */
        if (saved_loop_src != 0) {
            axl_loop_remove_source(saved_loop, saved_loop_src);
        }
        if (saved_cancel_src != 0) {
            axl_loop_remove_source(saved_loop, saved_cancel_src);
        }
        return AXL_SOURCE_REMOVE;
    }

    /* keep == true: cb did NOT close sock. Re-arm Receive into the
       same rx_token / rx_event. */
    axl_memset(&sock->rx_token, 0, sizeof(sock->rx_token));
    sock->rx_token.Event  = sock->rx_event;
    sock->rx_token.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData = NULL;

    EFI_STATUS status = axl_efi_call(sock->udp4->Receive, 2,
                                     sock->udp4, &sock->rx_token);
    if (status != 0) {
        udp_recv_drop_sources(sock);
        return AXL_SOURCE_REMOVE;
    }

    return AXL_SOURCE_CONTINUE;
}

static bool
on_udp_recv_cancel(void *data)
{
    AxlUdp   *sock    = (AxlUdp *)data;
    AxlUdpCallback  cb      = sock->on_recv;
    void           *cb_data = sock->on_recv_data;

    /* Tear down BEFORE invoking the cb so the cb is free to call
       axl_udp_close on the sock (close re-runs drop_sources but
       it's idempotent — sock->loop becomes NULL after the first
       run). Cancel is terminal; return value ignored. */
    udp_recv_drop_sources(sock);
    if (cb != NULL) {
        cb(sock, AXL_CANCELLED, NULL, 0, NULL, 0, cb_data);
    }
    return AXL_SOURCE_REMOVE;
}

int
axl_udp_recv_async(
    AxlUdp   *sock,
    AxlLoop        *loop,
    AxlCancellable *cancel,
    AxlUdpCallback  cb,
    void           *data
    )
{
    if (sock == NULL || sock->udp4 == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    if (sock->loop != NULL) {
        axl_debug("recv already started");
        return AXL_ERR;
    }

    /* Create event for receive signaling */
    EFI_STATUS status = axl_bs()->CreateEvent(0, 0, NULL, NULL,
                                              &sock->rx_event);
    if (status != 0) {
        return AXL_ERR;
    }

    sock->loop         = loop;
    sock->on_recv      = cb;
    sock->on_recv_data = data;

    axl_memset(&sock->rx_token, 0, sizeof(sock->rx_token));
    sock->rx_token.Event  = sock->rx_event;
    sock->rx_token.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData = NULL;

    status = axl_efi_call(sock->udp4->Receive, 2,
                          sock->udp4, &sock->rx_token);
    if (status != 0) {
        axl_bs()->CloseEvent(sock->rx_event);
        sock->rx_event = NULL;
        sock->loop = NULL;
        return AXL_ERR;
    }

    sock->loop_source = axl_loop_add_event(
        loop, sock->rx_event, on_udp_recv_event, sock);

    if (cancel != NULL) {
        sock->cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_udp_recv_cancel,
            sock);
    }

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API: async send (loop-integrated)
//
// Mirrors axl_tcp_send_async: callback receives status, optional
// AxlCancellable, no preemption (one in-flight send per socket).
// ---------------------------------------------------------------------------

static void
udp_send_drop_sources(AxlUdp *sock)
{
    if (sock->tx_source != 0) {
        axl_loop_remove_source(sock->tx_loop, sock->tx_source);
        sock->tx_source = 0;
    }
    if (sock->tx_cancel_source != 0) {
        axl_loop_remove_source(sock->tx_loop, sock->tx_cancel_source);
        sock->tx_cancel_source = 0;
    }
    if (sock->udp4 != NULL && sock->tx_in_flight) {
        axl_efi_call(sock->udp4->Cancel, 2, sock->udp4, &sock->tx_token);
    }
    if (sock->tx_event != NULL) {
        axl_bs()->CloseEvent(sock->tx_event);
        sock->tx_event = NULL;
    }
    sock->tx_in_flight = false;
    sock->tx_loop      = NULL;
    sock->on_send      = NULL;
    sock->on_send_data = NULL;
}

static bool
on_udp_send_event(void *data)
{
    AxlUdp       *sock = (AxlUdp *)data;
    AxlUdpSendCallback  cb;
    void               *cb_data;
    AxlStatus           cb_status;
    /* Snapshot so cb may close sock from inside the callback. */
    AxlLoop  *saved_loop          = sock->tx_loop;
    AxlSourceId  saved_tx_src        = sock->tx_source;
    AxlSourceId  saved_tx_cancel_src = sock->tx_cancel_source;
    EFI_EVENT saved_tx_event      = sock->tx_event;

    axl_efi_call(sock->udp4->Poll, 1, sock->udp4);

    cb        = sock->on_send;
    cb_data   = sock->on_send_data;
    cb_status = (sock->tx_token.Status == 0) ? AXL_OK : AXL_ERR;

    /* Clear in-flight state on sock BEFORE cb so a follow-up
       send_async from inside the cb can succeed. tx_event will be
       closed via saved handle below. */
    sock->tx_in_flight     = false;
    sock->tx_source        = 0;
    sock->tx_cancel_source = 0;
    sock->tx_event         = NULL;
    sock->tx_loop          = NULL;
    sock->on_send          = NULL;
    sock->on_send_data     = NULL;

    /* Close the EFI event + drop loop sources via the snapshot. The
       cb is then free to call axl_udp_close (which is now safe — we
       already cleared all our own state). */
    if (saved_tx_src != 0) {
        axl_loop_remove_source(saved_loop, saved_tx_src);
    }
    if (saved_tx_cancel_src != 0) {
        axl_loop_remove_source(saved_loop, saved_tx_cancel_src);
    }
    if (saved_tx_event != NULL) {
        axl_bs()->CloseEvent(saved_tx_event);
    }

    if (cb != NULL) {
        cb(sock, cb_status, cb_data);
    }
    return AXL_SOURCE_REMOVE;
}

static bool
on_udp_send_cancel(void *data)
{
    AxlUdp       *sock    = (AxlUdp *)data;
    AxlUdpSendCallback  cb      = sock->on_send;
    void               *cb_data = sock->on_send_data;

    udp_send_drop_sources(sock);
    if (cb != NULL) {
        cb(sock, AXL_CANCELLED, cb_data);
    }
    return AXL_SOURCE_REMOVE;
}

int
axl_udp_send_async(
    AxlUdp          *sock,
    const AxlIPv4Address  *dest,
    uint16_t               port,
    const void            *buf,
    size_t                 len,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlUdpSendCallback     cb,
    void                  *data
    )
{
    if (sock == NULL || sock->udp4 == NULL ||
        buf == NULL || len == 0 || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }
    if (dest == NULL && !sock->peer_set) {
        return AXL_ERR;
    }
    if (sock->tx_in_flight) {
        return AXL_ERR;
    }

    EFI_STATUS status = axl_bs()->CreateEvent(0, 0, NULL, NULL,
                                              &sock->tx_event);
    if (status != 0) {
        return AXL_ERR;
    }

    /* Build session + fragment + transmit data in struct storage so
       they outlive this call (UEFI Transmit references them until
       completion). When dest is NULL the configured peer is used —
       pass NULL UdpSessionData per UEFI 2.x spec §27.4.1. */
    EFI_UDP4_SESSION_DATA *session_ptr = NULL;
    if (dest != NULL) {
        axl_memset(&sock->tx_session, 0, sizeof(sock->tx_session));
        axl_memcpy(&sock->tx_session.DestinationAddress, dest, 4);
        sock->tx_session.DestinationPort = port;
        session_ptr = &sock->tx_session;
    }

    sock->tx_frag.FragmentLength = (uint32_t)len;
    sock->tx_frag.FragmentBuffer = (void *)buf;

    axl_memset(&sock->tx_data, 0, sizeof(sock->tx_data));
    sock->tx_data.UdpSessionData   = session_ptr;
    sock->tx_data.GatewayAddress   = NULL;
    sock->tx_data.DataLength       = (uint32_t)len;
    sock->tx_data.FragmentCount    = 1;
    sock->tx_data.FragmentTable[0] = sock->tx_frag;

    axl_memset(&sock->tx_token, 0, sizeof(sock->tx_token));
    sock->tx_token.Event       = sock->tx_event;
    sock->tx_token.Status      = EFI_ABORTED;
    sock->tx_token.Packet.TxData = &sock->tx_data;

    sock->tx_in_flight = true;
    sock->tx_loop      = loop;
    sock->on_send      = cb;
    sock->on_send_data = data;

    status = axl_efi_call(sock->udp4->Transmit, 2, sock->udp4, &sock->tx_token);
    if (status != 0) {
        sock->tx_in_flight = false;
        sock->tx_loop      = NULL;
        sock->on_send      = NULL;
        sock->on_send_data = NULL;
        axl_bs()->CloseEvent(sock->tx_event);
        sock->tx_event = NULL;
        return AXL_ERR;
    }

    sock->tx_source = axl_loop_add_event(
        loop, sock->tx_event, on_udp_send_event, sock);

    if (cancel != NULL) {
        sock->tx_cancel_source = axl_loop_add_event(
            loop,
            _axl_cancellable_event(cancel),
            on_udp_send_cancel,
            sock);
    }

    return AXL_OK;
}
