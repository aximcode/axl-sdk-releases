/** @file axl-test-loop.c
    Test application for AxlLoop, AxlDefer, and AxlPubsub.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-wait.h>
#include <uefi/axl-uefi.h>

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

    test_check(ctx.iter_rc == 0, "loop: iterate_until returns 0 on done");
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

    test_check(ctx.iter_rc == -1, "loop: iterate_until returns -1 on timeout");

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
    test_loop_wait_at_raised_tpl();
    test_loop_sync_wait_from_driver_pump();
    test_loop_in_callback_marker();
    test_intercept_break_flag();
    test_source_ids_globally_unique();

    return test_print_results();
}

AXL_APP(test_loop_main)
