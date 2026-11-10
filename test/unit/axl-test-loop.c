/** @file axl-test-loop.c
    Test application for AxlLoop, AxlDefer, and AxlPubsub.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-time.h>
#include <axl/axl-wait.h>
#include <uefi/axl-uefi.h>

/* WHY gBS->Stall AND NOT axl_usleep, throughout this file.
   axl_usleep/axl_msleep are the right answer nearly everywhere -- they idle
   the CPU on a one-shot timer instead of spinning, and this file already
   includes <axl/axl-wait.h> for other reasons. But they are built ON AxlLoop:
   axl_usleep -> axl_wait_ms -> _axl_event_wait_timeout_with_tick, which calls
   axl_loop_new(), axl_loop_add_timeout() and axl_loop_run(). Delaying with
   the subject under test is circular, and the failure it invites is not a
   wrong answer but a HANG: a loop defect that stops the wait returning never
   returns here either, and test-axl.sh runs every unit binary in ONE QEMU
   boot under ONE timeout, so one hung binary starves every later one.
   gBS->Stall always returns, so a broken loop fails loudly and locally.
   axl-wait.h names axl_backend_stall as the primitive for exactly this case,
   but it is backend-internal and tests use public headers only. The spin
   costs well under a second across this file; a hung suite costs the run. */

/* AxlLoop's internal keypress drain period (POLL_INTERVAL_MS in
   src/loop/axl-loop-internal.h), in microseconds. Duplicated rather than
   exported -- it is an implementation detail the library is free to retune.

   The constraint is ONE-DIRECTIONAL, so state it rather than claim the test
   is retune-proof: this value must be <= the library's period. Set HIGHER
   than the library's, the "was I preempted past a whole period" NOTE stops
   firing when it should, so a host stall becomes indistinguishable from the
   loop defect this test exists to catch. Set LOWER it is merely conservative
   -- the NOTE fires more readily, and the `2 *` Stall further below must
   still exceed one real period, which a lower value keeps true only while it
   stays above half the library's. If POLL_INTERVAL_MS ever drops below 10,
   drop this with it. */
#define POLL_INTERVAL_US            (10u * 1000u)

/* Bounds for the phase-pinning spins in test_keypress_drain_non_blocking.
   BOTH are needed. The wall-clock budget is the real limit (5 poll periods is
   ample for a tick that arrives every one), but axl_time_get_us() reports 0
   when the counter frequency is unavailable, and a spin with no backstop
   wedges QEMU -- which under test-axl.sh runs every binary in ONE boot under
   ONE timeout, so a wedge here starves every later binary. The iteration cap
   is that backstop, and it is sized rather than rounded: one iteration is a
   firmware CheckEvent, call it 1-10 us, and the budget to cover is 5 periods
   = 50 ms, so 200k iterations spans ~0.2-2 s. That comfortably overshoots the
   budget on any host fast enough to run this suite while staying far inside
   the single shared timeout test-axl.sh gives the whole boot -- which 2M
   iterations, at the slow end, would not. */
#define KEYPRESS_SPIN_BUDGET_US     (5u * POLL_INTERVAL_US)
#define KEYPRESS_SPIN_ITERS         200000u

/* The burst that replaced a SINGLE second dispatch, and why the shape
   changed.

   The invariant is "selected at most once per POLL TICK, not on every call".
   Sampling it with one call made the margin 1-vs-0: the assertion held only
   while the pair of calls stayed inside one 10 ms period, so any host
   preemption longer than that flipped it. Phase-pinning made the window a
   FULL period instead of an arbitrary 0..period slice, and the previous
   revision measured the window to excuse a stall -- but the excuse never
   fired on the failure it was written for (a loaded verify.sh, 2026-08-19:
   FAIL with no NOTE, so the guard's own window looked clean). It cannot
   reliably fire, because it compares axl_time_get_us() against a period timed
   by the FIRMWARE; under host contention those two clocks diverge, and the
   guard reads the one that under-reports.

   So stop sampling and stop excusing. 64 back-to-back dispatches turn the
   margin into 64-vs-0: a source that ignored the gate is selected by EVERY
   call, while a healthy one is selected only when a tick genuinely arrives
   mid-burst -- and each of those costs a full 10 ms, which the bound below
   allows for out of the burst's own measured duration. A 50 ms stall inside
   the burst now RAISES the bound instead of invalidating the test.

   64 calls is microseconds of CheckEvent on any host that runs this suite,
   so the healthy count is 0. SLACK covers the clock divergence above without
   needing to model it. The CEILING keeps the assertion able to FAIL: without
   it a long enough stall would compute an allowance >= the burst size, and a
   bound nothing can exceed is a green that means nothing. Hitting the ceiling
   is reported.

   WHAT THIS IS AND IS NOT EVIDENCE OF. The 2026-08-19 failure was ONE
   spurious re-selection (the single dispatch returned 0 instead of 1) inside
   a window the guard measured as under a period. That count is 1, and 1 <=
   SLACK -- so this shape would have PASSED that exact run. That is the claim
   being made, and it is checkable from the failure itself. What is NOT
   claimed is a reproduction: 12 runs of the OLD shape under 2x-nproc CPU hogs
   stayed green on BOTH x64 (KVM) and aa64 (TCG), so no control was found that
   moves the old assertion on demand, and a green run of THIS shape under the
   same load would therefore prove nothing. The case rests on the margin and
   the derived bound, not on a load test.

   THE COST, stated rather than buried: SLACK also admits a hypothetical
   regression that selected the source exactly TWICE per tick. No plausible
   edit produces that -- the gate is a single CheckEvent, so the failure modes
   are "gone" (64, caught here) and "always false" (0 ever, caught by the
   re-arm assertion below). Tightening SLACK to 1 would close it and leave
   zero headroom above the failure this is fixing, which is the trade that
   made the previous revision flaky. */
#define KEYPRESS_BURST_CALLS        64u
#define KEYPRESS_BURST_SLACK        2u
#define KEYPRESS_BURST_CEILING      (KEYPRESS_BURST_CALLS / 2u)

// ---------------------------------------------------------------------------
// Test 1: Timer fires N times then quits
// ---------------------------------------------------------------------------

static size_t timer_count;

static bool
on_timer_count(void *data)
{
    timer_count++;
    if (timer_count >= 5) {
        axl_loop_quit((AxlLoop *)data);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_timer_count(void)
{
    AxlLoop *loop;

    timer_count = 0;
    loop = axl_loop_new();
    test_check(loop != NULL, "loop: new");
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timer(loop, 50, on_timer_count, loop);
    axl_loop_run(loop);

    test_check(timer_count == 5, "loop: timer fired 5 times");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 2: Timeout fires (one-shot)
// ---------------------------------------------------------------------------

static bool timeout_fired;

static bool
on_timeout(void *data)
{
    timeout_fired = true;
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static void
test_timeout(void)
{
    AxlLoop *loop;

    timeout_fired = false;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timeout(loop, 100, on_timeout, loop);
    axl_loop_run(loop);

    test_check(timeout_fired, "loop: timeout fired");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 3: Idle runs every iteration
// ---------------------------------------------------------------------------

static size_t idle_count;
static size_t timer_count_2;

static bool
on_idle_count(void *data)
{
    (void)data;
    idle_count++;
    return AXL_SOURCE_CONTINUE;
}

static bool
on_timer_quit_3(void *data)
{
    timer_count_2++;
    if (timer_count_2 >= 3) {
        axl_loop_quit((AxlLoop *)data);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_idle_count(void)
{
    AxlLoop *loop;

    idle_count = 0;
    timer_count_2 = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_idle(loop, on_idle_count, NULL);
    axl_loop_add_timer(loop, 50, on_timer_quit_3, loop);
    axl_loop_run(loop);

    test_check(idle_count >= timer_count_2, "loop: idle count >= timer count");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 4: Quit from callback
// ---------------------------------------------------------------------------

static bool
on_quit_immediate(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static void
test_quit_from_callback(void)
{
    AxlLoop *loop;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timer(loop, 50, on_quit_immediate, loop);
    axl_loop_run(loop);

    test_pass("loop: quit from callback");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 5: Cleanup fires on exit
// ---------------------------------------------------------------------------

static bool cleanup_fired;

static bool
on_cleanup(void *data)
{
    (void)data;
    cleanup_fired = true;
    return AXL_SOURCE_CONTINUE;
}

static void
test_cleanup(void)
{
    AxlLoop *loop;

    cleanup_fired = false;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_cleanup(loop, on_cleanup, NULL);
    axl_loop_add_timeout(loop, 50, on_quit_immediate, loop);
    axl_loop_run(loop);

    test_check(cleanup_fired, "loop: cleanup fired");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 6: Dispatch non-blocking
// ---------------------------------------------------------------------------

static size_t dispatch_idle_count;

static bool
on_dispatch_idle(void *data)
{
    (void)data;
    dispatch_idle_count++;
    return AXL_SOURCE_CONTINUE;
}

static void
test_dispatch_non_blocking(void)
{
    AxlLoop *loop;

    dispatch_idle_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_idle(loop, on_dispatch_idle, NULL);

    /* Single non-blocking dispatch should fire idle once */
    axl_loop_dispatch(loop, false);
    test_check(dispatch_idle_count >= 1, "loop: dispatch fires idle");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 7: NextEvent + DispatchEvent separation
// ---------------------------------------------------------------------------

static size_t separate_count;

static bool
on_separate_timer(void *data)
{
    separate_count++;
    if (separate_count >= 3) {
        axl_loop_quit((AxlLoop *)data);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_next_dispatch_separate(void)
{
    AxlLoop *loop;
    int rc;

    separate_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timer(loop, 50, on_separate_timer, loop);

    /* Manual loop using primitives (FUSE-style) */
    for (;;) {
        rc = axl_loop_next_event(loop, true);
        if (rc == -1) {
            break;
        }
        if (rc == 0) {
            axl_loop_dispatch_event(loop);
        }
        if (!axl_loop_is_running(loop)) {
            break;
        }
    }

    test_check(separate_count == 3, "loop: next+dispatch separate fired 3");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 8: RemoveSource
// ---------------------------------------------------------------------------

static size_t remove_count;

static bool
on_remove_timer(void *data)
{
    (void)data;
    remove_count++;
    return AXL_SOURCE_CONTINUE;
}

static void
test_remove_source(void)
{
    AxlLoop *loop;
    AxlSourceId tid;

    remove_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    tid = axl_loop_add_timer(loop, 50, on_remove_timer, NULL);
    test_check(tid != 0, "loop: add timer returns ID");

    axl_loop_remove_source(loop, tid);

    /* Add a timeout to quit — if removed timer still fires, count > 0 */
    axl_loop_add_timeout(loop, 200, on_quit_immediate, loop);
    axl_loop_run(loop);

    test_check(remove_count == 0, "loop: removed timer did not fire");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 9: Timer + idle interaction (multiple source types)
// ---------------------------------------------------------------------------

static size_t multi_timer_count;
static size_t multi_idle_count;

static bool
on_multi_timer(void *data)
{
    (void)data;
    multi_timer_count++;
    return AXL_SOURCE_CONTINUE;
}

static bool
on_multi_idle(void *data)
{
    multi_idle_count++;
    if (multi_timer_count >= 3) {
        axl_loop_quit((AxlLoop *)data);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_timer_idle_interaction(void)
{
    AxlLoop *loop;

    multi_timer_count = 0;
    multi_idle_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timer(loop, 50, on_multi_timer, NULL);
    axl_loop_add_idle(loop, on_multi_idle, loop);
    axl_loop_run(loop);

    test_check(multi_timer_count >= 3,
               "loop: timer+idle: timer fired >= 3");
    test_check(multi_idle_count > multi_timer_count,
               "loop: timer+idle: idle ran more than timer");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 10: Timeout cancellation (remove before it fires)
// ---------------------------------------------------------------------------

static size_t cancelled_count;

static bool
on_cancelled_timeout(void *data)
{
    (void)data;
    cancelled_count++;
    return AXL_SOURCE_REMOVE;
}

static void
test_timeout_cancellation(void)
{
    AxlLoop *loop;
    AxlSourceId cancel_id;

    cancelled_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    /* Add a long timeout, then immediately remove it */
    cancel_id = axl_loop_add_timeout(loop, 500, on_cancelled_timeout, NULL);
    axl_loop_remove_source(loop, cancel_id);

    /* Add a short timeout that quits */
    axl_loop_add_timeout(loop, 100, on_quit_immediate, loop);
    axl_loop_run(loop);

    test_check(cancelled_count == 0, "loop: cancelled timeout did not fire");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 11: Multiple timers at different intervals
// ---------------------------------------------------------------------------

static size_t fast_count;
static size_t slow_count;

static bool
on_fast_timer(void *data)
{
    (void)data;
    fast_count++;
    return AXL_SOURCE_CONTINUE;
}

static bool
on_slow_timer(void *data)
{
    slow_count++;
    if (slow_count >= 2) {
        axl_loop_quit((AxlLoop *)data);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_multiple_timers(void)
{
    AxlLoop *loop;

    fast_count = 0;
    slow_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_loop_add_timer(loop, 30, on_fast_timer, NULL);
    axl_loop_add_timer(loop, 100, on_slow_timer, loop);
    axl_loop_run(loop);

    test_check(slow_count == 2,
               "loop: multi-timer: slow fired 2 times");
    test_check(fast_count > slow_count,
               "loop: multi-timer: fast fired more than slow");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 12: Raw event source (axl_loop_add_event)
// ---------------------------------------------------------------------------

static bool      raw_event_fired;
static AxlEvent *test_event;

static bool
on_raw_event(void *data)
{
    raw_event_fired = true;
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static bool
on_signal_idle(void *data)
{
    (void)data;
    /* Signal the event from an idle callback */
    axl_event_signal(test_event);
    return AXL_SOURCE_CONTINUE;
}

static void
test_raw_event_source(void)
{
    AxlLoop *loop;

    raw_event_fired = false;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    /* Create an AxlEvent and register its handle as a raw loop source. */
    test_event = axl_event_new();
    if (test_event == NULL) {
        test_fail("loop: raw event: new failed");
        axl_loop_free(loop);
        return;
    }

    axl_loop_add_event(loop, axl_event_handle(test_event),
                       on_raw_event, loop);
    axl_loop_add_idle(loop, on_signal_idle, NULL);

    axl_loop_run(loop);

    test_check(raw_event_fired, "loop: raw event source fires on signal");
    axl_event_free(test_event);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Shared helper: quit timer for defer/pubsub tests
// ---------------------------------------------------------------------------

static bool
on_defer_check_timer(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Test 13: Deferred work fires on next loop tick
// ---------------------------------------------------------------------------

static size_t defer_count;

static void
on_deferred(void *data)
{
    (void)data;
    defer_count++;
}

static void
test_defer_basic(void)
{
    AxlLoop *loop;

    defer_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_defer(loop, on_deferred, NULL);
    axl_defer(loop, on_deferred, NULL);
    axl_defer(loop, on_deferred, NULL);

    /* Timer quits after deferred work should have drained */
    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(defer_count == 3, "defer: basic: 3 callbacks fired");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 14: Deferred cancel prevents firing
// ---------------------------------------------------------------------------

static size_t defer_cancel_count;

static void
on_deferred_cancel(void *data)
{
    (void)data;
    defer_cancel_count++;
}

static void
test_defer_cancel(void)
{
    AxlLoop *loop;
    uint32_t h1, h2, h3;

    defer_cancel_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    h1 = axl_defer(loop, on_deferred_cancel, NULL);
    h2 = axl_defer(loop, on_deferred_cancel, NULL);
    h3 = axl_defer(loop, on_deferred_cancel, NULL);

    /* Cancel the middle one */
    test_check(axl_defer_cancel(loop, h2), "defer: cancel returns true");
    test_check(!axl_defer_cancel(loop, 999), "defer: cancel invalid returns false");

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(defer_cancel_count == 2, "defer: cancel: 2 of 3 fired");
    (void)h1; (void)h3;
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 15: Deferred work preserves FIFO order
// ---------------------------------------------------------------------------

static int defer_order[4];
static int defer_order_idx;

static void
on_defer_order(void *data)
{
    defer_order[defer_order_idx++] = (int)(size_t)data;
}

static void
test_defer_fifo(void)
{
    AxlLoop *loop;

    defer_order_idx = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_defer(loop, on_defer_order, (void *)1);
    axl_defer(loop, on_defer_order, (void *)2);
    axl_defer(loop, on_defer_order, (void *)3);

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(defer_order_idx == 3, "defer: FIFO: all 3 fired");
    test_check(defer_order[0] == 1 && defer_order[1] == 2 &&
               defer_order[2] == 3, "defer: FIFO: correct order 1,2,3");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 16: Deferred work from inside a callback (re-entrant enqueue)
// ---------------------------------------------------------------------------

static size_t defer_reentrant_count;

static void
on_defer_reentrant(void *data)
{
    defer_reentrant_count++;
    if (defer_reentrant_count == 1) {
        /* Schedule more work from inside deferred callback */
        axl_defer((AxlLoop *)data, on_defer_reentrant, data);
    }
}

static void
test_defer_reentrant(void)
{
    AxlLoop *loop;

    defer_reentrant_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_defer(loop, on_defer_reentrant, loop);

    /* Give it two loop ticks to drain both the original and re-enqueued work */
    axl_loop_add_timeout(loop, 200, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(defer_reentrant_count == 2,
               "defer: re-entrant: 2 callbacks fired");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 16b: a defer queued from a defer runs on the NEXT iteration
//
// axl-defer.h promises "the next loop tick". The drain runs at the start of
// an iteration, so work a deferred callback queues is left in the ring when
// the iteration goes on to wait -- and a blocking wait used to sleep until
// the 10 ms poll timer (POLL_INTERVAL_MS) or some unrelated event. A chain
// that re-queues itself measures it: in 50 ms it ran about five times (one
// per poll tick); with no wait while work is queued it runs many hundred.
// The bound is a count against a fixed window, not a wall-clock threshold.
// ---------------------------------------------------------------------------

static size_t defer_chain_count;
static size_t defer_chain_ticks;

/* A 1 ms timer fires at most once per poll tick (the firmware's timer tick
   is coarser), so it counts the TICKS the window held. */
static bool
on_defer_chain_tick(void *data)
{
    (void)data;
    defer_chain_ticks++;
    return AXL_SOURCE_CONTINUE;
}

static void
on_defer_chain(void *data)
{
    defer_chain_count++;
    if (axl_defer((AxlLoop *)data, on_defer_chain, data) == 0) {
        axl_loop_quit((AxlLoop *)data);
    }
}

static void
test_defer_chain_next_tick(void)
{
    AxlLoop *loop;

    defer_chain_count = 0;
    defer_chain_ticks = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        test_check(false, "defer: chain: loop created");
        return;
    }

    test_check(axl_defer(loop, on_defer_chain, loop) != 0
                   && axl_loop_add_timeout(loop, 50, on_defer_check_timer, loop) != 0
                   && axl_loop_add_timer(loop, 1, on_defer_chain_tick, NULL) != 0,
               "defer: chain: the first link and a 50 ms window are scheduled");
    axl_loop_run(loop);

    /* Judged PER POLL TICK, not per wall-clock window: a loop that waits for
       the tick between links manages ~1 a tick; one that does not, hundreds.
       A starved vCPU (a pooled guest beside the gates) fits fewer of both
       into 50 ms of wall time, so a fixed count failed on load alone. */
    size_t ticks = defer_chain_ticks > 0 ? defer_chain_ticks : 1;
    test_check(defer_chain_count >= 20 * ticks,
               "defer: a defer queued from a defer runs on the next iteration, "
               "not the next poll tick (>= 20 links per tick)");
    if (defer_chain_count < 20 * ticks) {
        axl_printf("    %u links over %u ticks\n", (unsigned)defer_chain_count, (unsigned)ticks);
    }
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 17: axl_defer returns 0 when fn is NULL
// ---------------------------------------------------------------------------

static void
test_defer_null_fn(void)
{
    AxlLoop *loop;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    test_check(axl_defer(loop, NULL, NULL) == 0, "defer: NULL fn returns 0");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 18: Pubsub basic publish + receive
// ---------------------------------------------------------------------------

static int sig_value;

static void
on_pubsub_received(void *event_data, void *user_data)
{
    (void)user_data;
    sig_value = *(int *)event_data;
}

static void
test_pubsub_basic(void)
{
    AxlLoop *loop;
    int payload = 42;

    sig_value = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    test_check(axl_pubsub_register(loop, "test-basic"), "pubsub:new returns true");
    test_check(axl_pubsub_subscribe(loop, "test-basic", on_pubsub_received, NULL) != 0,
               "pubsub:connect returns handle");

    axl_pubsub_publish(loop, "test-basic", &payload);

    /* Run loop so defer drains */
    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_value == 42, "pubsub:callback received value 42");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 19: Multiple subscribers on one topic
// ---------------------------------------------------------------------------

static int sig_count_a;
static int sig_count_b;

static void
on_pubsub_a(void *event_data, void *user_data)
{
    (void)event_data; (void)user_data;
    sig_count_a++;
}

static void
on_pubsub_b(void *event_data, void *user_data)
{
    (void)event_data; (void)user_data;
    sig_count_b++;
}

static void
test_pubsub_multi_sub(void)
{
    AxlLoop *loop;

    sig_count_a = 0;
    sig_count_b = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    axl_pubsub_subscribe(loop, "multi-sub", on_pubsub_a, NULL);
    axl_pubsub_subscribe(loop, "multi-sub", on_pubsub_b, NULL);

    axl_pubsub_publish(loop, "multi-sub", NULL);

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_count_a == 1, "pubsub:multi-sub: A fired once");
    test_check(sig_count_b == 1, "pubsub:multi-sub: B fired once");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 20: Disconnect removes subscriber
// ---------------------------------------------------------------------------

static int sig_disconnect_count;

static void
on_pubsub_unsub(void *event_data, void *user_data)
{
    (void)event_data; (void)user_data;
    sig_disconnect_count++;
}

static void
test_pubsub_disconnect(void)
{
    AxlLoop *loop;
    uint32_t handle;

    sig_disconnect_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    handle = axl_pubsub_subscribe(loop, "disc-test", on_pubsub_unsub, NULL);

    test_check(axl_pubsub_unsubscribe(loop, handle),
               "pubsub:disconnect returns true");
    test_check(!axl_pubsub_unsubscribe(loop, handle),
               "pubsub:double disconnect returns false");
    test_check(!axl_pubsub_unsubscribe(loop, 999),
               "pubsub:disconnect invalid returns false");

    axl_pubsub_publish(loop, "disc-test", NULL);

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_disconnect_count == 0,
               "pubsub:disconnected handler did not fire");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 21: Publish on unknown topic returns false
// ---------------------------------------------------------------------------

static void
test_pubsub_emit_unknown(void)
{
    AxlLoop *loop;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    test_check(!axl_pubsub_publish(loop, "no-such-topic", NULL),
               "pubsub: publish unknown returns false");
    test_check(!axl_pubsub_publish(loop, NULL, NULL),
               "pubsub: publish NULL name returns false");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 22: Auto-create on connect (no axl_pubsub_register needed)
// ---------------------------------------------------------------------------

static int sig_auto_count;

static void
on_pubsub_auto(void *event_data, void *user_data)
{
    (void)event_data; (void)user_data;
    sig_auto_count++;
}

static void
test_pubsub_auto_create(void)
{
    AxlLoop *loop;

    sig_auto_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);

    /* Connect without prior axl_pubsub_register — should auto-create */
    axl_pubsub_subscribe(loop, "auto-created", on_pubsub_auto, NULL);
    axl_pubsub_publish(loop, "auto-created", NULL);

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_auto_count == 1, "pubsub:auto-create on connect works");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 23: User data passed through correctly
// ---------------------------------------------------------------------------

static void *sig_user_data_received;

static void
on_pubsub_user_data(void *event_data, void *user_data)
{
    (void)event_data;
    sig_user_data_received = user_data;
}

static void
test_pubsub_user_data(void)
{
    AxlLoop *loop;
    int sentinel = 99;

    sig_user_data_received = NULL;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    axl_pubsub_subscribe(loop, "userdata-test", on_pubsub_user_data, &sentinel);
    axl_pubsub_publish(loop, "userdata-test", NULL);

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_user_data_received == &sentinel,
               "pubsub:user_data passed correctly");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 24: axl_pubsub_reset frees subscribers and clears table
// ---------------------------------------------------------------------------

static int sig_reset_count;

static void
on_pubsub_reset(void *event_data, void *user_data)
{
    (void)event_data; (void)user_data;
    sig_reset_count++;
}

static void
test_pubsub_reset(void)
{
    AxlLoop *loop;

    sig_reset_count = 0;
    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    axl_pubsub_reset(loop);
    axl_pubsub_subscribe(loop, "reset-test", on_pubsub_reset, NULL);

    /* Reset should wipe the topic and its subscriber */
    axl_pubsub_reset(loop);

    /* Publish after reset — topic no longer exists */
    test_check(!axl_pubsub_publish(loop, "reset-test", NULL),
               "pubsub:emit after reset returns false");

    axl_loop_add_timeout(loop, 100, on_defer_check_timer, loop);
    axl_loop_run(loop);

    test_check(sig_reset_count == 0, "pubsub:reset prevented callback");
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Test 25: iterate_until -- done event fires, outer loop stays alive
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop  *outer;
    AxlEvent *inner;
    int       outer_ticks;
    int       side_ticks;
    int       iter_rc;
    bool      iter_ran;
} IterUntilCtx;

static bool
iter_side_source_tick(void *data)
{
    IterUntilCtx *ctx = data;
    ctx->side_ticks++;
    return AXL_SOURCE_CONTINUE;
}

static bool
iter_inner_producer(void *data)
{
    AxlEvent *e = data;
    axl_event_signal(e);
    return AXL_SOURCE_REMOVE;
}

static bool
iter_outer_tick_done_path(void *data)
{
    IterUntilCtx *ctx = data;
    ctx->outer_ticks++;

    if (!ctx->iter_ran) {
        /* Arm a producer that fires ~50ms later on the same outer loop.
         * With iterate_until driving the outer loop, the producer DOES
         * fire; without it, the outer would be starved inside the
         * callback. Also arm a repeating 30ms "side" source to verify
         * unrelated outer sources keep ticking. */
        axl_loop_add_timeout(ctx->outer, 50, iter_inner_producer, ctx->inner);
        AxlSourceId side_id = axl_loop_add_timer(ctx->outer, 30,
                                              iter_side_source_tick, ctx);

        ctx->iter_rc = axl_loop_iterate_until(ctx->outer, ctx->inner,
                                              500 * 1000);
        axl_event_reset(ctx->inner);
        axl_loop_remove_source(ctx->outer, side_id);
        ctx->iter_ran = true;
    }

    if (ctx->outer_ticks >= 2) {
        axl_loop_quit(ctx->outer);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_iterate_until_done(void)
{
    IterUntilCtx ctx = {0};

    ctx.outer = axl_loop_new();
    ctx.inner = axl_event_new();
    if (ctx.outer == NULL || ctx.inner == NULL) {
        test_fail("loop: iterate_until: alloc failed");
        return;
    }

    axl_loop_add_timer(ctx.outer, 100, iter_outer_tick_done_path, &ctx);
    axl_loop_run(ctx.outer);

    test_check(ctx.iter_rc == AXL_OK, "loop: iterate_until returns AXL_OK on done");
    test_check(ctx.side_ticks > 0,
               "loop: iterate_until lets outer sources keep firing");

    axl_event_free(ctx.inner);
    axl_loop_free(ctx.outer);
}

// ---------------------------------------------------------------------------
// Test 26: iterate_until -- timeout path
// ---------------------------------------------------------------------------

static bool
iter_outer_tick_timeout_path(void *data)
{
    IterUntilCtx *ctx = data;
    ctx->outer_ticks++;

    if (!ctx->iter_ran) {
        /* No producer armed. With a 50ms timeout and no done-event
         * signal, iterate_until must return -1. */
        ctx->iter_rc = axl_loop_iterate_until(ctx->outer, ctx->inner,
                                              50 * 1000);
        ctx->iter_ran = true;
    }

    axl_loop_quit(ctx->outer);
    return AXL_SOURCE_REMOVE;
}

static void
test_iterate_until_timeout(void)
{
    IterUntilCtx ctx = {0};

    ctx.outer = axl_loop_new();
    ctx.inner = axl_event_new();
    if (ctx.outer == NULL || ctx.inner == NULL) {
        test_fail("loop: iterate_until timeout: alloc failed");
        return;
    }

    axl_loop_add_timer(ctx.outer, 100, iter_outer_tick_timeout_path, &ctx);
    axl_loop_run(ctx.outer);

    test_check(ctx.iter_rc == AXL_TIMEOUT, "loop: iterate_until returns AXL_TIMEOUT on timeout");

    axl_event_free(ctx.inner);
    axl_loop_free(ctx.outer);
}

// ---------------------------------------------------------------------------
// Driver-mode attach (axl_loop_attach_driver / axl_loop_detach_driver)
//
// Closing the loop with axl_loop_run inside this test would mask
// the new path: axl_loop_run's own poll_timer drives idle anyway,
// so a counter incremented from idle proves nothing about the
// driver-mode timer. Instead we do NOT call axl_loop_run — the
// only thing that can fire idle is the firmware-managed
// EVT_TIMER+EVT_NOTIFY_SIGNAL preempting at TPL_CALLBACK during
// gBS->Stall (or gBS->WaitForEvent at any TPL <= TPL_APPLICATION).
// We stall, watch the counter advance, then detach and assert no
// further ticks fire.
// ---------------------------------------------------------------------------

typedef struct {
    int  tick_count;
} DriverModeCtx;

/* Idle callback fires from inside whatever axl_loop_dispatch path
   walks idle sources. With the driver-mode timer attached and
   axl_loop_run NOT called, the only path firing idle is the
   driver-mode timer's notify trampoline. Each notify increments
   tick_count by exactly 1. */
static bool
driver_mode_idle_increment(void *user)
{
    DriverModeCtx *ctx = user;
    ctx->tick_count++;
    return AXL_SOURCE_CONTINUE;
}

static void
test_loop_attach_driver(void)
{
    /* 1. Argument validation */
    test_check(axl_loop_attach_driver(NULL, 50) != AXL_OK,
               "loop: attach_driver rejects NULL loop");
    test_check(axl_loop_detach_driver(NULL) != AXL_OK,
               "loop: detach_driver rejects NULL loop");

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("loop: attach_driver: loop_new alloc failed");
        return;
    }

    test_check(axl_loop_attach_driver(loop, 0) != AXL_OK,
               "loop: attach_driver rejects zero interval");

    /* 2. Detach when not attached returns ERR */
    test_check(axl_loop_detach_driver(loop) != AXL_OK,
               "loop: detach_driver on un-attached loop returns ERR");

    /* 3. First attach succeeds */
    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "loop: attach_driver succeeds first time");

    /* 4. Re-attach fails (must detach first) */
    test_check(axl_loop_attach_driver(loop, 20) != AXL_OK,
               "loop: attach_driver refuses to re-attach without detach");

    /* 5. The notify actually drives axl_loop_dispatch — verify by
       seeing the idle counter advance from a stall (no foreground
       axl_loop_run masking the new path). 20 ms tick × 200 ms stall
       should net 5-10 ticks; assert >= 2 to leave slack for QEMU
       jitter. */
    DriverModeCtx ctx = { .tick_count = 0 };
    axl_loop_add_idle(loop, driver_mode_idle_increment, &ctx);

    gBS->Stall(200000);  /* 200 ms */
    test_check(ctx.tick_count >= 2,
               "loop: driver-mode notify fires axl_loop_dispatch (no foreground caller)");
    int ticks_during_attach = ctx.tick_count;

    /* 6. Detach succeeds + cleans up */
    test_check(axl_loop_detach_driver(loop) == AXL_OK,
               "loop: detach_driver succeeds");

    /* 7. After detach the timer no longer fires — counter stays
       flat across another stall. */
    gBS->Stall(200000);  /* 200 ms */
    test_check(ctx.tick_count == ticks_during_attach,
               "loop: detach_driver actually stops the timer (no further dispatch)");

    /* 8. Detach is not idempotent — second call returns ERR */
    test_check(axl_loop_detach_driver(loop) != AXL_OK,
               "loop: second detach_driver returns ERR");

    /* 9. Re-attach after detach succeeds + counter advances again */
    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "loop: attach_driver works again after detach");
    int ticks_before_reattach_stall = ctx.tick_count;
    gBS->Stall(200000);  /* 200 ms */
    test_check(ctx.tick_count > ticks_before_reattach_stall,
               "loop: re-attached timer drives dispatch again");
    test_check(axl_loop_detach_driver(loop) == AXL_OK,
               "loop: detach_driver after re-attach succeeds");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Driver-tick budget and busy idle sources
//
// A plain idle source must see NO change: one call per driver tick. A BUSY
// idle source (axl_loop_add_idle_busy) keeps the tick iterating for up to
// the loop's budget, so it is called many times per tick -- unless the
// budget is 0. Counted over the same stall window as test_loop_attach_driver
// (200 ms at a 20 ms tick: about 10 ticks), with the driver timer the only
// thing that can call either source.
// ---------------------------------------------------------------------------

/* Ticks in a 200 ms stall at a 20 ms period, with generous slack for QEMU
   timer jitter: a plain source is called once per tick, so more than this
   means the tick iterated. */
#define BUDGET_PLAIN_CALLS_MAX   25
/* A 5 ms budget spins thousands of passes per tick; one pass per tick for
   ~10 ticks cannot reach this. */
#define BUDGET_BUSY_CALLS_MIN    200
/* A 1 ms tick clamps the budget to 0: at most one pass per tick, and a
   1 ms timer cannot fire more than ~200 times in 200 ms. An unclamped 5 ms
   budget spins thousands of passes. */
#define BUDGET_CLAMPED_CALLS_MAX 250
/* Thousands of passes fit in 3 ms when the tick is extended; one pass (the
   tick then ends) when it is not. */
#define BUDGET_MIDTICK_CALLS_MIN 50

static bool
budget_count_idle(void *user)
{
    int *count = user;

    (*count)++;
    return AXL_SOURCE_CONTINUE;
}

static void
test_loop_driver_budget(void)
{
    AxlLoop *loop = axl_loop_new();

    if (loop == NULL) {
        test_fail("loop: driver budget: loop_new alloc failed");
        return;
    }

    /* 1. Validation -- exact return values, nothing changed on refusal. */
    test_check(axl_loop_set_driver_budget_ms(NULL, 5) == AXL_INVALID,
               "loop: set_driver_budget_ms(NULL) is AXL_INVALID");
    test_check(axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS + 1u)
                   == AXL_INVALID,
               "loop: set_driver_budget_ms(MAX + 1) is AXL_INVALID");
    test_check(axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS)
                   == AXL_OK,
               "loop: set_driver_budget_ms(MAX) is AXL_OK");
    test_check(axl_loop_set_driver_budget_ms(loop, 0) == AXL_OK,
               "loop: set_driver_budget_ms(0) is AXL_OK");
    test_check(axl_loop_add_idle_busy(NULL, budget_count_idle, NULL) == 0,
               "loop: add_idle_busy(NULL loop) returns 0");
    test_check(axl_loop_add_idle_busy(loop, NULL, NULL) == 0,
               "loop: add_idle_busy(NULL cb) returns 0");
    test_check(axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS)
                   == AXL_OK,
               "loop: set_driver_budget_ms(DEFAULT) is AXL_OK");

    /* 2. A plain idle source is unchanged: about one call per tick. */
    int plain = 0;
    AxlSourceId plain_id = axl_loop_add_idle(loop, budget_count_idle, &plain);

    test_check(plain_id != 0, "loop: driver budget: plain idle added");
    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "loop: driver budget: attach at 20 ms");
    gBS->Stall(200000);
    axl_loop_detach_driver(loop);
    axl_printf("  driver budget: plain idle alone: %d call(s) in 200 ms\n", plain);
    test_check(plain >= 2 && plain <= BUDGET_PLAIN_CALLS_MAX,
               "loop: a plain idle source is called about once per driver tick (opt-in)");
    axl_loop_remove_source(loop, plain_id);

    /* 3. A busy idle source extends every tick up to the budget. */
    int busy = 0;
    AxlSourceId busy_id = axl_loop_add_idle_busy(loop, budget_count_idle, &busy);

    test_check(busy_id != 0, "loop: driver budget: busy idle added");
    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "loop: driver budget: re-attach at 20 ms");
    gBS->Stall(200000);
    axl_loop_detach_driver(loop);
    axl_printf("  driver budget: busy idle, 5 ms budget: %d call(s) in 200 ms\n", busy);
    test_check(busy >= BUDGET_BUSY_CALLS_MIN,
               "loop: a busy idle source keeps a driver tick iterating");

    /* 4. Budget 0: the busy source is back to one call per tick. */
    busy = 0;
    test_check(axl_loop_set_driver_budget_ms(loop, 0) == AXL_OK,
               "loop: driver budget: set 0");
    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "loop: driver budget: re-attach with budget 0");
    gBS->Stall(200000);
    axl_loop_detach_driver(loop);
    axl_printf("  driver budget: busy idle, budget 0: %d call(s) in 200 ms\n", busy);
    test_check(busy >= 2 && busy <= BUDGET_PLAIN_CALLS_MAX,
               "loop: budget 0 means one pass per tick, busy or not");

    axl_loop_remove_source(loop, busy_id);

    /* 5. The clamp: a 1 ms tick halves to a 0 ms budget, so a busy source is
       back to one pass per tick even with the default 5 ms budget. Without
       the clamp every 1 ms tick would spin its whole 5 ms. */
    busy = 0;
    test_check(axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS)
                   == AXL_OK,
               "loop: driver budget: back to the default for the clamp case");
    busy_id = axl_loop_add_idle_busy(loop, budget_count_idle, &busy);
    test_check(axl_loop_attach_driver(loop, 1) == AXL_OK,
               "loop: driver budget: attach at 1 ms");
    gBS->Stall(200000);
    axl_loop_detach_driver(loop);
    axl_printf("  driver budget: busy idle, 1 ms tick (clamped to 0): %d call(s) in 200 ms\n",
               busy);
    test_check(busy >= 2 && busy <= BUDGET_CLAMPED_CALLS_MAX,
               "loop: the budget is clamped to half the tick (1 ms tick: one pass per tick)");
    axl_loop_remove_source(loop, busy_id);
    axl_loop_free(loop);
}

/* A busy source added FROM a callback in the middle of a tick -- the way
   AxlTcp adds its pacer from a send issued by a request handler -- must
   extend that same tick, not wait for the next one. Counted over the
   window just after it was added: well inside one tick's budget. */
typedef struct {
    AxlLoop    *loop;
    AxlSourceId busy_id;
    uint64_t    added_us;
    int         early_calls;
} BudgetMidTickCtx;

/* Calls within this long of the add fall inside the tick that added it:
   the default budget is 5 ms from the tick's start, and the add happens in
   that tick's first pass. */
#define BUDGET_MIDTICK_WINDOW_US  3000u

static bool
budget_midtick_busy(void *user)
{
    BudgetMidTickCtx *ctx = user;
    uint64_t          now = axl_time_get_us();

    if (now - ctx->added_us < BUDGET_MIDTICK_WINDOW_US) {
        ctx->early_calls++;
        return AXL_SOURCE_CONTINUE;
    }
    return AXL_SOURCE_REMOVE;   /* window over: stop spending ticks */
}

static bool
budget_midtick_add(void *user)
{
    BudgetMidTickCtx *ctx = user;

    ctx->added_us = axl_time_get_us();
    ctx->busy_id  = axl_loop_add_idle_busy(ctx->loop, budget_midtick_busy, ctx);
    return AXL_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Deferred work in a driver tick
//
// A pass that dispatches no source but leaves deferred work queued is not
// the end of the tick: what was deferred is due on the NEXT pass
// (axl-defer.h's "next tick" of the loop), as in axl_loop_run, which never
// blocks while the queue holds work. Ending the tick there held each hop of
// a defer chain for a whole driver period -- sshd's channel flush defers
// its transport flush, so a request's reply waited a tick for it.
// ---------------------------------------------------------------------------

/* Hops in the chain: each deferred callback defers the next. */
#define DEFER_CHAIN_HOPS    3
/* Every hop must run within this of the first: far below one 20 ms tick. */
#define DEFER_CHAIN_MAX_US  5000u

typedef struct {
    AxlLoop  *loop;
    int       ran;
    uint64_t  at_us[DEFER_CHAIN_HOPS];
} DeferChainCtx;

static void
defer_chain_hop(void *data)
{
    DeferChainCtx *ctx = data;

    if (ctx->ran < DEFER_CHAIN_HOPS) {
        ctx->at_us[ctx->ran++] = axl_time_get_us();
    }
    if (ctx->ran < DEFER_CHAIN_HOPS && axl_defer(ctx->loop, defer_chain_hop, ctx) == 0) {
        ctx->ran = -100;   /* the queue refused: fails the count check */
    }
}

/* A deferred callback that defers itself again, each run DEFER_SLOW_US
   long: the tick runs it only within its budget, like a busy idle source
   -- not to the dispatch cap, which held a tick for 2 x AXL_MAX_SOURCES
   runs (and its overrun cost the idle extension its ticks). */
#define DEFER_SLOW_US       1000u
/* The longest run of back-to-back calls: the 5 ms busy budget plus one
   call (5.05 ms measured), within the renderer's 7 ms per-tick bound --
   below the 10 ms a half-interval clamp would give, and far below the
   dispatch cap's 128 ms. */
#define DEFER_SLOW_MAX_US   7000u
/* The chain stops itself after this many runs, so a tick that ignores its
   budget fails this check by name instead of starving the foreground: with
   no bound, the ticks ran back to back and test-axl.sh timed out. */
#define DEFER_SLOW_RUNS     40u

typedef struct {
    AxlLoop  *loop;
    bool      stop;
    uint64_t  run_start_us;
    uint64_t  last_us;
    uint64_t  longest_us;
    uint32_t  calls;
} DeferSlowCtx;

static void
defer_slow_hop(void *data)
{
    DeferSlowCtx *ctx = data;
    uint64_t      now = axl_time_get_us();

    if (ctx->calls == 0 || now - ctx->last_us > DEFER_SLOW_US * 2u) {
        ctx->run_start_us = now;
    }
    gBS->Stall(DEFER_SLOW_US);
    ctx->last_us = axl_time_get_us();
    if (ctx->last_us - ctx->run_start_us > ctx->longest_us) {
        ctx->longest_us = ctx->last_us - ctx->run_start_us;
    }
    ctx->calls++;
    if (!ctx->stop && ctx->calls < DEFER_SLOW_RUNS
        && axl_defer(ctx->loop, defer_slow_hop, ctx) == 0) {
        ctx->stop = true;
    }
}

// ---------------------------------------------------------------------------
// Driver-tick budget against a waiting foreground
//
// A tick that finds the foreground waiting in gBS->WaitForEvent (what the
// Shell does at its prompt) may extend to its idle budget; every other tick
// keeps the busy-foreground budget and its half-period clamp. Measured as
// the MEDIAN span of a tick -- first to last call of a busy idle source,
// ticks told apart by a gap longer than any gap inside one -- so a few
// preempted ticks cannot move it, and it is the bound itself rather than a
// consequence of it. A foreground that alternates work with short waits is
// measured by the work it gets done: it must keep its share.
// ---------------------------------------------------------------------------

#define SPAN_MAX            64u
/* Ticks measured per case: enough for a median that ignores a few
   preempted ones. */
#define SPAN_MIN_TICKS      5u
/* Spans in us. Busy foreground: 5 ms (the default budget). Waiting
   foreground: 18 ms of a 20 ms tick (9/10); 25 ms of a 50 ms tick
   (AXL_LOOP_DRIVER_BUDGET_MAX_MS). A 10 ms idle budget: 10 ms. No
   extension: one pass, about 0. */
#define SPAN_BUSY_MIN       3000u
#define SPAN_BUSY_MAX       10000u
#define SPAN_IDLE20_MIN     14000u
#define SPAN_IDLE20_MAX     19500u
#define SPAN_IDLE50_MIN     20000u
#define SPAN_IDLE50_MAX     30000u
#define SPAN_IDLE10_MIN     8000u
#define SPAN_IDLE10_MAX     12500u
#define SPAN_NONE_MAX       1000u
/* At least this share of a waiting foreground's ELIGIBLE ticks (all but
   the first SPAN_INELIGIBLE after attach) must extend (reach
   SPAN_IDLE20_MIN at 20 ms, SPAN_IDLE50_MIN at 50 ms). The upper quartile
   alone passes while only a quarter of them do -- an idle extension
   three-quarters dead. Measured on a quiet host: 100% at both periods
   (x64 and aarch64); a recency threshold cut to 50 us read 58-64% at
   20 ms, and every third period forced to read "ran" 29-35%. */
#define SPAN_IDLE_COVER_PCT 80u
/* Ticks after attach that cannot extend (the third-tick rule). */
#define SPAN_INELIGIBLE     2u
/* A slow busy source: every pass takes SLOW_PASS_US, the shape of a TCP
   pacer on aarch64 TCG. At a 20 ms tick the 18 ms idle budget then starts
   its last pass at 16 ms, and that pass ends at 20 ms -- on the next
   tick's interrupt, which queues that tick behind this one. The tick that
   runs straight after found no fresh halt (the foreground could not run in
   between), so it read as busy and set "ran", and the extension needed
   three more ticks to come back: measured before the fix (x64), 33% of
   the eligible ticks extended and the longest span was 28.3 ms, an
   extended tick and the one queued behind it. An extended tick must END
   before the next one is due (the longest span at most SPAN_IDLE20_MAX)
   and keep extending (SPAN_IDLE_COVER_PCT of the eligible ticks). */
#define SLOW_PASS_US        4000u
/* A late long call: one call of the busy source per tick, LATE_AFTER_US
   into it, takes LATE_US -- the shape of a signalled callback (an SFTP
   read, a packet's encryption) that no earlier pass predicts. The tick
   runs into the next one's interrupt whatever it predicts, and the next
   tick is queued behind it and runs straight after. That tick is no
   evidence either way -- the foreground never had the CPU -- so the
   extension must resume on the tick after it (SPAN_IDLE_COVER_PCT of the
   eligible ticks: an extended tick and the one queued behind it measure
   as one span). Measured before the fix (x64): 30-33%. The queued tick
   itself must not extend -- extended ticks would chain, each running into
   the next one's interrupt, and the foreground would never get the CPU --
   so that span is at most LATE_SPAN_MAX_US: the extended tick (about
   22 ms) and a busy-budget one (5 ms plus a pass), well short of two
   extended ticks. */
#define LATE_AFTER_US       16000u
#define LATE_US             6000u
#define LATE_SPAN_MAX_US    35000u
/* Frame-paced foreground: FRAME_WORK_MS of computation, then a
   FRAME_WAIT_MS wait, for FRAME_WINDOW_MS. What it keeps is measured as the
   median wall time of one work phase, beside a resident tick against
   alone: the half-period clamp (5 ms of each 20 ms tick) stretches it
   about 1.3x; ticks that took 9/10 of every period stretch it about 10x.
   The wait is excluded -- a tick that lands on it finds the foreground
   waiting and may run the idle budget, by design. */
#define FRAME_WORK_MS       10u
#define FRAME_WAIT_MS       6u
#define FRAME_WINDOW_MS     600u
#define FRAME_MAX           64u
#define FRAME_STRETCH_PCT   250u
/* A renderer: computation, then a short wait, repeated. EDK2 fires every
   timer on its 10 ms system tick, so the renderer wakes on the same
   interrupts that queue driver ticks, by construction rather than by
   chance; the phase is pinned by the firmware. Its frame period (mean,
   wake to wake) with the default idle budget must stay within
   RENDER_SLOW_PCT of the same loop with the idle extension off. Measured
   before the fix, a tick that ran the idle budget on the renderer's wake
   doubled it: 20.0 ms against 10.0 at a 10 ms tick with 4 ms of work and a
   RENDER_WAIT_MS wait (sshd's tick), 40.0 against 20.0 at a 20 ms tick
   with 10 ms. Counting only ticks that found it waiting twice running
   still gave 15.0 and 30.0. RENDER_LONG_WAIT_MS spans one whole driver
   period, so a period passes with the renderer never running: the wake
   after it must not extend either. (A renderer on a PERIODIC frame timer
   settles out of the wake phase on its own and showed no loss.) */
#define RENDER_WAIT_MS      6u
/* No tick of a renderer run may span more than this: a busy-budget tick is
   5 ms plus one pass, an extended one 9 ms (10 ms tick) or 18 ms (20 ms).
   This is R1 measured directly -- the frame period is its consequence. */
#define RENDER_TICK_MAX_US  7000u
/* The gap that separates two renderer ticks' spans. Above PROBE_GAP_US, so
   a preemption the probes let pass (under 1 ms) cannot split an extended
   tick into two short spans and hide it from RENDER_TICK_MAX_US; renderer
   ticks are at least a work phase (about 4 ms) apart. The tick/20 used
   elsewhere is 0.5 ms at a 10 ms tick. */
#define RENDER_SEP_US       2000u
#define RENDER_LONG_WAIT_MS 12u
#define RENDER_WINDOW_MS    300u
/* Each renderer figure is the least of RENDER_REPS runs, the default
   and the control alternated: host preemption only ever LENGTHENS a
   frame, and on a loaded box (verify.sh runs both arches at once) it
   hit one run and not the other often enough to fail a single pair. */
#define RENDER_REPS         2u
/* Whole attempts at the timing cases (see
   test_loop_driver_budget_idle_foreground), and how many of them must be
   JUDGED (their probes clean) and miss a bound before the miss counts. */
#define IDLE_ATTEMPTS       3u
#define IDLE_CONFIRM        2u
/* The host-noise probe before and after each attempt: spin PROBE_SPIN_MS
   reading the clock; a gap of more than PROBE_GAP_US between two reads is
   the vCPU descheduled. More than PROBE_VOID_GAPS such gaps in either
   probe voids the attempt. Measured: on a quiet host every probe read 0
   gaps, worst 49-221 us (x64 and aarch64, 10 probes); under load 15 on 12
   cores 15 of 21 probes read 1-43 gaps, worst 1.2-6.3 ms -- and an attempt
   that ran under a clean probe still failed now and then, the load having
   risen inside it, which is why the probe is taken on both sides. */
#define PROBE_SPIN_MS       200u
#define PROBE_GAP_US        1000u
#define PROBE_VOID_GAPS     0u
/* ... and a short probe right before and right after EVERY measurement
   inside an attempt (each tick-span case, each renderer run, the
   calibration and the frame-paced runs): any gap in any of them voids the
   attempt too. The attempt's own probes are 8-10 s apart, and load that
   rose and fell between them let host noise through as a FAIL. Measured
   on aarch64 at host load 11-24 (lint.sh in a loop, with and without four
   busy loops): before, 2 of 18 runs were judged and failed timing bounds;
   with these probes 18 of 18 were void, 11 of the 24 attempts that both
   of the attempt's own probes passed voided only by these. */
#define SPAN_PROBE_MS       20u
/* A spin that reads the clock this often without reaching PROBE_SPIN_MS
   is a clock that does not advance: void, never a hang. Far above the
   reads a 200 ms spin makes on either arch. */
#define PROBE_READS_MAX     200000000u
/* ... and this many CONSECUTIVE reads of one unchanged value is a clock
   stuck at a non-zero value: void within milliseconds, not after the total
   bound (which could take most of test-axl.sh's boot timeout on TCG). */
#define PROBE_SAME_MAX      1000000u
/* Spacings between successive gaps printed with a void probe: periodic at
   one tick interval is a driver tick still running (a stuck detach), random
   is host noise. */
#define PROBE_SPACING_MAX   6u
/* Timing assertions in an attempt: what a void-everywhere run declares
   skipped (test_skip_n). Keep equal to the checks after the attempts. */
#define IDLE_TIMING_CHECKS  30u
#define RENDER_SLOW_PCT     125u

typedef enum {
    FG_BUSY,    ///< gBS->Stall: never waits
    FG_WAIT,    ///< one gBS->WaitForEvent for the whole window
    FG_FRAMES,  ///< computation, then a short wait, repeated
    FG_RENDER,  ///< computation, then a short wait; frame period measured
} FgMode;

/* How long a call of the measuring busy source takes: every call
   pass_us, and the first call of a tick at least late_after_us into it
   late_us more (a signalled callback no pass before it could foresee). */
typedef struct {
    uint32_t pass_us;
    uint32_t late_after_us;
    uint32_t late_us;
} SpanLoad;

typedef struct {
    uint64_t sep_us;            ///< a gap longer than this starts a new tick
    SpanLoad load;
    bool     late_done;         ///< this tick's late call has run
    uint64_t first_us;
    uint64_t last_us;
    uint32_t spans[SPAN_MAX];
    uint32_t n;
} SpanCtx;

static bool
span_busy(void *user)
{
    SpanCtx *ctx = user;
    uint64_t now = axl_time_get_us();

    if (ctx->first_us == 0) {
        ctx->first_us = now;
    } else if (now - ctx->last_us > ctx->sep_us) {
        if (ctx->n < SPAN_MAX) {
            ctx->spans[ctx->n++] = (uint32_t)(ctx->last_us - ctx->first_us);
        }
        ctx->first_us  = now;
        ctx->late_done = false;
    }
    uint32_t spin = ctx->load.pass_us;
    if (ctx->load.late_us != 0 && !ctx->late_done
        && now - ctx->first_us >= ctx->load.late_after_us) {
        ctx->late_done = true;
        spin += ctx->load.late_us;
    }
    if (spin != 0) {
        gBS->Stall(spin);
        now = axl_time_get_us();
    }
    ctx->last_us = now;
    return AXL_SOURCE_CONTINUE;
}

static uint32_t
span_median(SpanCtx *ctx)
{
    /* Insertion sort: at most SPAN_MAX entries. */
    for (uint32_t i = 1; i < ctx->n; i++) {
        uint32_t v = ctx->spans[i];
        uint32_t j = i;
        while (j > 0 && ctx->spans[j - 1] > v) {
            ctx->spans[j] = ctx->spans[j - 1];
            j--;
        }
        ctx->spans[j] = v;
    }
    return ctx->n == 0 ? 0u : ctx->spans[ctx->n / 2];
}

/* gBS->WaitForEvent on a lone timer for @p ms: the foreground is idle for
   all of it. False when the firmware refused the timer. */
static bool
fg_wait_ms(uint32_t ms)
{
    EFI_EVENT ev  = NULL;
    UINTN     idx = 0;
    bool      ok  = false;

    if (!EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &ev))) {
        ok = !EFI_ERROR(gBS->SetTimer(ev, TimerRelative, (UINT64)ms * 10000u))
             && !EFI_ERROR(gBS->WaitForEvent(1, &ev, &idx));
        gBS->CloseEvent(ev);
    }
    return ok;
}

/* Computation: @p iters loop iterations of CPU time, however long the wall
   clock takes while a tick holds the CPU. */
static volatile uint32_t g_frame_sink;

static void
fg_work(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        g_frame_sink = g_frame_sink + (uint32_t)i;
    }
}

/* Iterations of fg_work() that take FRAME_WORK_MS with nothing attached. */
static uint64_t
fg_calibrate(void)
{
    uint64_t iters = 100000u;

    for (;;) {
        uint64_t t0 = axl_time_get_us();
        fg_work(iters);
        uint64_t us = axl_time_get_us() - t0;
        if (us >= 20000u || iters >= (1ull << 40)) {
            return us == 0 ? iters : iters * FRAME_WORK_MS * 1000u / us;
        }
        iters *= 4u;
    }
}

/* Frames for FRAME_WINDOW_MS; the median wall time of a work phase, in
   us. */
static uint32_t
fg_frames(uint64_t iters, bool *ok)
{
    SpanCtx  work = { 0 };
    uint64_t t0   = axl_time_get_us();

    while (axl_time_get_us() - t0 < FRAME_WINDOW_MS * 1000u && work.n < FRAME_MAX) {
        uint64_t w0 = axl_time_get_us();
        fg_work(iters);
        work.spans[work.n++] = (uint32_t)(axl_time_get_us() - w0);
        if (!fg_wait_ms(FRAME_WAIT_MS)) {
            *ok = false;
        }
    }
    return span_median(&work);
}

/* RENDER_WINDOW_MS of frames: @p iters of computation, then a @p wait_ms
   wait. The mean frame period, first wake to last, in us; *work_us the
   median wall time of a work phase. */
static uint32_t
fg_render(uint64_t iters, uint32_t wait_ms, uint32_t *work_us, bool *ok)
{
    SpanCtx  work   = { 0 };
    uint64_t first  = 0;
    uint64_t last   = 0;
    uint32_t frames = 0;

    while (first == 0 || last - first < RENDER_WINDOW_MS * 1000u) {
        uint64_t w0 = axl_time_get_us();
        fg_work(iters);
        if (work.n < FRAME_MAX) {
            work.spans[work.n++] = (uint32_t)(axl_time_get_us() - w0);
        }
        if (!fg_wait_ms(wait_ms)) {
            *ok = false;
            break;
        }
        last = axl_time_get_us();
        if (first == 0) {
            first = last;
        } else {
            frames++;
        }
    }
    *work_us = span_median(&work);
    return frames == 0 ? 0u : (uint32_t)((last - first) / frames);
}

/* Can this host time sub-millisecond events at all, right now? The
   timing cases need a vCPU that, while it spins, keeps running: a tick
   whose busy idle source is descheduled for more than a millisecond
   splits into two short spans, and a foreground descheduled before its
   next idle pass reads as having run. On an oversubscribed host (load 15
   on 12 cores) both happened on both arches, so even the busy control
   read short. A HALTED vCPU is not the measure -- woken threads are
   scheduled promptly, and a probe of timer wakes read clean under the
   same load. This probe uses NO AxlLoop and no code under test: it spins
   @p spin_ms on the clock (axl_time_get_us) and counts the gaps between
   consecutive reads longer than PROBE_GAP_US -- time the vCPU did not
   run -- and the worst. */
typedef struct {
    bool     clock_ok;                      ///< the clock read and advanced
    uint32_t gaps;                          ///< reads more than PROBE_GAP_US apart
    uint32_t worst_us;
    uint32_t spacing[PROBE_SPACING_MAX];    ///< us between successive gaps
    uint32_t nspacing;
} ProbeResult;

static void
host_probe(ProbeResult *pr, uint32_t spin_ms)
{
    uint64_t t0       = axl_time_get_us();
    uint64_t last     = t0;
    uint64_t last_gap = 0;

    axl_memset(pr, 0, sizeof(*pr));
    if (t0 == 0) {
        return;   /* the clock cannot be read: clock_ok stays false */
    }
    uint32_t same = 0;
    for (uint32_t reads = 0; reads < PROBE_READS_MAX; reads++) {
        uint64_t now = axl_time_get_us();
        uint64_t gap = now - last;
        if (now == 0 || now < last) {
            return;
        }
        same = now == last ? same + 1u : 0u;
        if (same >= PROBE_SAME_MAX) {
            return;
        }
        if (gap > PROBE_GAP_US) {
            if (pr->gaps != 0 && pr->nspacing < PROBE_SPACING_MAX) {
                pr->spacing[pr->nspacing++] = (uint32_t)(now - last_gap);
            }
            pr->gaps++;
            last_gap = now;
        }
        if (gap > pr->worst_us) {
            pr->worst_us = (uint32_t)gap;
        }
        last = now;
        if (now - t0 >= (uint64_t)spin_ms * 1000u) {
            pr->clock_ok = true;
            return;
        }
    }
}

/* A probe that lets an attempt be judged. */
static bool
probe_clean(const ProbeResult *pr)
{
    return pr->clock_ok && pr->gaps <= PROBE_VOID_GAPS;
}

/* A SPAN_PROBE_MS probe, its gaps (or 1 for a clock that cannot be read)
   added to *noise. */
static void
span_noise_probe(uint32_t *noise)
{
    ProbeResult pr;

    host_probe(&pr, SPAN_PROBE_MS);
    *noise += pr.clock_ok ? pr.gaps : 1u;
}

/* What span_measure() saw besides its return value. */
typedef struct {
    uint32_t max_us;    ///< the longest tick span
    uint32_t work_us;   ///< FG_RENDER: the median work phase
} SpanExtra;

/* Attach @p loop at @p tick_ms with a busy idle source and run the
   foreground in @p mode for @p window_ms; each call of the source takes
   as long as @p load says (NULL: returns at once). Returns the median tick span in
   us (FG_WAIT: the upper quartile); *ticks gets the number of whole ticks
   seen, and *long_pct (unless NULL) the share of them, in percent, that
   reached @p long_min_us. FG_FRAMES and FG_RENDER run for their own windows and
   put a result in *frames: the median work phase, or the mean frame
   period, in us. For FG_RENDER, @p window_ms is the renderer's wait
   instead. *extra (unless NULL) gets the longest span and, for FG_RENDER,
   the median work phase. The probes on both sides add their gaps to
   *noise. */
static uint32_t
span_measure(AxlLoop *loop, uint32_t tick_ms, FgMode mode, uint32_t window_ms,
             const SpanLoad *load, uint64_t iters, uint32_t *frames, uint32_t *ticks,
             uint32_t long_min_us, uint32_t *long_pct, SpanExtra *extra, uint32_t *noise,
             bool *ok)
{
    uint32_t work_us = 0;

    SpanCtx     ctx = { .sep_us = mode == FG_RENDER ? RENDER_SEP_US
                                                    : (uint64_t)tick_ms * 1000u / 20u };
    if (load != NULL) {
        ctx.load = *load;
    }
    span_noise_probe(noise);
    AxlSourceId id  = axl_loop_add_idle_busy(loop, span_busy, &ctx);

    *ok = id != 0 && axl_loop_attach_driver(loop, tick_ms) == AXL_OK;
    if (*ok) {
        if (mode == FG_WAIT) {
            *ok = fg_wait_ms(window_ms);
        } else if (mode == FG_BUSY) {
            gBS->Stall(window_ms * 1000u);
        } else if (mode == FG_FRAMES) {
            *frames = fg_frames(iters, ok);
        } else {
            *frames = fg_render(iters, window_ms, &work_us, ok);
        }
        axl_loop_detach_driver(loop);
    }
    if (id != 0) {
        axl_loop_remove_source(loop, id);
    }
    span_noise_probe(noise);
    *ticks = ctx.n;
    /* Coverage counts the ELIGIBLE ticks only, in time order (before the
       median sorts them): the first SPAN_INELIGIBLE after attach can never
       extend -- the tick needs two whole idle periods before it -- so a
       clean run reads 100%. */
    uint32_t longs = 0;
    for (uint32_t i = SPAN_INELIGIBLE; i < ctx.n; i++) {
        longs += ctx.spans[i] >= long_min_us ? 1u : 0u;
    }
    if (long_pct != NULL) {
        *long_pct = ctx.n <= SPAN_INELIGIBLE ? 0u : longs * 100u / (ctx.n - SPAN_INELIGIBLE);
    }
    uint32_t median = span_median(&ctx);
    if (extra != NULL) {
        extra->max_us  = ctx.n == 0 ? 0u : ctx.spans[ctx.n - 1u];
        extra->work_us = work_us;
    }
    /* A waiting foreground's ticks can only be read SHORT by noise: a
       preempted vCPU makes an idle pass late, which counts as the
       foreground having run. So the upper quartile is its figure; a
       busy one's is the median. */
    return mode == FG_WAIT && ctx.n != 0 ? ctx.spans[ctx.n * 3u / 4u] : median;
}

/* The renderer at @p tick_ms with a @p wait_ms wait: its frame period with
   the default idle budget (*on) and with the idle extension off (*off),
   alternated RENDER_REPS times, the least of each; *max_tick the longest
   tick span of any default-budget run. Every run is printed with its
   longest tick, its work phase and the calibrated @p iters, so a slow
   frame can be told apart: an extended tick, a short work phase, or noise.
   Leaves the idle budget at the default. */
static void
render_pair(AxlLoop *loop, uint32_t tick_ms, uint32_t wait_ms, uint64_t iters,
            uint32_t *on, uint32_t *off, uint32_t *max_tick, uint32_t *noise, bool *ok)
{
    uint32_t n = 0;

    *on  = UINT32_MAX;
    *off = UINT32_MAX;
    for (uint32_t r = 0; r < RENDER_REPS; r++) {
        for (uint32_t idle = 0; idle < 2u; idle++) {
            uint32_t period = 0;
            bool     run_ok = true;
            if (axl_loop_set_driver_idle_budget_ms(
                    loop, idle == 0 ? AXL_LOOP_DRIVER_BUDGET_MAX_MS : 0) != AXL_OK) {
                *ok = false;
            }
            SpanExtra x = { 0 };
            span_measure(loop, tick_ms, FG_RENDER, wait_ms, NULL, iters, &period, &n, 0, NULL,
                         &x, noise, &run_ok);
            *ok = *ok && run_ok && period != 0;
            axl_printf("  driver idle budget: renderer %u ms tick, %u ms wait, idle budget %s: "
                       "frame %u us, longest tick %u us, work phase %u us (%llu iters)\n",
                       tick_ms, wait_ms, idle == 0 ? "default" : "0", period, x.max_us,
                       x.work_us, (unsigned long long)iters);
            if (idle == 0 && x.max_us > *max_tick) {
                *max_tick = x.max_us;
            }
            uint32_t *best = idle == 0 ? on : off;
            if (period < *best) {
                *best = period;
            }
        }
    }
    if (axl_loop_set_driver_idle_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS) != AXL_OK) {
        *ok = false;
    }
}

/* One measurement of every timing case, and whether each meets its
   bound. */
typedef struct {
    bool     ran;
    uint32_t busy, idle20, idle20_long_pct, idle50, idle50_long_pct;
    uint32_t idle_custom, idle_off, none;
    uint32_t alone, shared;
    uint32_t r10_on, r10_off, r20_on, r20_off, rl_on, rl_off;
    uint32_t render_max_tick;
    bool     mid_added, mid_attach, mid_busy_id;
    int      mid_early;
    bool     chain_deferred, chain_attach;
    int      chain_ran;
    uint64_t chain_span;
    bool     dslow_deferred, dslow_attach;
    uint32_t dslow_calls;
    uint64_t dslow_longest;
    uint32_t noise;     ///< gaps the per-measurement probes saw
    uint32_t slow_long_pct, slow_max, late_long_pct, late_max;
    bool     ok_slow_cover, ok_slow_max, ok_late_cover, ok_late_max;
    bool     ok_busy, ok_idle20, ok_idle20_cover, ok_idle50, ok_idle50_cover;
    bool     ok_custom, ok_off, ok_none;
    bool     ok_frames, ok_r10, ok_r20, ok_rl, ok_render_tick;
} IdleBudgetRun;

static bool
slower_than(uint32_t on, uint32_t off)
{
    return off == 0 || on == 0 || (uint64_t)on * 100u > (uint64_t)off * RENDER_SLOW_PCT;
}

/* The three wall-clock driver cases outside the tick-span family -- a
   busy source added mid-tick, a defer chain, a self-deferring slow
   callback -- measured inside the attempt like the rest, so host noise
   voids them instead of failing them (each is probed on both sides). */
static void
measure_mid_tick(IdleBudgetRun *r, uint32_t *noise)
{
    AxlLoop *loop = axl_loop_new();

    if (loop == NULL) {
        r->ran = false;
        return;
    }
    BudgetMidTickCtx ctx = { .loop = loop };

    span_noise_probe(noise);
    r->mid_added  = axl_loop_add_timeout(loop, 30, budget_midtick_add, &ctx) != 0;
    r->mid_attach = axl_loop_attach_driver(loop, 20) == AXL_OK;
    gBS->Stall(200000);
    axl_loop_detach_driver(loop);
    span_noise_probe(noise);
    r->mid_busy_id = ctx.busy_id != 0;
    r->mid_early   = ctx.early_calls;
    axl_loop_free(loop);
}

static void
measure_defer_chain(IdleBudgetRun *r, uint32_t *noise)
{
    AxlLoop *loop = axl_loop_new();

    if (loop == NULL) {
        r->ran = false;
        return;
    }
    DeferChainCtx ctx = { .loop = loop };

    span_noise_probe(noise);
    r->chain_deferred = axl_defer(loop, defer_chain_hop, &ctx) != 0;
    r->chain_attach   = axl_loop_attach_driver(loop, 20) == AXL_OK;
    gBS->Stall(100000);
    axl_loop_detach_driver(loop);
    span_noise_probe(noise);
    r->chain_ran  = ctx.ran;
    r->chain_span = ctx.ran == DEFER_CHAIN_HOPS
                        ? ctx.at_us[DEFER_CHAIN_HOPS - 1] - ctx.at_us[0] : UINT64_MAX;
    axl_loop_free(loop);
}

static void
measure_defer_budget(IdleBudgetRun *r, uint32_t *noise)
{
    AxlLoop *loop = axl_loop_new();

    if (loop == NULL) {
        r->ran = false;
        return;
    }
    DeferSlowCtx ctx = { .loop = loop };

    span_noise_probe(noise);
    r->dslow_deferred = axl_defer(loop, defer_slow_hop, &ctx) != 0;
    r->dslow_attach   = axl_loop_attach_driver(loop, 20) == AXL_OK;
    gBS->Stall(100000);
    axl_loop_detach_driver(loop);
    ctx.stop = true;
    span_noise_probe(noise);
    r->dslow_calls   = ctx.calls;
    r->dslow_longest = ctx.longest_us;
    axl_loop_free(loop);
}

static void
idle_budget_run(AxlLoop *loop, IdleBudgetRun *r)
{
    bool     ok  = true;
    uint32_t n   = 0;
    uint32_t fr  = 0;

    axl_memset(r, 0, sizeof(*r));
    r->ran = true;
    uint32_t *noise = &r->noise;

    measure_mid_tick(r, noise);
    measure_defer_chain(r, noise);
    measure_defer_budget(r, noise);

    /* Busy foreground -- the control: the measure sees an extension, and
       a foreground that never waits keeps the busy budget. */
    r->busy = span_measure(loop, 20, FG_BUSY, 400, NULL, 0, &fr, &n, 0, NULL, NULL, noise, &ok);
    r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;

    /* A fresh loop's idle budget -- the default -- at 20 ms: 9/10 of the
       period; and at 50 ms: capped at AXL_LOOP_DRIVER_BUDGET_MAX_MS. A
       fresh loop, so nothing set here can stand in for the default. */
    AxlLoop *fresh = axl_loop_new();
    if (fresh != NULL) {
        r->idle20 = span_measure(fresh, 20, FG_WAIT, 400, NULL, 0, &fr, &n, SPAN_IDLE20_MIN,
                                 &r->idle20_long_pct, NULL, noise, &ok);
        r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;
        r->idle50 = span_measure(fresh, 50, FG_WAIT, 600, NULL, 0, &fr, &n, SPAN_IDLE50_MIN,
                                 &r->idle50_long_pct, NULL, noise, &ok);
        r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;
        /* A slow busy source: the extended tick stops in time for the next
           one, and keeps extending. */
        SpanExtra sx = { 0 };
        SpanLoad  slow = { .pass_us = SLOW_PASS_US };
        span_measure(fresh, 20, FG_WAIT, 400, &slow, 0, &fr, &n, SPAN_IDLE20_MIN,
                     &r->slow_long_pct, &sx, noise, &ok);
        r->slow_max = sx.max_us;
        r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;
        /* A late long call: the tick overruns anyway, and the extension
           resumes on the tick after the one queued behind it. */
        SpanLoad late = { .late_after_us = LATE_AFTER_US, .late_us = LATE_US };
        SpanExtra lx   = { 0 };
        span_measure(fresh, 20, FG_WAIT, 400, &late, 0, &fr, &n, SPAN_IDLE20_MIN,
                     &r->late_long_pct, &lx, noise, &ok);
        r->late_max = lx.max_us;
        r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;
        axl_loop_free(fresh);
    } else {
        r->ran = false;
    }

    /* A custom idle budget, and 0 (the idle extension off). */
    r->ran = r->ran && axl_loop_set_driver_idle_budget_ms(loop, 10) == AXL_OK;
    r->idle_custom = span_measure(loop, 20, FG_WAIT, 400, NULL, 0, &fr, &n, 0, NULL, NULL,
                                  noise, &ok);
    r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;
    r->ran = r->ran && axl_loop_set_driver_idle_budget_ms(loop, 0) == AXL_OK;
    r->idle_off = span_measure(loop, 20, FG_WAIT, 400, NULL, 0, &fr, &n, 0, NULL, NULL, noise, &ok);
    r->ran = r->ran && ok && n >= SPAN_MIN_TICKS;

    /* Budget 0 turns off ALL extension, whatever the idle budget. */
    r->ran = r->ran
             && axl_loop_set_driver_idle_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS)
                    == AXL_OK
             && axl_loop_set_driver_budget_ms(loop, 0) == AXL_OK;
    r->none = span_measure(loop, 20, FG_WAIT, 400, NULL, 0, &fr, &n, 0, NULL, NULL, noise, &ok);
    r->ran = r->ran && ok && n >= SPAN_MIN_TICKS
             && axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS)
                    == AXL_OK;

    /* A frame-paced foreground keeps its share of the CPU while it works. */
    span_noise_probe(noise);
    uint64_t iters = fg_calibrate();
    bool     fok   = true;
    r->alone = fg_frames(iters, &fok);
    span_noise_probe(noise);
    span_measure(loop, 20, FG_FRAMES, 0, NULL, iters, &r->shared, &n, 0, NULL, NULL, noise, &ok);
    r->ran = r->ran && ok && fok;

    /* A renderer whose wakes land on driver ticks keeps its frame rate:
       the default idle budget against the idle extension off, at sshd's
       10 ms tick (4 ms of work), at a 20 ms tick (10 ms), and with a wait
       that spans a whole 10 ms period. */
    uint64_t i4  = iters * 4u / FRAME_WORK_MS;
    bool     rok = true;
    render_pair(loop, 10, RENDER_WAIT_MS, i4, &r->r10_on, &r->r10_off,
                &r->render_max_tick, noise, &rok);
    render_pair(loop, 20, RENDER_WAIT_MS, iters, &r->r20_on, &r->r20_off,
                &r->render_max_tick, noise, &rok);
    render_pair(loop, 10, RENDER_LONG_WAIT_MS, i4, &r->rl_on, &r->rl_off,
                &r->render_max_tick, noise, &rok);
    r->ran = r->ran && rok;

    r->ok_busy   = r->busy >= SPAN_BUSY_MIN && r->busy <= SPAN_BUSY_MAX;
    r->ok_idle20 = r->idle20 >= SPAN_IDLE20_MIN && r->idle20 <= SPAN_IDLE20_MAX;
    r->ok_idle20_cover = r->idle20_long_pct >= SPAN_IDLE_COVER_PCT;
    r->ok_idle50_cover = r->idle50_long_pct >= SPAN_IDLE_COVER_PCT;
    r->ok_idle50 = r->idle50 >= SPAN_IDLE50_MIN && r->idle50 <= SPAN_IDLE50_MAX;
    r->ok_custom = r->idle_custom >= SPAN_IDLE10_MIN && r->idle_custom <= SPAN_IDLE10_MAX;
    r->ok_off    = r->idle_off >= SPAN_BUSY_MIN && r->idle_off <= SPAN_BUSY_MAX;
    r->ok_none   = r->none <= SPAN_NONE_MAX;
    r->ok_frames = r->alone > 0 && r->shared > 0
                   && (uint64_t)r->shared * 100u <= (uint64_t)r->alone * FRAME_STRETCH_PCT;
    r->ok_r10    = !slower_than(r->r10_on, r->r10_off);
    r->ok_r20    = !slower_than(r->r20_on, r->r20_off);
    r->ok_rl     = !slower_than(r->rl_on, r->rl_off);
    r->ok_render_tick = r->render_max_tick != 0 && r->render_max_tick <= RENDER_TICK_MAX_US;
    r->ok_slow_cover  = r->slow_long_pct >= SPAN_IDLE_COVER_PCT;
    r->ok_slow_max    = r->slow_max != 0 && r->slow_max <= SPAN_IDLE20_MAX;
    r->ok_late_cover  = r->late_long_pct >= SPAN_IDLE_COVER_PCT;
    r->ok_late_max    = r->late_max != 0 && r->late_max <= LATE_SPAN_MAX_US;

    axl_printf("  driver idle budget: tick span (us; busy median, waiting upper "
               "quartile): busy fg %u, waiting fg 20 ms tick %u (%u%% extended), "
               "50 ms tick %u (%u%% extended), idle budget 10 ms %u, idle budget 0 "
               "%u, budget 0 %u; frame-paced fg work phase (us): %u alone, %u beside "
               "the tick; renderer frame period (us), idle budget default / 0: "
               "10 ms tick %u / %u, 20 ms tick %u / %u, 10 ms tick with a long wait "
               "%u / %u\n",
               r->busy, r->idle20, r->idle20_long_pct, r->idle50, r->idle50_long_pct,
               r->idle_custom, r->idle_off, r->none, r->alone, r->shared,
               r->r10_on, r->r10_off, r->r20_on, r->r20_off, r->rl_on, r->rl_off);
    axl_printf("  driver idle budget: slow busy source (%u us a pass), waiting fg, 20 ms "
               "tick: %u%% extended, longest tick %u us; a late %u us call: %u%% extended, "
               "longest span %u us\n",
               SLOW_PASS_US, r->slow_long_pct, r->slow_max, LATE_US, r->late_long_pct,
               r->late_max);
}

/* Every bound an attempt is judged on, in the order they are reported. An
   attempt's verdict is a row of these; the group's is per bound, across the
   judged attempts (test_loop_driver_budget_idle_foreground). */
typedef enum {
    IC_RAN,
    IC_BUSY,
    IC_IDLE20,
    IC_IDLE20_COVER,
    IC_IDLE50,
    IC_IDLE50_COVER,
    IC_CUSTOM,
    IC_OFF,
    IC_NONE,
    IC_FRAMES,
    IC_R10,
    IC_R20,
    IC_RL,
    IC_RENDER_TICK,
    IC_SLOW_MAX,
    IC_SLOW_COVER,
    IC_LATE_COVER,
    IC_LATE_MAX,
    IC_MID_ADDED,
    IC_MID_ATTACH,
    IC_MID_BUSY_ID,
    IC_MID_EARLY,
    IC_CHAIN_DEFERRED,
    IC_CHAIN_ATTACH,
    IC_CHAIN_RAN,
    IC_CHAIN_SPAN,
    IC_DSLOW_DEFERRED,
    IC_DSLOW_ATTACH,
    IC_DSLOW_CALLS,
    IC_DSLOW_LONGEST,
    IC_COUNT
} IdleCheck;

_Static_assert(IC_COUNT == IDLE_TIMING_CHECKS,
               "IDLE_TIMING_CHECKS is the skip count: one per IdleCheck");

static const char *const g_idle_check_names[IC_COUNT] = {
    [IC_RAN]          = "loop: driver idle budget: every case attached, waited and saw ticks",
    [IC_BUSY]         = "loop: a busy foreground keeps the busy budget (and the measure sees it)",
    [IC_IDLE20]       = "loop: a waiting foreground lets a tick run 9/10 of a 20 ms period, no more",
    [IC_IDLE20_COVER] = "loop: at least 80% of a waiting foreground's eligible 20 ms ticks extend",
    [IC_IDLE50]       = "loop: the idle extension is capped at AXL_LOOP_DRIVER_BUDGET_MAX_MS",
    [IC_IDLE50_COVER] = "loop: at least 80% of a waiting foreground's eligible 50 ms ticks extend",
    [IC_CUSTOM]       = "loop: a custom idle budget bounds a waiting-foreground tick",
    [IC_OFF]          = "loop: idle budget 0 leaves a waiting foreground at the busy budget",
    [IC_NONE]         = "loop: budget 0 is no extension even with the foreground waiting",
    [IC_FRAMES]       = "loop: a foreground that works between short waits keeps its share",
    [IC_R10]          = "loop: a renderer waking on 10 ms driver ticks keeps its frame rate",
    [IC_R20]          = "loop: a renderer waking on 20 ms driver ticks keeps its frame rate",
    [IC_RL]           = "loop: a renderer that waits through a whole period keeps its frame rate",
    [IC_RENDER_TICK]  = "loop: no tick of a renderer run extends (R1, measured per tick)",
    [IC_SLOW_MAX]     = "loop: an extended tick with a slow busy source ends before the next is due",
    [IC_SLOW_COVER]   = "loop: at least 80% of eligible ticks extend with a slow busy source",
    [IC_LATE_COVER]   = "loop: a tick queued behind an overrun one does not stop the extension",
    [IC_LATE_MAX]     = "loop: a tick queued behind an overrun one does not extend itself",
    [IC_MID_ADDED]    = "loop: driver budget mid-tick: one-shot added",
    [IC_MID_ATTACH]   = "loop: driver budget mid-tick: attach at 20 ms",
    [IC_MID_BUSY_ID]  = "loop: driver budget mid-tick: busy source added",
    [IC_MID_EARLY]    = "loop: a busy source added mid-tick extends that same tick",
    [IC_CHAIN_DEFERRED] = "loop: driver defer chain: first hop deferred",
    [IC_CHAIN_ATTACH] = "loop: driver defer chain: attach at 20 ms",
    [IC_CHAIN_RAN]    = "loop: driver defer chain: every hop ran",
    [IC_CHAIN_SPAN]   = "loop: a driver tick runs work deferred by deferred work, not the next tick",
    [IC_DSLOW_DEFERRED] = "loop: driver defer budget: first run deferred",
    [IC_DSLOW_ATTACH] = "loop: driver defer budget: attach at 20 ms",
    [IC_DSLOW_CALLS]  = "loop: driver defer budget: the deferred callback ran",
    [IC_DSLOW_LONGEST] = "loop: deferred work that defers itself ends the tick at its budget",
};

/* The checks that are not timing bounds: the RETURN VALUES of set-up calls
   (axl_loop_add_timeout, axl_loop_attach_driver, axl_defer), which host load
   cannot make miss. A miss of one is a defect, not noise: it fails at once,
   in any judged attempt, and is printed in words verify.sh's load witness
   never matches ("failed (not timing)"), so it can never be relabelled as
   load. Everything that needs a driver tick to FIRE inside a fixed window
   is timing, whatever it looks like: IC_RAN (every case must see
   SPAN_MIN_TICKS ticks), IC_MID_BUSY_ID (the busy source is added by a
   30 ms timeout the 20 ms tick dispatches, inside a 200 ms Stall),
   IC_CHAIN_RAN and IC_DSLOW_CALLS (a tick inside a 100 ms Stall). A vCPU
   descheduled across that Stall can reach its end with no tick dispatched,
   and the probes sample only before and after it. */
#define IDLE_FUNCTIONAL_CHECKS  6u
static const bool g_idle_check_functional[IC_COUNT] = {
    [IC_MID_ADDED]      = true,
    [IC_MID_ATTACH]     = true,
    [IC_CHAIN_DEFERRED] = true,
    [IC_CHAIN_ATTACH]   = true,
    [IC_DSLOW_DEFERRED] = true,
    [IC_DSLOW_ATTACH]   = true,
};

/* One attempt's row: whether it met each bound. */
static void
idle_budget_checks(const IdleBudgetRun *r, bool ok[IC_COUNT])
{
    ok[IC_RAN]            = r->ran;
    ok[IC_BUSY]           = r->ok_busy;
    ok[IC_IDLE20]         = r->ok_idle20;
    ok[IC_IDLE20_COVER]   = r->ok_idle20_cover;
    ok[IC_IDLE50]         = r->ok_idle50;
    ok[IC_IDLE50_COVER]   = r->ok_idle50_cover;
    ok[IC_CUSTOM]         = r->ok_custom;
    ok[IC_OFF]            = r->ok_off;
    ok[IC_NONE]           = r->ok_none;
    ok[IC_FRAMES]         = r->ok_frames;
    ok[IC_R10]            = r->ok_r10;
    ok[IC_R20]            = r->ok_r20;
    ok[IC_RL]             = r->ok_rl;
    ok[IC_RENDER_TICK]    = r->ok_render_tick;
    ok[IC_SLOW_MAX]       = r->ok_slow_max;
    ok[IC_SLOW_COVER]     = r->ok_slow_cover;
    ok[IC_LATE_COVER]     = r->ok_late_cover;
    ok[IC_LATE_MAX]       = r->ok_late_max;
    ok[IC_MID_ADDED]      = r->mid_added;
    ok[IC_MID_ATTACH]     = r->mid_attach;
    ok[IC_MID_BUSY_ID]    = r->mid_busy_id;
    ok[IC_MID_EARLY]      = r->mid_early >= BUDGET_MIDTICK_CALLS_MIN;
    ok[IC_CHAIN_DEFERRED] = r->chain_deferred;
    ok[IC_CHAIN_ATTACH]   = r->chain_attach;
    ok[IC_CHAIN_RAN]      = r->chain_ran == DEFER_CHAIN_HOPS;
    ok[IC_CHAIN_SPAN]     = r->chain_span <= DEFER_CHAIN_MAX_US;
    ok[IC_DSLOW_DEFERRED] = r->dslow_deferred;
    ok[IC_DSLOW_ATTACH]   = r->dslow_attach;
    ok[IC_DSLOW_CALLS]    = r->dslow_calls != 0;
    ok[IC_DSLOW_LONGEST]  = r->dslow_longest <= DEFER_SLOW_MAX_US;
}

/* The group's verdict across attempts. Noise only ever makes a bound MISS,
   so a bound holds once any judged attempt meets it. The attempts stop
   early only when every bound has held; otherwise all IDLE_ATTEMPTS run,
   and a bound FAILS only if EVERY judged attempt missed it, with at least
   IDLE_CONFIRM of them judged. A miss in the only judged attempt (the rest
   void) is no verdict. A defect misses every time and FAILs; noise has to
   spoil the bound in every judged attempt -- the same odds as before this
   rule, with the single-judged-miss case now unjudged instead of a FAIL.
   That is for TIMING bounds. A functional check (g_idle_check_functional)
   holds only if EVERY judged attempt met it: load cannot make it miss, so
   one judged miss is a defect, decided at once. all_held, judged and more
   speak for the timing bounds alone. */
typedef struct {
    uint32_t judged;            ///< attempts whose probes were clean
    bool     held[IC_COUNT];    ///< timing: met in some judged attempt;
                                ///< functional: met in every one
    bool     all_held;          ///< every TIMING bound held
} IdleVerdict;

static void
idle_verdict_add(IdleVerdict *v, const bool ok[IC_COUNT])
{
    v->judged++;
    v->all_held = true;
    for (uint32_t i = 0; i < IC_COUNT; i++) {
        if (g_idle_check_functional[i]) {
            v->held[i] = (v->judged == 1u || v->held[i]) && ok[i];
            continue;
        }
        v->held[i]  = v->held[i] || ok[i];
        v->all_held = v->all_held && v->held[i];
    }
}

/* Every timing bound decided: held, or missed in every judged attempt with
   at least IDLE_CONFIRM judged. Read once the attempts are over. */
static bool
idle_verdict_judged(const IdleVerdict *v)
{
    return v->judged != 0 && (v->all_held || v->judged >= IDLE_CONFIRM);
}

/* Whether another attempt is to be made: until every bound has held.
   Stopping at the second judged miss would let noise fail a bound by
   spoiling two attempts where the third might have met it -- q^2 against
   the q^3 of running them all. */
static bool
idle_verdict_more(const IdleVerdict *v)
{
    return !v->all_held;
}

/* The verdict logic alone, on made-up attempt rows: no timing, so it is
   judged on every host and both arches. */
static void
test_loop_idle_verdict(void)
{
    bool all[IC_COUNT];
    bool no_r10[IC_COUNT];
    bool no_r20[IC_COUNT];

    for (uint32_t i = 0; i < IC_COUNT; i++) {
        all[i] = no_r10[i] = no_r20[i] = true;
    }
    no_r10[IC_R10] = false;
    no_r20[IC_R20] = false;

    static const IdleCheck functional[] = {
        IC_MID_ADDED, IC_MID_ATTACH, IC_CHAIN_DEFERRED, IC_CHAIN_ATTACH,
        IC_DSLOW_DEFERRED, IC_DSLOW_ATTACH,
    };
    uint32_t nfunc = 0;
    bool     listed = true;
    for (uint32_t i = 0; i < IC_COUNT; i++) {
        nfunc += g_idle_check_functional[i] ? 1u : 0u;
    }
    for (uint32_t i = 0; i < sizeof(functional) / sizeof(functional[0]); i++) {
        listed = listed && g_idle_check_functional[functional[i]];
    }
    test_check(nfunc == 6u && nfunc == IDLE_FUNCTIONAL_CHECKS && listed
                   && !g_idle_check_functional[IC_RAN]
                   && !g_idle_check_functional[IC_MID_BUSY_ID]
                   && !g_idle_check_functional[IC_CHAIN_RAN]
                   && !g_idle_check_functional[IC_DSLOW_CALLS]
                   && !g_idle_check_functional[IC_MID_EARLY]
                   && !g_idle_check_functional[IC_CHAIN_SPAN]
                   && !g_idle_check_functional[IC_DSLOW_LONGEST],
               "loop: idle verdict: exactly the six return-value checks are functional");

    bool no_attach[IC_COUNT];
    for (uint32_t i = 0; i < IC_COUNT; i++) {
        no_attach[i] = true;
    }
    no_attach[IC_MID_ATTACH] = false;

    IdleVerdict v = { 0 };
    idle_verdict_add(&v, no_attach);
    test_check(idle_verdict_judged(&v) && v.all_held && !v.held[IC_MID_ATTACH],
               "loop: idle verdict: a functional miss in the only judged attempt fails at once");
    idle_verdict_add(&v, all);
    test_check(!v.held[IC_MID_ATTACH],
               "loop: idle verdict: a later clean attempt does not undo a functional miss");

    axl_memset(&v, 0, sizeof(v));
    test_check(!idle_verdict_judged(&v), "loop: idle verdict: no judged attempt is no verdict");
    idle_verdict_add(&v, all);
    test_check(idle_verdict_judged(&v) && v.all_held,
               "loop: idle verdict: one judged attempt meeting every bound is a pass");
    test_check(!idle_verdict_more(&v),
               "loop: idle verdict: every bound held stops the attempts");
    test_check(v.held[IC_MID_ATTACH],
               "loop: idle verdict: a functional check met in the only judged attempt holds");

    axl_memset(&v, 0, sizeof(v));
    idle_verdict_add(&v, no_r10);
    test_check(!idle_verdict_judged(&v),
               "loop: idle verdict: a miss in the only judged attempt is no verdict");
    idle_verdict_add(&v, no_r10);
    test_check(idle_verdict_judged(&v) && !v.held[IC_R10] && v.held[IC_R20] && !v.all_held,
               "loop: idle verdict: a miss in two judged attempts fails that bound alone");
    test_check(idle_verdict_more(&v),
               "loop: idle verdict: a bound missed twice still gets the remaining attempts");
    idle_verdict_add(&v, all);
    test_check(idle_verdict_judged(&v) && v.all_held && !idle_verdict_more(&v),
               "loop: idle verdict: a bound missed twice, then met, holds");

    axl_memset(&v, 0, sizeof(v));
    idle_verdict_add(&v, no_r10);
    idle_verdict_add(&v, no_r20);
    test_check(idle_verdict_judged(&v) && v.all_held,
               "loop: idle verdict: misses that do not repeat are noise -- every bound held");

    axl_memset(&v, 0, sizeof(v));
    idle_verdict_add(&v, no_r10);
    idle_verdict_add(&v, all);
    test_check(idle_verdict_judged(&v) && v.all_held,
               "loop: idle verdict: a miss then a clean attempt is a pass");
}

/* " (clock unreadable)", or the gap count, worst and spacing. */
static void
probe_describe(const ProbeResult *pr, char *buf, size_t cap)
{
    if (!pr->clock_ok) {
        axl_snprintf(buf, cap, "clock unreadable or not advancing");
        return;
    }
    int n = axl_snprintf(buf, cap, "%u gap(s) over %u us, worst %u us", pr->gaps,
                         PROBE_GAP_US, pr->worst_us);
    for (uint32_t i = 0; i < pr->nspacing && n > 0 && (size_t)n < cap; i++) {
        n += axl_snprintf(buf + n, cap - (size_t)n, "%s%u", i == 0 ? ", spacing us " : " ",
                          pr->spacing[i]);
    }
}

static void
test_loop_driver_budget_idle_foreground(void)
{
    AxlLoop       *loop = axl_loop_new();
    IdleBudgetRun  r = {0};   /* filled by every judged attempt; zero otherwise (gcc -O2 cannot see it) */

    if (loop == NULL) {
        test_fail("loop: driver idle budget: loop_new alloc failed");
        return;
    }

    /* 1. The setter's validation -- exact return values. */
    test_check(axl_loop_set_driver_idle_budget_ms(NULL, 5) == AXL_INVALID,
               "loop: set_driver_idle_budget_ms(NULL) is AXL_INVALID");
    test_check(axl_loop_set_driver_idle_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS + 1u)
                   == AXL_INVALID,
               "loop: set_driver_idle_budget_ms(MAX + 1) is AXL_INVALID");
    test_check(axl_loop_set_driver_idle_budget_ms(loop, 0) == AXL_OK,
               "loop: set_driver_idle_budget_ms(0) is AXL_OK");
    test_check(axl_loop_set_driver_idle_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS)
                   == AXL_OK,
               "loop: set_driver_idle_budget_ms(MAX) is AXL_OK");
    test_check(axl_loop_set_driver_idle_budget_ms(loop, 10) == AXL_OK,
               "loop: set_driver_idle_budget_ms(10) is AXL_OK");
    test_check(axl_loop_set_driver_budget_ms(loop, 0) == AXL_OK
                   && axl_loop_set_driver_idle_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_MAX_MS)
                          == AXL_OK,
               "loop: set_driver_budget_ms(0) leaves the idle budget settable");
    test_check(axl_loop_set_driver_budget_ms(loop, AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS)
                   == AXL_OK,
               "loop: set_driver_budget_ms(DEFAULT) after 0 is AXL_OK");

    /* 2. The timing cases, up to IDLE_ATTEMPTS times. Host noise moves
          every figure in ONE direction only -- a preempted vCPU shortens a
          waiting tick's span (a late idle pass reads as the foreground
          having run) and lengthens a frame -- so a bound met in any judged
          attempt is met, and a defect misses its bound in every attempt:
          each sabotage of this code did, on both arches. So the attempts
          stop early only once every bound has held; otherwise all of them
          run, and a bound FAILS only if every judged attempt missed it,
          with at least IDLE_CONFIRM judged (IdleVerdict). Noise the probes
          miss (it happened, at load 11-17: they sample at points, and load
          arriving between them is invisible) must spoil the bound in every
          judged attempt -- the odds of the rule before this one -- and a
          miss in the only judged attempt, the others void, is no verdict:
          the group reads unjudged, never pass, where it used to FAIL.
          verify.sh's quiet slot adds an independent witness for load on
          this kernel (see its header). Every attempt is printed.
          Three attempts of about 8 s keep a failing run inside
          test-axl.sh's 120 s boot timeout -- measured with every attempt
          failing: 105.5 s of guest time on x64, 100.8 s on aarch64 -- so a
          regression is a named FAIL here, not a truncated suite. */
    IdleVerdict   v        = { 0 };
    uint32_t      max_gaps = 0;
    bool          clock_ok = true;
    ProbeResult   pre;
    ProbeResult   post;
    IdleBudgetRun t;
    for (uint32_t a = 0; a < IDLE_ATTEMPTS && idle_verdict_more(&v); a++) {
        /* 3. The host-noise probe, before and after: an attempt is judged
              only if the vCPU kept running through both. A void one counts
              for nothing, pass or fail. */
        char d1[160];
        char d2[160];
        host_probe(&pre, PROBE_SPIN_MS);
        bool usable = probe_clean(&pre);
        axl_memset(&post, 0, sizeof(post));
        post.clock_ok = true;
        if (usable) {
            idle_budget_run(loop, &t);
            host_probe(&post, PROBE_SPIN_MS);
            usable = probe_clean(&post) && t.noise == 0;
            max_gaps = t.noise > max_gaps ? t.noise : max_gaps;
        }
        clock_ok = clock_ok && pre.clock_ok && post.clock_ok;
        max_gaps = pre.gaps > max_gaps ? pre.gaps : max_gaps;
        max_gaps = post.gaps > max_gaps ? post.gaps : max_gaps;
        probe_describe(&pre, d1, sizeof(d1));
        if (probe_clean(&pre)) {
            probe_describe(&post, d2, sizeof(d2));
            size_t l2 = axl_strlen(d2);
            axl_snprintf(d2 + l2, sizeof(d2) - l2, "; %u gap(s) around its measurements",
                         t.noise);
        } else {
            axl_snprintf(d2, sizeof(d2), "not run");
        }
        axl_printf("  driver idle budget: host probe, attempt %u (%u ms of spinning): "
                   "before %s; after %s: %s\n", (unsigned)(a + 1u), PROBE_SPIN_MS, d1, d2,
                   usable ? "valid" : "VOID");
        if (!usable) {
            continue;
        }
        r = t;
        bool ok[IC_COUNT];
        idle_budget_checks(&t, ok);
        for (uint32_t i = 0; i < IC_COUNT; i++) {
            /* Two wordings on purpose: verify.sh's load witness reads only
               "missed:", so a functional failure can never be called load. */
            if (!ok[i]) {
                axl_printf("  driver idle budget: judged attempt %u %s: %s\n",
                           (unsigned)(a + 1u),
                           g_idle_check_functional[i] ? "failed (not timing)" : "missed",
                           g_idle_check_names[i]);
            }
        }
        idle_verdict_add(&v, ok);
    }
    /* The functional checks first: decided by any judged attempt, whatever
       the timing verdict. With none judged they are skipped with the rest. */
    if (v.judged != 0) {
        for (uint32_t i = 0; i < IC_COUNT; i++) {
            if (g_idle_check_functional[i]) {
                test_check(v.held[i], g_idle_check_names[i]);
            }
        }
    }
    if (!idle_verdict_judged(&v)) {
        /* Never a pass: a counted skip, balanced against the checks below,
           that says why and with what figures. verify.sh's quiet AxlTestLoop
           slot keys on the ABSENCE of the marker below and reports UNJUDGED;
           it quotes the "(probe: N gaps" figure when one is given. */
        char why[200];
        if (v.judged == 0) {
            axl_snprintf(why, sizeof(why),
                         "loop: driver idle budget timing: host too noisy to judge "
                         "(probe: %u gaps over %u us%s) -- every attempt voided", max_gaps,
                         PROBE_GAP_US, clock_ok ? "" : ", clock unreadable");
        } else {
            axl_snprintf(why, sizeof(why),
                         "loop: driver idle budget timing: a bound missed in the only judged "
                         "attempt was not reproduced (probe: %u gaps over %u us) -- unconfirmed",
                         max_gaps, PROBE_GAP_US);
            /* A fixed line verify.sh quotes, so a real defect on a noisy box
               does not read exactly like pure noise. */
            for (uint32_t i = 0; i < IC_COUNT; i++) {
                if (!v.held[i] && !g_idle_check_functional[i]) {
                    axl_printf("LOOP-TIMING: unconfirmed miss: %s\n", g_idle_check_names[i]);
                }
            }
        }
        test_skip_n(v.judged == 0 ? IDLE_TIMING_CHECKS
                                  : IDLE_TIMING_CHECKS - IDLE_FUNCTIONAL_CHECKS, why);
        axl_loop_free(loop);
        return;
    }
    /* A fixed marker, independent of every message above: verify.sh's
       quiet slot is green only when it sees this AND nothing failed --
       a reworded skip cannot read as a pass. */
    axl_printf("LOOP-TIMING: judged\n");
    axl_printf("  driver idle budget: %u judged attempt(s); longest tick of the last one's "
               "default-budget renderer runs: %u us; busy source added mid-tick: %d call(s) "
               "in its first 3 ms; defer chain: %d hop(s), first to last %llu us; defer "
               "budget: %u run(s), longest back-to-back %llu us\n",
               v.judged, r.render_max_tick, r.mid_early, r.chain_ran,
               (unsigned long long)r.chain_span, r.dslow_calls,
               (unsigned long long)r.dslow_longest);
    /* A timing bound fails only if every judged attempt -- at least
       IDLE_CONFIRM of them -- missed it. */
    for (uint32_t i = 0; i < IC_COUNT; i++) {
        if (!g_idle_check_functional[i]) {
            test_check(v.held[i], g_idle_check_names[i]);
        }
    }
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Driver attach when the idle-loop members cannot all be registered
//
// The backend's notify table (64 slots, shared by every attached loop's
// timer and two idle-loop members, AxlTaskPool hooks and protocol
// notifies) is the resource at stake: when the second member fails after
// the first was created, attach must give the first one's slot back at
// once, not hold it until detach. Occupancy is not public, so the test
// reads it the way a consumer would meet it -- by what still attaches:
//
//   fill the table with attached loops until one is refused; free one
//   (three slots back); attach Z with its wake member's allocation failed
//   (axl_mem_fail_next_alloc). Z keeps its timer and, with the fix, gives
//   the pass member back: two slots stay free. Y then takes its timer and
//   pass member and finds no slot for its wake member -- the same branch,
//   by exhaustion this time -- and gives the pass member back: one slot
//   stays free, and W attaches. Without the fix Z keeps two slots, Y one,
//   and W is refused.
// ---------------------------------------------------------------------------

/* More loops than the table can hold attached at once. */
#define FILL_LOOPS_MAX  40u
/* More one-slot loops than the table has slots. */
#define SLOT_LOOPS_MAX  80u

typedef struct {
    int calls;
} AttachProbe;

static bool
attach_probe_idle(void *user)
{
    ((AttachProbe *)user)->calls++;
    return AXL_SOURCE_CONTINUE;
}

/* The free slots of the backend's notify table, counted one by one: each
   probe loop attaches with its pass member's allocation failed
   (axl_mem_fail_next_alloc(2): the timer's context is the first, the pass
   member's the second), so it holds its timer and nothing else -- exactly
   one slot. Loop counts would not do: a full attach takes three and the
   tail loops one, so a two-slot leak can leave the loop count unchanged. */
static uint32_t
count_free_slots(void)
{
    AxlLoop *l[SLOT_LOOPS_MAX] = { 0 };
    uint32_t n = 0;

    for (; n < SLOT_LOOPS_MAX; n++) {
        l[n] = axl_loop_new();
        if (l[n] == NULL) {
            break;
        }
        axl_mem_fail_next_alloc(2);
        int rc = axl_loop_attach_driver(l[n], 1000);
        axl_mem_fail_next_alloc(0);
        if (rc != AXL_OK) {
            axl_loop_free(l[n]);
            break;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        axl_loop_detach_driver(l[i]);
        axl_loop_free(l[i]);
    }
    return n;
}

static void
test_loop_attach_driver_idle_member_fails(void)
{
    AxlLoop *fill[FILL_LOOPS_MAX] = { 0 };
    uint32_t nfill = 0;
    bool     refused = false;
    uint32_t slots_before = count_free_slots();

    /* 1. Fill the table: a long period, so these loops barely tick. */
    for (; nfill < FILL_LOOPS_MAX; nfill++) {
        fill[nfill] = axl_loop_new();
        if (fill[nfill] == NULL) {
            break;
        }
        if (axl_loop_attach_driver(fill[nfill], 1000) != AXL_OK) {
            axl_loop_free(fill[nfill]);
            fill[nfill] = NULL;
            refused = true;
            break;
        }
    }
    test_check(refused && nfill >= 2,
               "loop: attach idle members: the notify table fills and refuses an attach");

    /* 2. Free one fully attached loop (the first): three slots back. */
    axl_loop_detach_driver(fill[0]);
    axl_loop_free(fill[0]);
    fill[0] = NULL;

    /* 3. Z: its third allocation in attach -- the wake member's bridge
          context, after the timer's and the pass member's -- fails. */
    AxlLoop    *z     = axl_loop_new();
    AttachProbe probe = { 0 };
    int         z_rc  = AXL_ERR;
    if (z != NULL) {
        axl_loop_add_idle(z, attach_probe_idle, &probe);
        axl_mem_fail_next_alloc(3);
        z_rc = axl_loop_attach_driver(z, 20);
        axl_mem_fail_next_alloc(0);
        gBS->Stall(100000);   /* the loop still runs: about 5 ticks */
    }
    test_check(z_rc == AXL_OK,
               "loop: attach idle members: attach succeeds when the wake member fails");
    test_check(probe.calls >= 2,
               "loop: attach idle members: the loop still ticks without them");

    /* 4. Y fills the last slots and meets the same branch by exhaustion;
          W still finds the slot both gave back. */
    AxlLoop *y    = axl_loop_new();
    AxlLoop *w    = axl_loop_new();
    int      y_rc = y != NULL ? axl_loop_attach_driver(y, 1000) : AXL_ERR;
    int      w_rc = w != NULL ? axl_loop_attach_driver(w, 1000) : AXL_ERR;
    test_check(y_rc == AXL_OK,
               "loop: attach idle members: attach succeeds when the table runs out mid-way");
    test_check(w_rc == AXL_OK,
               "loop: attach idle members: a failed member's partner slot is given back at once");

    /* 5. Teardown: every loop detaches and frees -- the leak gate and the
          backend's double-close ring see the rest. */
    AxlLoop *all[3] = { z, y, w };
    int      rcs[3] = { z_rc, y_rc, w_rc };
    for (uint32_t i = 0; i < 3; i++) {
        if (all[i] != NULL) {
            if (rcs[i] == AXL_OK) {
                axl_loop_detach_driver(all[i]);
            }
            axl_loop_free(all[i]);
        }
    }
    for (uint32_t i = 0; i < nfill; i++) {
        if (fill[i] != NULL) {
            axl_loop_detach_driver(fill[i]);
            axl_loop_free(fill[i]);
        }
    }
    /* Everything is back: as many free slots as before the test, counted
       slot by slot. */
    uint32_t slots_after = count_free_slots();
    axl_printf("  attach idle members: %u loop(s) on the first fill; %u free slot(s) "
               "before, %u after teardown\n", nfill, slots_before, slots_after);
    test_check(slots_before >= 3 && slots_after == slots_before,
               "loop: attach idle members: detach returns every slot (counted one by one)");
}

// ---------------------------------------------------------------------------
// Keypress drain in the non-blocking dispatch path
//
// SOURCE_KEYPRESS is never in event_array (its WaitForKeyEx notify discards
// modifier-only partials), so it is selected off the poll timer instead. That
// selection used to live only in the BLOCKING branch of next_event, which two
// real consumers never reach: a driver-mode pump (axl_loop_attach_driver — a
// GUI hosting a blocking nested Shell) and any blocking loop that also has an
// idle source (next_event routes `blocking && has_idle` through the same
// CheckEvent path). Both stranded every keystroke in the firmware queue.
// ---------------------------------------------------------------------------

static int keypress_drain_calls;

static bool
on_keypress_drain(AxlInputKey key, void *data)
{
    (void)key;
    (void)data;
    keypress_drain_calls++;
    return AXL_SOURCE_CONTINUE;
}

/* Dispatch non-blocking until it returns @p want, bounded by BOTH a wall-clock
   budget and an iteration cap. Returns true if @p want was observed.

   Two bounds because either alone can fail open: the clock reads 0 on a host
   without a usable counter frequency, and an unbounded iteration count wedges
   QEMU -- test-axl.sh runs every binary in ONE boot under ONE timeout, so a
   spin here would starve every binary after it. */
static bool
keypress_spin_until(AxlLoop *loop, int want, uint64_t *at_us)
{
    uint64_t started_us = axl_time_get_us();

    for (unsigned i = 0; i < KEYPRESS_SPIN_ITERS; i++) {
        /* Stamped BEFORE the call, and reported for the one that succeeds.
           A dispatch that returns 0 has already RUN the source -- including
           the firmware key-drain -- so a timestamp taken afterwards would
           exclude exactly the segment a caller needs to measure. */
        uint64_t before_us = axl_time_get_us();
        int      rc        = axl_loop_dispatch(loop, false);

        if (rc == want) {
            if (at_us != NULL) {
                *at_us = before_us;
            }
            return true;
        }
        /* A quit latches rc < 0 forever -- neither 0 nor 1 -- so without this
           both spins burn their whole bound before anyone hears about it. */
        if (rc < 0) {
            return false;
        }
        if (started_us != 0) {
            uint64_t now_us = axl_time_get_us();
            if (now_us != 0 && now_us - started_us > KEYPRESS_SPIN_BUDGET_US) {
                return false;
            }
        }
    }
    return false;
}

static void
test_keypress_drain_non_blocking(void)
{
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("loop: keypress drain: loop_new alloc failed");
        return;
    }

    keypress_drain_calls = 0;
    AxlSourceId id = axl_loop_add_key_press(loop, on_keypress_drain, NULL);
    test_check(id != 0, "loop: add_key_press returns a source id");

    /* The poll timer is the drain tick. Let it expire, then take TWO
       non-blocking dispatches BACK-TO-BACK, capturing both results into locals
       BEFORE asserting anything:
         - the first selects the keypress source (rc 0 = a source ran) and
           consumes the poll-timer signal;
         - the second must find nothing (rc 1) — proving the source is gated on
           the poll timer and re-selected only ONCE per tick, not on every call.
           driver_dispatch_notify loops until dispatch reports nothing pending,
           so an always-ready keypress source would burn the whole per-tick
           budget every tick.
       No Stall and NO test_check (serial I/O) between the two dispatches on
       purpose: the "not re-armed yet" invariant holds only while under
       POLL_INTERVAL_MS (10 ms) of wall-clock elapses between them.

       Measuring that window's DURATION was the previous hardening, and it is
       the wrong measurement. The selection is gated on
       axl_backend_event_check(loop->poll_timer) (axl-loop.c), i.e. CheckEvent
       on a PERIODIC timer: the timer signals once per period and that signal
       PERSISTS until a check consumes it. So the signal the first dispatch
       consumes may have been waiting almost a full period already -- a
       dispatch returning 0 says a tick fired at some unknown point in the
       past, NOT that one just fired. The time left before the next tick is
       therefore an arbitrary 0..period, and a short window landing near the
       end of it sees the timer signal again, legitimately. The second dispatch
       returns 0, and the sampler prints nothing because the window itself
       looked fine. That is the residual: red once in a loaded verify.sh,
       against 26 green runs of this same code standalone (idle, under CPU
       hogs, and under a concurrent verify.sh) -- rare enough that it was never
       reproduced on demand, which is why this is fixed by construction rather
       than by tuning a threshold against a measurement.

       So PIN THE PHASE instead of timing it. Drain to idle, then spin until
       the next tick selects the source -- the instant that dispatch returns 0
       a tick has just fired, so very nearly a full period remains before the
       next one, whatever the host is doing. The pair of calls that follows is
       then bounded by the loop's own period rather than by host scheduling.

       The gap is still measured, because a preemption longer than a whole
       poll period would still invalidate the assertion and must be said out
       loud rather than reported as a loop defect. The difference is that the
       measurement now guards the window it is actually about.

       The firmware queue is empty here, so the callback never fires — what is
       under test is that the source is reachable at all, and gated to one
       drain per tick. */
    unsigned reselected = 0;
    uint64_t burst_us   = 0;
    uint64_t tick_at_us = 0;

    /* Drain whatever is already selected, leaving the loop idle. */
    bool drained = keypress_spin_until(loop, 1, NULL);
    /* Pin the phase: the tick that flips this back to 0 has just fired, and
       tick_at_us stamps the moment just before that dispatch consumed it. */
    bool synced  = drained && keypress_spin_until(loop, 0, &tick_at_us);

    /* The burst. Nothing between the calls -- no Stall, no test_check (serial
       I/O would itself span ticks). An ungated source answers all 64. */
    for (unsigned i = 0; i < KEYPRESS_BURST_CALLS; i++) {
        if (axl_loop_dispatch(loop, false) == 0) {
            reselected++;
        }
    }
    uint64_t ended_us = axl_time_get_us();

    /* Measured from BEFORE the pinning dispatch, so it covers that dispatch's
       own firmware key-drain -- a preemption there is just as able to advance
       the tick as one inside the burst. axl_time_get_us() reports 0 when the
       counter frequency is unavailable; that is reported rather than read as a
       clean duration of 0. */
    bool clock_ok = (tick_at_us != 0 && ended_us != 0);
    if (clock_ok) {
        burst_us = ended_us - tick_at_us;
    }

    /* The bound is DERIVED, not tuned: every legitimate re-selection costs a
       whole poll period, so the burst's own measured duration says how many
       could have happened. A slow host raises its own bar. With the clock
       unavailable only SLACK applies, which is still a 64-vs-2 discriminator
       against the defect. */
    unsigned allowance = KEYPRESS_BURST_SLACK;
    if (clock_ok) {
        allowance += (unsigned)(burst_us / POLL_INTERVAL_US);
    }
    bool capped = (allowance > KEYPRESS_BURST_CEILING);
    if (capped) {
        allowance = KEYPRESS_BURST_CEILING;
    }

    /* Ordered so the most explanatory NOTE wins. A failed sync is reported
       first but must NOT quote the budget as though it were enforced -- with
       the clock unavailable the spin is bounded only by its iteration cap. */
    if (!synced) {
        axl_printf("  NOTE: keypress drain: no poll tick observed (drained=%d)"
                   "; bound was %llu us%s. The assertion below never reached "
                   "its precondition\n",
                   (int)drained,
                   (unsigned long long)KEYPRESS_SPIN_BUDGET_US,
                   (axl_time_get_us() == 0)
                       ? " but the clock is unavailable, so only the iteration"
                         " cap applied"
                       : "");
    } else if (!clock_ok) {
        axl_printf("  NOTE: keypress drain: axl_time_get_us() is unavailable, "
                   "so the burst allowance is SLACK (%u) alone rather than "
                   "SLACK plus the periods that actually elapsed\n",
                   KEYPRESS_BURST_SLACK);
    } else if (capped) {
        axl_printf("  NOTE: keypress drain: the burst took %llu us (>= %u "
                   "periods); the allowance was capped at %u so the assertion "
                   "can still fail. A failure below is a loaded host, not a "
                   "loop defect\n",
                   (unsigned long long)burst_us,
                   (unsigned)KEYPRESS_BURST_CEILING,
                   (unsigned)KEYPRESS_BURST_CEILING);
    } else if (reselected > 0) {
        axl_printf("  NOTE: keypress drain: %u of %u burst dispatches were "
                   "re-selected across %llu us (allowance %u); a tick landed "
                   "inside the burst, which is legitimate\n",
                   reselected, (unsigned)KEYPRESS_BURST_CALLS,
                   (unsigned long long)burst_us, allowance);
    }

    test_check(synced,
               "loop: non-blocking dispatch drains keypress on the poll tick");
    test_check(keypress_drain_calls == 0,
               "loop: empty key queue -> keypress callback not invoked");
    test_check(reselected <= allowance,
               "loop: keypress is gated on the poll tick, not re-selected on "
               "every dispatch");

    /* The next tick re-arms it (the poll timer is periodic). */
    gBS->Stall(2 * POLL_INTERVAL_US);
    test_check(axl_loop_dispatch(loop, false) == 0,
               "loop: keypress is re-selected on the following poll tick");

    /* A loop with no keypress source must not consume the poll timer: an
       empty non-blocking dispatch still reports "nothing pending". */
    axl_loop_remove_source(loop, id);
    gBS->Stall(2 * POLL_INTERVAL_US);
    test_check(axl_loop_dispatch(loop, false) == 1,
               "loop: no keypress source -> non-blocking dispatch stays idle");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Raised-TPL safety of nested blocking waits
//
// A consumer's driver-pump (axl_loop_attach_driver) dispatches notifies at
// TPL_CALLBACK. A callback that performs a SYNCHRONOUS axl-sdk network op
// (axl_udp_send / axl_http_post / a DNS lookup) reaches a NESTED axl_loop_run
// via the _axl_*_wait family (src/net/axl-net-wait.c -> src/event/axl-wait.c).
// gBS->WaitForEvent returns EFI_UNSUPPORTED above TPL_APPLICATION, so before
// the raised-TPL fix that nested loop spun forever and hard-wedged the server
// (SoftBMC syslog-under-console-mirror wedge, 2026-06-16).
//
// These exercise that exact path with NO real network I/O: a blocking wait
// nested at TPL_CALLBACK must make progress (the loop falls back to a
// CheckEvent sweep + Stall instead of WaitForEvent) and return its normal
// result. WITHOUT the fix each of these HANGS (infinite spin) — it cannot
// fail "cleanly", so confirm RED by running this binary in ISOLATION
// (TEST_APPS_ONLY=AxlTestLoop) and watching it stall, never in the full suite.
// ---------------------------------------------------------------------------

static bool
raised_tpl_flip_cond(void *ctx)
{
    int *calls = (int *)ctx;
    (*calls)++;
    return *calls >= 3;   /* satisfied on the 3rd poll */
}

static void
test_loop_wait_at_raised_tpl(void)
{
    /* 1. Pure timeout wait nested at TPL_CALLBACK completes — the timeout
       timer fires and is observed via CheckEvent, not WaitForEvent. */
    EFI_TPL   old = gBS->RaiseTPL(TPL_CALLBACK);
    AxlStatus rc = axl_wait_ms(NULL, 20);
    gBS->RestoreTPL(old);
    test_check(rc == AXL_OK,
               "loop: axl_wait_ms completes at TPL_CALLBACK (no WaitForEvent wedge)");

    /* 2. Condition+tick wait nested at TPL_CALLBACK completes — the tick
       source fires and polls the condition to satisfaction. */
    int calls = 0;
    old = gBS->RaiseTPL(TPL_CALLBACK);
    rc = axl_wait_for(raised_tpl_flip_cond, &calls, NULL, 5000000);
    gBS->RestoreTPL(old);
    test_check(rc == AXL_OK,
               "loop: axl_wait_for condition resolves at TPL_CALLBACK");
    test_check(calls >= 3,
               "loop: tick polled the condition at TPL_CALLBACK");
}

// ---------------------------------------------------------------------------
// The faithful reproduction: a sync nested wait performed from INSIDE a
// driver-pump notify (the SoftBMC scenario, minus the network protocol).
// ---------------------------------------------------------------------------

typedef struct {
    bool       done;
    AxlStatus  rc;
} NestedWaitCtx;

static bool
nested_wait_from_pump(void *user)
{
    NestedWaitCtx *c = (NestedWaitCtx *)user;
    if (!c->done) {
        c->done = true;
        c->rc = axl_wait_ms(NULL, 20);   /* nested loop, runs at TPL_CALLBACK */
    }
    return AXL_SOURCE_CONTINUE;
}

static void
test_loop_sync_wait_from_driver_pump(void)
{
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("sync-wait-pump: loop_new alloc failed");
        return;
    }

    NestedWaitCtx ctx = { .done = false, .rc = AXL_ERR };
    axl_loop_add_idle(loop, nested_wait_from_pump, &ctx);

    test_check(axl_loop_attach_driver(loop, 20) == AXL_OK,
               "sync-wait-pump: attach_driver");
    gBS->Stall(200000);  /* 200 ms — a notify fires the idle cb at TPL_CALLBACK */
    test_check(axl_loop_detach_driver(loop) == AXL_OK,
               "sync-wait-pump: detach_driver");

    test_check(ctx.done,
               "sync-wait-pump: driver notify ran the callback");
    test_check(ctx.rc == AXL_OK,
               "sync-wait-pump: nested sync wait from a TPL_CALLBACK notify completes");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Loop-callback re-entrancy marker (_axl_loop_in_callback) — substrate for the
// sync-wait re-entrancy guard (docs/AXL-Loop-Reentrancy-Plan.md Item 1).
//
// _axl_loop_in_callback() is defined in axl-loop.c; forward-declared here
// because the loop's internal header is src/loop-only.
// ---------------------------------------------------------------------------

bool _axl_loop_in_callback(void);

static bool g_rtin_observed_in_cb;

static bool
rtin_observe_cb(void *data)
{
    g_rtin_observed_in_cb = _axl_loop_in_callback();
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static void
test_loop_in_callback_marker(void)
{
    test_check(!_axl_loop_in_callback(),
               "in_callback: false at top level (not inside a dispatch)");

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("in_callback: loop_new alloc failed");
        return;
    }

    g_rtin_observed_in_cb = false;
    /* One-shot timeout: when its callback runs (dispatched by the loop),
       _axl_loop_in_callback() must read true. */
    axl_loop_add_timeout(loop, 5, rtin_observe_cb, loop);
    axl_loop_run(loop);

    test_check(g_rtin_observed_in_cb,
               "in_callback: true inside a dispatched loop callback");
    test_check(!_axl_loop_in_callback(),
               "in_callback: false again after the loop returns");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
// Ctrl-C intercept flag (GUI apps deliver Ctrl+C instead of quitting)
// ---------------------------------------------------------------------------

static void
test_intercept_break_flag(void)
{
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) { test_fail("intercept_break: alloc"); return; }

    test_check(axl_loop_intercept_break(loop),
               "intercept_break: defaults to true (Ctrl-C quits)");
    axl_loop_set_intercept_break(loop, false);
    test_check(!axl_loop_intercept_break(loop),
               "intercept_break: set false (Ctrl-C delivered to the app)");
    axl_loop_set_intercept_break(loop, true);
    test_check(axl_loop_intercept_break(loop),
               "intercept_break: set back to true");

    axl_loop_free(loop);

    // A driver-attached loop is not the foreground app: the Shell's Ctrl-C
    // belongs to whatever the Shell is running, so by default it does not
    // intercept -- and gets the default back when it detaches.
    loop = axl_loop_new();
    if (loop == NULL) { test_fail("intercept_break: alloc 2"); return; }
    test_check(axl_loop_attach_driver(loop, 50) == AXL_OK,
               "intercept_break: attach_driver");
    test_check(!axl_loop_intercept_break(loop),
               "intercept_break: a driver-attached loop does not intercept by default");
    test_check(axl_loop_detach_driver(loop) == AXL_OK,
               "intercept_break: detach_driver");
    test_check(axl_loop_intercept_break(loop),
               "intercept_break: detached, the foreground default (true) is back");
    // An explicit choice wins over the attachment, both ways.
    axl_loop_set_intercept_break(loop, true);
    test_check(axl_loop_attach_driver(loop, 50) == AXL_OK,
               "intercept_break: re-attach after an explicit true");
    test_check(axl_loop_intercept_break(loop),
               "intercept_break: an explicit true survives attach_driver");
    test_check(axl_loop_detach_driver(loop) == AXL_OK,
               "intercept_break: detach again");
    axl_loop_set_intercept_break(loop, false);
    test_check(!axl_loop_intercept_break(loop),
               "intercept_break: an explicit false holds in the foreground");
    axl_loop_free(loop);

    // NULL is a safe no-op / false.
    axl_loop_set_intercept_break(NULL, false);
    test_check(!axl_loop_intercept_break(NULL),
               "intercept_break: NULL loop returns false");
}

// ---------------------------------------------------------------------------
// Source ids are PROCESS-globally unique, not per-loop
// ---------------------------------------------------------------------------

// Two fresh loops must hand out DISTINCT ids for their first source. Per-loop
// ids (each loop counting from 1) made the same id live on every loop, so a
// source id that outlived its loop (e.g. a freed ephemeral sync-wrapper loop)
// could be removed from a DIFFERENT loop and silently delete an unrelated
// source — the adbf5461 second-server-dead-accept class. A global counter makes
// that cross-loop removal a harmless no-op.
static void
test_source_ids_globally_unique(void)
{
    AxlLoop *a = axl_loop_new();
    AxlLoop *b = axl_loop_new();
    if (a == NULL || b == NULL) {
        test_fail("global-id: alloc");
        axl_loop_free(a);
        axl_loop_free(b);
        return;
    }

    // The callbacks never fire (the loops are not run); we only inspect ids.
    AxlSourceId id_a = axl_loop_add_idle(a, on_idle_count, NULL);
    AxlSourceId id_b = axl_loop_add_idle(b, on_idle_count, NULL);

    test_check(id_a != 0 && id_b != 0, "loop: source ids are nonzero");
    test_check(id_a != id_b,
               "loop: source ids unique across loops (no per-loop collision)");

    // A stale cross-loop id can't match a source on the other loop, so removing
    // it is a no-op; ids keep advancing from the single global counter.
    axl_loop_remove_source(b, id_a);
    AxlSourceId id_b2 = axl_loop_add_idle(b, on_idle_count, NULL);
    test_check(id_b2 != id_a && id_b2 != id_b,
               "loop: ids keep advancing globally after a cross-loop remove");

    axl_loop_free(a);
    axl_loop_free(b);
}

// ---------------------------------------------------------------------------
// iterate_until fails closed when it cannot register a wake source
// (P3 security review MED-2 / fix S1)
//
// axl_loop_iterate_until used to ignore the return of axl_loop_add_event
// (done) and axl_loop_add_timeout (the deadline), both of which return 0
// when the loop's fixed source table is full. The wait then exited only on
// a flag that nothing could ever set: done == NULL with a failed timeout
// waited forever, both failing waited forever, and a failed done with a
// succeeding timeout returned AXL_TIMEOUT even though nothing was actually
// waited on. This fills the table through the public API alone -- tests
// use public headers only, so the fill loop stops on the first add_timer
// that returns 0 rather than hardcoding AXL_MAX_SOURCES from the internal
// header.
// ---------------------------------------------------------------------------

/* Long enough that it cannot fire before this test (or the harness) is
   done with it -- it only needs to occupy a source-table slot. */
#define ITERATE_FAIL_FILLER_MS    (3600u * 1000u)

/* Comfortably exceeds the real table size (AXL_MAX_SOURCES == 64 today)
   without hardcoding it; fill_source_table stops at the first add that
   returns 0, well short of this array bound. */
#define ITERATE_FAIL_FILLER_MAX   128

static bool
iterate_fail_noop_timer(void *data)
{
    (void)data;
    return AXL_SOURCE_CONTINUE;
}

static size_t
iterate_fail_fill_table(AxlLoop *loop, AxlSourceId *ids, size_t cap)
{
    size_t n = 0;

    while (n < cap) {
        AxlSourceId id = axl_loop_add_timer(loop, ITERATE_FAIL_FILLER_MS,
                                            iterate_fail_noop_timer, NULL);
        if (id == 0) {
            break;
        }
        ids[n++] = id;
    }
    return n;
}

static void
iterate_fail_drain_table(AxlLoop *loop, const AxlSourceId *ids, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        axl_loop_remove_source(loop, ids[i]);
    }
}

static void
test_iterate_until_fails_closed(void)
{
    AxlLoop    *loop;
    AxlEvent   *done;
    AxlSourceId ids[ITERATE_FAIL_FILLER_MAX];
    size_t      filled;
    size_t      refilled;
    AxlStatus   rc;

    loop = axl_loop_new();
    done = axl_event_new();
    if (loop == NULL || done == NULL) {
        test_fail("loop: iterate_until fails closed: alloc failed");
        axl_event_free(done);
        axl_loop_free(loop);
        return;
    }

    filled = iterate_fail_fill_table(loop, ids, ITERATE_FAIL_FILLER_MAX);
    test_check(filled > 0 && filled < ITERATE_FAIL_FILLER_MAX,
               "loop: iterate_until fails closed: source table fillable to capacity via public API");

    rc = axl_loop_iterate_until(loop, NULL, 1000);
    test_check(rc == AXL_NO_RESOURCES,
               "loop: iterate_until fails closed when the timeout source cannot register");

    rc = axl_loop_iterate_until(loop, done, 1000);
    test_check(rc == AXL_NO_RESOURCES,
               "loop: iterate_until fails closed when the done source cannot register");

    iterate_fail_drain_table(loop, ids, filled);

    rc = axl_loop_iterate_until(loop, NULL, 1000);
    test_check(rc == AXL_TIMEOUT,
               "loop: iterate_until times out normally once the table has room again");

    /* If either failed call above had leaked a source (removed the table
       entry it added on the OTHER failure path but not this one, or left
       a partially-registered source behind), the table would refill to a
       SMALLER capacity than the first time. */
    refilled = iterate_fail_fill_table(loop, ids, ITERATE_FAIL_FILLER_MAX);
    test_check(refilled == filled,
               "loop: a failed iterate_until leaks no source (table refills to the same capacity)");
    iterate_fail_drain_table(loop, ids, refilled);

    axl_event_free(done);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Dispatch fairness: sources that stay signalled share the loop round-robin
//
// Each iteration dispatches ONE source. gBS->WaitForEvent returns the
// LOWEST-index signalled event and the non-blocking branch scans from the
// same end, so without a rotating start a source that is ready on every
// iteration (a listener under a connection flood) starves every source
// registered after it: the later sources are dispatched ZERO times. These
// sources re-signal themselves from their own callback, which is exactly
// that shape. With the rotation the order is deterministic -- each wait
// starts just past the source dispatched last -- so the checks pin the
// exact sequence, not merely "everyone got a turn".
// ---------------------------------------------------------------------------

#define FAIR_SOURCES      3u
#define FAIR_DISPATCHES   30u
#define FAIR_ITER_CAP     400u

typedef struct {
    AxlEvent    *event;
    AxlSourceId  id;
    char         tag;
    char        *seq;
    size_t      *seq_len;
    uint32_t     stall_us;
} FairSource;

static bool
fair_on_ready(void *data)
{
    FairSource *fs = data;

    if (*fs->seq_len < FAIR_DISPATCHES) {
        fs->seq[*fs->seq_len] = fs->tag;
    }
    (*fs->seq_len)++;
    if (fs->stall_us > 0) {
        gBS->Stall(fs->stall_us);
    }
    axl_event_signal(fs->event);    /* stay ready: the flood shape */
    return AXL_SOURCE_CONTINUE;
}

/* Registers FAIR_SOURCES self-re-signalling sources on @a loop, all signalled
   up front. Returns false (after reporting) if any allocation fails. */
static bool
fair_setup(
    AxlLoop    *loop,
    FairSource *fs,
    char       *seq,
    size_t     *seq_len,
    uint32_t    stall_us
    )
{
    for (size_t i = 0; i < FAIR_SOURCES; i++) {
        fs[i].event = axl_event_new();
        fs[i].tag = (char)('A' + i);
        fs[i].seq = seq;
        fs[i].seq_len = seq_len;
        fs[i].stall_us = stall_us;
        if (fs[i].event != NULL) {
            fs[i].id = axl_loop_add_event(loop, axl_event_handle(fs[i].event),
                                          fair_on_ready, &fs[i]);
        }
        if (fs[i].id == 0) {
            test_fail("loop: fairness: source setup failed");
            return false;
        }
        axl_event_signal(fs[i].event);
    }
    return true;
}

static void
fair_teardown(
    AxlLoop    *loop,
    FairSource *fs
    )
{
    /* Caller-owned events: remove the sources before freeing the loop,
       and free the events last. */
    for (size_t i = 0; i < FAIR_SOURCES; i++) {
        axl_loop_remove_source(loop, fs[i].id);
    }
    axl_loop_free(loop);
    for (size_t i = 0; i < FAIR_SOURCES; i++) {
        axl_event_free(fs[i].event);
    }
}

static void
test_dispatch_fairness_non_blocking(void)
{
    FairSource fs[FAIR_SOURCES] = {0};
    char       seq[FAIR_DISPATCHES + 1] = {0};
    size_t     seq_len = 0;
    AxlLoop   *loop = axl_loop_new();

    if (loop == NULL) {
        test_fail("loop: fairness non-blocking: loop_new failed");
        return;
    }
    if (!fair_setup(loop, fs, seq, &seq_len, 0)) {
        fair_teardown(loop, fs);
        return;
    }

    /* The non-blocking branch is also what a driver-mode tick drains
       (driver_dispatch_notify loops axl_loop_dispatch(loop, false)). */
    for (size_t i = 0; i < FAIR_DISPATCHES; i++) {
        axl_loop_dispatch(loop, false);
    }

    test_check(seq_len == FAIR_DISPATCHES,
               "loop: fairness non-blocking: every call dispatched a ready source");
    test_check(axl_strcmp(seq, "ABCABCABCABCABCABCABCABCABCABC") == 0,
               "loop: fairness non-blocking: always-ready sources dispatch round-robin");

    fair_teardown(loop, fs);
}

static void
test_dispatch_fairness_blocking(void)
{
    FairSource fs[FAIR_SOURCES] = {0};
    char       seq[FAIR_DISPATCHES + 1] = {0};
    size_t     seq_len = 0;
    size_t     intrinsic_wins = 0;
    AxlLoop   *loop = axl_loop_new();

    if (loop == NULL) {
        test_fail("loop: fairness blocking: loop_new failed");
        return;
    }
    /* 1 ms per callback: 30 dispatches span ~3 of the loop's 10 ms poll
       periods, so the poll timer is signalled while the sources are. */
    if (!fair_setup(loop, fs, seq, &seq_len, 1000)) {
        fair_teardown(loop, fs);
        return;
    }

    for (size_t i = 0; i < FAIR_ITER_CAP && seq_len < FAIR_DISPATCHES; i++) {
        int rc = axl_loop_dispatch(loop, true);
        if (rc == 1) {
            intrinsic_wins++;   /* poll-timer (or console-key) wakeup */
        } else if (rc != 0) {
            break;
        }
    }

    test_check(seq_len == FAIR_DISPATCHES,
               "loop: fairness blocking: the wait kept dispatching ready sources");
    test_check(axl_strcmp(seq, "ABCABCABCABCABCABCABCABCABCABC") == 0,
               "loop: fairness blocking: always-ready sources dispatch round-robin");
    /* The loop's own events sit AFTER the user sources in the wait array.
       Without the rotation they are never reached while a user source is
       ready, which strands a keypress source (drained on the poll tick) and
       the serial-Ctrl-C intrinsic behind any always-ready source. */
    test_check(intrinsic_wins > 0,
               "loop: fairness blocking: the poll timer still gets a turn under saturated sources");

    fair_teardown(loop, fs);
}

// ---------------------------------------------------------------------------

int
test_loop_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlLoop");

    test_timer_count();
    test_timeout();
    test_idle_count();
    test_quit_from_callback();
    test_cleanup();
    test_dispatch_non_blocking();
    test_next_dispatch_separate();
    test_remove_source();
    test_timer_idle_interaction();
    test_timeout_cancellation();
    test_multiple_timers();
    test_raw_event_source();
    test_defer_basic();
    test_defer_cancel();
    test_defer_fifo();
    test_defer_reentrant();
    test_defer_chain_next_tick();
    test_defer_null_fn();
    test_pubsub_basic();
    test_pubsub_multi_sub();
    test_pubsub_disconnect();
    test_pubsub_emit_unknown();
    test_pubsub_auto_create();
    test_pubsub_user_data();
    test_pubsub_reset();
    test_iterate_until_done();
    test_iterate_until_timeout();
    test_loop_attach_driver();
    test_loop_driver_budget();
    test_loop_idle_verdict();
    test_loop_driver_budget_idle_foreground();
    test_loop_attach_driver_idle_member_fails();
    test_keypress_drain_non_blocking();
    test_loop_wait_at_raised_tpl();
    test_loop_sync_wait_from_driver_pump();
    test_loop_in_callback_marker();
    test_intercept_break_flag();
    test_source_ids_globally_unique();
    test_iterate_until_fails_closed();
    test_dispatch_fairness_non_blocking();
    test_dispatch_fairness_blocking();

    return test_print_results();
}

AXL_APP(test_loop_main)
