/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-loop-internal.h
    Internal types shared by axl-loop.c, axl-defer.c, axl-pubsub.c.
    NOT a public header — never include from outside src/loop/.
**/

#ifndef AXL_LOOP_INTERNAL_H
#define AXL_LOOP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-defer.h>
#include <axl/axl-pubsub.h>
#include <axl/axl-loop.h>
#include <axl/axl-ring-buf.h>
#include "../backend/axl-backend.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* 64 slots covers an http-server bursting at curl-storm rates: 1
   accept + 8 active recv + up to ~20 outstanding close ctxes (each
   holds a slot for ~TIME_WAIT seconds while the async-close
   finalizes) + per-conn cancellables, with healthy headroom for
   caller-registered timers / idle / pubsub. */
#define AXL_MAX_SOURCES       64
#define AXL_DEFER_BUF_SIZE    1024  /* power of 2, holds ~42 DeferEntry */
#define AXL_MAX_TOPICS        16
#define POLL_INTERVAL_MS      10
#define MS_TO_100NS           10000ULL

// ---------------------------------------------------------------------------
// Loop source (event source registered with the loop)
// ---------------------------------------------------------------------------

typedef enum {
    SOURCE_TIMER,
    SOURCE_TIMEOUT,
    SOURCE_KEYPRESS,
    SOURCE_IDLE,
    SOURCE_PROTOCOL,
    SOURCE_EVENT
} SourceType;

typedef struct {
    AxlSourceId     id;
    SourceType      type;
    bool            active;
    bool            owns_event;
    /* SOURCE_IDLE only: added with axl_loop_add_idle_busy, so a driver tick
       keeps iterating while it exists (see driver_dispatch_notify). */
    bool            busy;
    AxlEventHandle  event;
    union {
        AxlLoopCallback cb;
        AxlKeyCallback  key_cb;
    } fn;
    void            *data;
} LoopSource;

// ---------------------------------------------------------------------------
// Defer queue entry
// ---------------------------------------------------------------------------

typedef struct {
    AxlDeferCallback  fn;
    void             *data;
    uint32_t          id;
    bool              cancelled;
} DeferEntry;

// ---------------------------------------------------------------------------
// Pub/sub topic table entry
// ---------------------------------------------------------------------------

typedef struct Subscriber {
    AxlPubsubCallback    cb;
    void                *data;
    uint32_t             id;
    struct Subscriber   *next;
} Subscriber;

typedef struct {
    const char  *name;
    Subscriber  *subscribers;
    bool         active;
} PubsubTopic;

// ---------------------------------------------------------------------------
// AxlLoop — full struct definition (private)
// ---------------------------------------------------------------------------

struct AxlLoop {
    // Tier-1 registry handle (0 = unregistered)
    uint32_t        _registry_handle;

    // Event sources
    bool            running;
    bool            quit_requested;
    /* The caller's explicit Ctrl-C choice (axl_loop_set_intercept_break),
     * valid only when intercept_break_set.  When false, the loop does NOT
     * treat a bare Ctrl-C (UnicodeChar=0x03, KeyShiftState=0) or the shell
     * break event as "quit"; the 0x03 byte is delivered to the keypress
     * source instead.  A GUI app (an editor mapping Ctrl+C to Copy) sets it
     * off.  With no explicit choice the loop intercepts only in the
     * foreground: not while driver-attached, and never in a driver image --
     * see loop_intercepts_break(). */
    bool            intercept_break;
    bool            intercept_break_set;
    LoopSource      sources[AXL_MAX_SOURCES];
    size_t          source_count;
    int             pending_source;
    /* Where the next wait starts scanning, as a WAIT KEY: a source slot
     * index for user sources, AXL_MAX_SOURCES + n for the loop's own
     * events (see wait_key in axl-loop.c). Set to one past the key of
     * whatever the last wait selected, so a source that stays signalled
     * cannot win every iteration and starve the ones after it. Per loop,
     * not global: loops nest (axl_loop_iterate_until, sync wrappers). */
    size_t          wait_rotor;
    AxlEventHandle  break_event;
    AxlEventHandle  poll_timer;
    /* ConIn->WaitForKey, captured at loop creation. Always added to the
     * WaitForEvent array so we can intercept raw serial Ctrl-C
     * (UnicodeChar=0x03, KeyShiftState=0 — what TerminalDxe emits) and
     * signal break. Doesn't conflict with user SOURCE_KEYPRESS sources:
     * those wait on the same event, so the read in dispatch covers
     * both — see the keypress-source path in axl_loop_dispatch_event. */
    AxlEventHandle  keypress_event;
    AxlLoopCallback cleanups[AXL_MAX_SOURCES];
    void           *cleanup_data[AXL_MAX_SOURCES];
    size_t          cleanup_count;

    // Defer queue (ring buffer)
    AxlRingBuf      defer_ring;
    uint8_t         defer_buf[AXL_DEFER_BUF_SIZE];
    uint32_t        defer_next_id;

    // Pub/sub topic table
    PubsubTopic     topics[AXL_MAX_TOPICS];
    size_t          topic_count;
    uint32_t        pubsub_next_sub_id;

    // Driver-mode attachment — firmware-managed periodic timer that
    // drives axl_loop_dispatch from TPL_APPLICATION notify. NULL when
    // not attached. See axl_loop_attach_driver.
    AxlEventHandle  driver_timer;
    // Consecutive driver ticks that drained their full per-tick budget. A
    // single busy tick is normal back-pressure (a high-output burst); a
    // sustained run signals a runaway callback or persistent overload. Used
    // to gate the drain-cap diagnostic in driver_dispatch_notify.
    uint32_t        drain_cap_streak;
    // Driver-tick extension for busy idle sources: the per-tick budget
    // (axl_loop_set_driver_budget_ms), the attached period it is clamped to
    // half of, and whether the monotonic clock was calibrated successfully
    // OUTSIDE a tick (clock_warm: attempted; clock_ok: it worked). A tick
    // never reads the clock unless clock_ok -- a failed calibration is not
    // memoised by the backend and would stall 10 ms on every read.
    uint32_t        driver_budget_ms;
    uint64_t        driver_interval_ms;
    // AxlTcp's deferred-close sweeper on this loop (axl-tcp-sync.c), 0 when
    // none: it finalizes the loop's graceful closes as the firmware completes
    // them. Still active at axl_loop_free means closes are pending on a loop
    // that is going away, which axl_loop_free reports like a caller-owned
    // source.
    AxlSourceId     tcp_close_sweeper;
    // Next driver-attached loop (g_driver_loops in axl-loop.c), so the
    // network latency policy can shorten every attached tick while fast
    // (_axl_loop_driver_fast). Changed only at TPL_CALLBACK.
    struct AxlLoop *driver_next;
    bool            clock_warm;
    bool            clock_ok;
    // Waiting-foreground extension (axl_loop_set_driver_idle_budget_ms).
    // driver_dispatch_notify says how these decide it; all times are raw
    // counter ticks (axl_backend_counter_ticks), 0 = none.
    //   driver_idle_budget_ms  the budget
    //   driver_pass_event      EDK2 idle-loop member at the idle pass
    //                          (TPL_NOTIFY, before the halt), or NULL
    //   driver_wake_event      ... and after the halt (TPL_CALLBACK), or NULL
    //   driver_recent_ticks    DRIVER_IDLE_RECENT_US in ticks; 0 = no
    //                          counter, so the foreground never counts as
    //                          waiting
    //   driver_ref_ct          the foreground's last chance to run: the end
    //                          of the last tick or of the last halt
    //   driver_wake_ct         the end of the last halt
    //   driver_ran             an idle pass came long after driver_ref_ct
    //                          since the last tick: the foreground ran
    //   driver_prev_idle       the previous tick's period was idle
    // The pass member is TPL_NOTIFY and never runs inside a tick (the group
    // is signalled only at TPL_APPLICATION); it only reads the counter and
    // stores.
    uint32_t        driver_idle_budget_ms;
    AxlEventHandle  driver_pass_event;
    AxlEventHandle  driver_wake_event;
    uint64_t        driver_recent_ticks;
    uint64_t        driver_ref_ct;
    uint64_t        driver_wake_ct;
    bool            driver_ran;
    bool            driver_prev_idle;
};

// ---------------------------------------------------------------------------
// Internal functions called by axl-loop.c
// ---------------------------------------------------------------------------

/// Drain all pending deferred work for this loop.
void axl_defer_drain_internal(AxlLoop *loop);

/// Free all subscribers and reset the topic table.
void axl_pubsub_reset_internal(AxlLoop *loop);

/// Loop-callback re-entrancy depth (defined in axl-loop.c). Bracket every
/// dispatched user callback with enter()/leave(); the sync wait primitive
/// reads _axl_loop_in_callback() to detect a blocking wait nested inside a
/// callback. _axl_loop_in_callback() is also forward-declared by the sync
/// wait primitive (src/event/axl-wait.c), which cannot include this
/// src/loop-only header.
void _axl_loop_cb_enter(void);
void _axl_loop_cb_leave(void);
bool _axl_loop_in_callback(void);

/// Test seam (axl-test-net's receive re-arm tests): whether @p id names an
/// active source on @p loop. Source ids are unique across loops, so removing
/// an id from the WRONG loop is a silent no-op that leaves the source
/// registered; this is how a test sees which loop actually holds it.
/// Forward-declared by the test, which cannot include this header.
bool _axl_loop_has_source(const AxlLoop *loop, AxlSourceId id);

/// Run every driver-attached loop's tick at most every @p tick_us
/// microseconds (0: back to each loop's attached interval), and attach new
/// loops the same way until called again. For axl_net_set_latency(): while
/// the network is fast, a driver tick -- which dispatches what a poll
/// completes -- must keep up with it. Callable at TPL_CALLBACK or below.
void _axl_loop_driver_fast(uint32_t tick_us);

#endif /* AXL_LOOP_INTERNAL_H */
