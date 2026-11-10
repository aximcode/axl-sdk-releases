/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-latency.c
    axl_net_set_latency: trade CPU for AxlTcp round-trip latency.

    EDK2's network stack has no receive interrupt; a frame waits for the
    firmware's tick and then the stack's own 10 ms receive timer. "Fast"
    raises the tick (axl_timer_resolution_begin) AND polls every live AxlTcp
    socket's TCP instance from a periodic timer event at TPL_CALLBACK -- what
    the stack's receive timer does every 10 ms -- AND shortens every
    driver-attached loop's tick (_axl_loop_driver_fast), which dispatches
    what the poll completes. The contract is axl-net.h.

    State is process-wide and changes only at TPL_CALLBACK: the poll notify
    runs there (and ends WINDOW's fast spell), and the application-level
    callers raise to it around every change. EDK2's TCP functions raise to
    TPL_CALLBACK themselves, so a poll cannot cut into a TCP call in progress.
**/

#include <axl/axl-net.h>
#include <axl/axl-atexit.h>
#include <axl/axl-log.h>
#include <axl/axl-time.h>     /* axl_timer_resolution_* */
#include "axl-tcp-internal.h"
#include "../backend/axl-backend.h"
#include "../loop/axl-loop-internal.h"

AXL_LOG_DOMAIN("net");

static AxlNetLatency       g_policy = {
    AXL_NET_LATENCY_FIRMWARE, AXL_NET_LATENCY_WINDOW_DEFAULT_MS, AXL_NET_LATENCY_TICK_DEFAULT_US
};
static AxlTcp             *g_live;          /* every live AxlTcp, via lat_next */
static bool                g_fast;
static AxlTimerResolutionId g_resolution;
static AxlEventHandle      g_poll_event;
static uint32_t            g_idle_ticks;    /* poll ticks since the last activity */
static uint32_t            g_atexit;
static bool                g_exited;        /* the exit handler ran: never fast again */

static void latency_exit_fast(void);

// ---------------------------------------------------------------------------
// The poll
// ---------------------------------------------------------------------------

static void
latency_poll_notify(
    void *ctx
    )
{
    (void)ctx;
    for (AxlTcp *s = g_live; s != NULL; s = s->lat_next) {
        if (s->tcp4 != NULL) {
            axl_efi_call(s->tcp4->Poll, 1, s->tcp4);
        }
    }
    /* Counted in ticks, not read from a clock: a clock whose calibration
       failed stalls ~10 ms per read and reads 0, which here would wedge the
       image at TPL_CALLBACK and keep WINDOW fast forever. */
    g_idle_ticks++;
    if (g_policy.mode == AXL_NET_LATENCY_WINDOW
        && (uint64_t)g_idle_ticks * g_policy.tick_us >= (uint64_t)g_policy.window_ms * 1000u) {
        latency_exit_fast();
    }
}

// ---------------------------------------------------------------------------
// Fast and back (TPL_CALLBACK)
// ---------------------------------------------------------------------------

static void
latency_release_at_exit(
    void *data
    )
{
    (void)data;
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    latency_exit_fast();
    /* Later exit handlers (LIFO: one registered before the first socket, a
       C++ destructor) may still arm or close a socket. Fast again now would
       create a timer whose notify is this image's code, firing after it has
       unloaded. */
    g_exited = true;
    axl_backend_tpl_restore(old);
    /* The notify is this image's code: the event must not outlive it. */
    if (g_poll_event != NULL) {
        axl_backend_event_close(g_poll_event);
        g_poll_event = NULL;
    }
}

static void
latency_enter_fast(void)
{
    if (g_fast || g_exited) {
        return;
    }
    uint64_t period = (uint64_t)g_policy.tick_us * 10u;   /* 100 ns units */
    if (g_poll_event == NULL) {
        if (axl_backend_event_create_notify_timer(latency_poll_notify, NULL, period,
                                                  &g_poll_event) != AXL_OK) {
            g_poll_event = NULL;
            return;   /* no event to poll from: stay at the firmware's pace */
        }
        if (g_atexit == 0) {
            g_atexit = axl_atexit(latency_release_at_exit, NULL);
        }
    } else {
        axl_backend_event_set_timer(g_poll_event, AXL_TIMER_PERIODIC, period);
    }
    if (axl_timer_resolution_begin(g_policy.tick_us, &g_resolution, NULL) != AXL_OK) {
        g_resolution = 0;   /* polling still helps a little; keep going */
    }
    _axl_loop_driver_fast(g_policy.tick_us);
    g_idle_ticks = 0;
    g_fast       = true;
}

static void
latency_exit_fast(void)
{
    if (!g_fast) {
        return;
    }
    if (g_poll_event != NULL) {
        axl_backend_event_set_timer(g_poll_event, AXL_TIMER_CANCEL, 0);
    }
    if (g_resolution != 0) {
        axl_timer_resolution_end(g_resolution);
        g_resolution = 0;
    }
    _axl_loop_driver_fast(0);
    g_fast = false;
}

// ---------------------------------------------------------------------------
// Hooks from AxlTcp
// ---------------------------------------------------------------------------

void
_axl_net_latency_activity(void)
{
    if (g_policy.mode != AXL_NET_LATENCY_WINDOW) {
        return;
    }
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    g_idle_ticks = 0;
    latency_enter_fast();
    axl_backend_tpl_restore(old);
}

void
_axl_net_latency_link(
    AxlTcp *sock
    )
{
    if (sock == NULL || sock->lat_linked) {
        return;
    }
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    sock->lat_next   = g_live;
    sock->lat_linked = true;
    g_live           = sock;
    if (g_policy.mode == AXL_NET_LATENCY_ALWAYS) {
        latency_enter_fast();
    }
    axl_backend_tpl_restore(old);
    _axl_net_latency_activity();
}

void
_axl_net_latency_unlink(
    AxlTcp *sock
    )
{
    if (sock == NULL || !sock->lat_linked) {
        return;
    }
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    for (AxlTcp **pp = &g_live; *pp != NULL; pp = &(*pp)->lat_next) {
        if (*pp == sock) {
            *pp = sock->lat_next;
            break;
        }
    }
    sock->lat_next   = NULL;
    sock->lat_linked = false;
    if (g_policy.mode == AXL_NET_LATENCY_ALWAYS && g_live == NULL) {
        latency_exit_fast();
    }
    axl_backend_tpl_restore(old);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_net_latency_defaults(
    AxlNetLatency *out
    )
{
    if (out == NULL) {
        return;
    }
    out->mode      = AXL_NET_LATENCY_FIRMWARE;
    out->window_ms = AXL_NET_LATENCY_WINDOW_DEFAULT_MS;
    out->tick_us   = AXL_NET_LATENCY_TICK_DEFAULT_US;
}

void
axl_net_get_latency(
    AxlNetLatency *out
    )
{
    if (out != NULL) {
        *out = g_policy;
    }
}

int
axl_net_set_latency(
    const AxlNetLatency *cfg
    )
{
    AxlNetLatency next;
    if (cfg == NULL) {
        axl_net_latency_defaults(&next);
    } else {
        next = *cfg;
        if (next.window_ms == 0) {
            next.window_ms = AXL_NET_LATENCY_WINDOW_DEFAULT_MS;
        }
        if (next.tick_us == 0) {
            next.tick_us = AXL_NET_LATENCY_TICK_DEFAULT_US;
        }
    }
    /* Only the fields the mode uses are checked; one it ignores, if out of
       range, is kept at its default so a later mode change starts sane. */
    if ((unsigned)next.mode > (unsigned)AXL_NET_LATENCY_ALWAYS) {
        return AXL_INVALID;
    }
    bool uses_tick   = next.mode != AXL_NET_LATENCY_FIRMWARE;
    bool uses_window = next.mode == AXL_NET_LATENCY_WINDOW;
    bool tick_ok     = next.tick_us >= AXL_TIMER_RESOLUTION_MIN_US
                       && next.tick_us <= AXL_NET_LATENCY_TICK_MAX_US;
    bool window_ok   = next.window_ms <= AXL_NET_LATENCY_WINDOW_MAX_MS;
    if ((uses_tick && !tick_ok) || (uses_window && !window_ok)) {
        return AXL_INVALID;
    }
    if (!tick_ok) {
        next.tick_us = AXL_NET_LATENCY_TICK_DEFAULT_US;
    }
    if (!window_ok) {
        next.window_ms = AXL_NET_LATENCY_WINDOW_DEFAULT_MS;
    }
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    latency_exit_fast();
    g_policy = next;
    if (g_policy.mode == AXL_NET_LATENCY_ALWAYS && g_live != NULL) {
        latency_enter_fast();
    }
    axl_backend_tpl_restore(old);
    return AXL_OK;
}
