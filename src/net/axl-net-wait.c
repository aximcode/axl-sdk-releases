/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-wait.c
    Tier 4 per-protocol wait helpers (UDP4, TCP4, DNS4). Each is a
    thin wrapper that drives the protocol's Poll() as the periodic
    tick of AxlWait's internal _axl_event_wait_timeout_with_tick
    primitive. The protocols' EFI driver advances while the CPU
    idles between tick events.
**/

#include "axl-net-internal.h"
#include "../event/axl-wait-internal.h"

#include <stdint.h>

/* 10 ms tick keeps the driver-advancement hint without burning CPU
   at the old idiom's 1 ms stall cadence. */
#define AXL_NET_POLL_TICK_US  10000ULL

// ---------------------------------------------------------------------------
// UDP4 — the per-protocol wait helper is gone: axl_udp_send / _sendrecv now
// wrap axl_udp_send_async / _recv_async on a private loop (which drives Poll
// itself at a raised TPL), so nothing needs the standalone _axl_udp_wait.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TCP4
// ---------------------------------------------------------------------------

static void
tcp_poll_tick(void *ctx)
{
    EFI_TCP4_PROTOCOL *tcp4 = (EFI_TCP4_PROTOCOL *)ctx;
    axl_efi_call(tcp4->Poll, 1, tcp4);
}

AxlStatus
_axl_tcp_wait(
    EFI_TCP4_PROTOCOL *tcp4,
    EFI_EVENT          event,
    uint64_t           timeout_us
    )
{
    if (tcp4 == NULL || event == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(
        (AxlEventHandle)event,
        NULL, NULL,
        tcp_poll_tick, tcp4,
        AXL_NET_POLL_TICK_US, NULL, timeout_us);
}

// ---------------------------------------------------------------------------
// DNS4
// ---------------------------------------------------------------------------

static void
dns_poll_tick(void *ctx)
{
    EFI_DNS4_PROTOCOL *dns4 = (EFI_DNS4_PROTOCOL *)ctx;
    axl_efi_call(dns4->Poll, 1, dns4);
}

AxlStatus
_axl_dns_wait(
    EFI_DNS4_PROTOCOL *dns4,
    EFI_EVENT          event,
    uint64_t           timeout_us
    )
{
    if (dns4 == NULL || event == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(
        (AxlEventHandle)event,
        NULL, NULL,
        dns_poll_tick, dns4,
        AXL_NET_POLL_TICK_US, NULL, timeout_us);
}

// ---------------------------------------------------------------------------
// IP4
// ---------------------------------------------------------------------------

static void
ip4_poll_tick(void *ctx)
{
    EFI_IP4_PROTOCOL *ip4 = (EFI_IP4_PROTOCOL *)ctx;
    axl_efi_call(ip4->Poll, 1, ip4);
}

AxlStatus
_axl_ip4_wait(
    EFI_IP4_PROTOCOL *ip4,
    EFI_EVENT         event,
    uint64_t          timeout_us
    )
{
    if (ip4 == NULL || event == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(
        (AxlEventHandle)event,
        NULL, NULL,
        ip4_poll_tick, ip4,
        AXL_NET_POLL_TICK_US, NULL, timeout_us);
}
