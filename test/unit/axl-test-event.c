/** @file axl-test-event.c
    Unit tests for the event module: AxlEvent (the foundational one-
    shot latch), AxlCancellable (typed stop-token wrapper), and the
    axl_wait_* helpers. Merges what was previously split across
    axl-test-completion.c and axl-test-cancellable.c.
**/

#include "axl-test.h"

#include <axl/axl-cancellable.h>
#include <axl/axl-event.h>
#include <axl/axl-log.h>
#include <axl/axl-wait.h>

#include <axl/axl-str.h>   /* axl_strstr */

#include "axl-backend.h"   /* axl_backend_event_{create,close} — the
                              double-close guard has no public call site */

AXL_LOG_DOMAIN("test");

// ---------------------------------------------------------------------------
// Shared fixtures
// ---------------------------------------------------------------------------

static volatile bool     g_flag;
static volatile uint64_t g_word;
static size_t            g_tick_count;

// ---------------------------------------------------------------------------
// AxlEvent
// ---------------------------------------------------------------------------

static void
test_event_new_and_free(void)
{
    AxlEvent *e = axl_event_new();
    test_check(e != NULL, "event: new returns non-NULL");
    axl_event_free(e);
    test_pass("event: free succeeds");

    /* NULL-safety */
    axl_event_free(NULL);
    axl_event_signal(NULL);
    axl_event_reset(NULL);
    test_check(axl_event_is_set(NULL) == false,
               "event: is_set(NULL) returns false");
    test_check(axl_event_handle(NULL) == NULL,
               "event: handle(NULL) returns NULL");
    test_pass("event: NULL ops are no-ops");
}

static void
test_event_fresh_is_not_set(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: fresh: new failed");
        return;
    }
    test_check(axl_event_is_set(e) == false,
               "event: fresh reports not-set");
    axl_event_free(e);
}

static void
test_event_signal_sets_flag(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: signal: new failed");
        return;
    }
    axl_event_signal(e);
    test_check(axl_event_is_set(e) == true,
               "event: after signal reports set");
    axl_event_free(e);
}

static void
test_event_handle_is_stable(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: handle: new failed");
        return;
    }
    AxlEventHandle h1 = axl_event_handle(e);
    AxlEventHandle h2 = axl_event_handle(e);
    test_check(h1 != NULL, "event: handle is non-NULL");
    test_check(h1 == h2, "event: handle is stable across calls");
    axl_event_free(e);
}

static void
test_event_signal_before_wait(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: signal-before-wait: new failed");
        return;
    }

    axl_event_signal(e);
    test_check(axl_event_wait_timeout(e, NULL, 1000000) == AXL_OK,
               "event: signal-before-wait returns AXL_OK");
    axl_event_free(e);
}

static void
test_event_reset_and_reuse(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: reset-reuse: new failed");
        return;
    }

    /* First cycle: signal then wait. The successful wait itself
       clears is_set (because the loop's SOURCE_EVENT dispatch
       consumed the signal via CheckEvent). */
    axl_event_signal(e);
    test_check(axl_event_is_set(e) == true,
               "event: is_set true after signal");
    test_check(axl_event_wait_timeout(e, NULL, 1000000) == AXL_OK,
               "event: first wait returns AXL_OK");
    test_check(axl_event_is_set(e) == false,
               "event: successful wait clears is_set");

    /* Explicit reset is also safe and idempotent. */
    axl_event_reset(e);
    test_check(axl_event_is_set(e) == false,
               "event: reset keeps is_set false");

    /* Second cycle -- the event is reusable after being consumed. */
    axl_event_signal(e);
    test_check(axl_event_wait_timeout(e, NULL, 1000000) == AXL_OK,
               "event: second wait returns AXL_OK");
    test_check(axl_event_is_set(e) == false,
               "event: is_set cleared after second wait");

    axl_event_free(e);
}

/*
 * Cancellation path must NOT clear is_set -- the event never fired,
 * the cancel did. Callers who signal-then-cancel-then-wait should
 * still see is_set=true after the wait returns AXL_CANCELLED.
 */
static void
test_event_cancelled_wait_preserves_is_set(void)
{
    AxlEvent       *e = axl_event_new();
    AxlCancellable *x = axl_cancellable_new();
    if (e == NULL || x == NULL) {
        test_fail("event+cancel: new failed");
        axl_cancellable_free(x);
        axl_event_free(e);
        return;
    }

    axl_event_signal(e);
    axl_cancellable_cancel(x);
    /* Pre-cancelled -- wait returns AXL_CANCELLED via fast path. */
    test_check(axl_event_wait_timeout(e, x, 1000000) == AXL_CANCELLED,
               "event: wait returns AXL_CANCELLED when cancel wins");
    test_check(axl_event_is_set(e) == true,
               "event: is_set preserved when wait cancelled");

    axl_cancellable_free(x);
    axl_event_free(e);
}

/*
 * Timeout path also must not clear is_set -- the event never fired.
 * (Unsignalled event + short timeout -> rc=-1, is_set stays false.)
 */
static void
test_event_timed_out_wait_preserves_is_set(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: timeout-preserves-is-set: new failed");
        return;
    }

    /* Event is unsignalled; short timeout. is_set stays false. */
    test_check(axl_event_wait_timeout(e, NULL, 20000) == AXL_TIMEOUT,
               "event: wait returns AXL_TIMEOUT on deadline");
    test_check(axl_event_is_set(e) == false,
               "event: is_set still false after timeout");

    axl_event_free(e);
}

static void
test_event_reset_drops_pending(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: reset-pending: new failed");
        return;
    }

    axl_event_signal(e);
    axl_event_reset(e);
    test_check(axl_event_is_set(e) == false,
               "event: reset drops is_set");
    test_check(axl_event_wait_timeout(e, NULL, 10000) == AXL_TIMEOUT,
               "event: reset drops pending signal (wait times out)");

    axl_event_free(e);
}

static void
test_event_timeout(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: timeout: new failed");
        return;
    }

    test_check(axl_event_wait_timeout(e, NULL, 20000) == AXL_TIMEOUT,
               "event: unsignalled wait times out");

    axl_event_free(e);
}

/*
 * Regression test for the AXL_TIMEOUT / AXL_ERR disambiguation
 * introduced with AxlStatus. Pre-AxlStatus, both timeout and
 * invalid-arg returned -1, so callers couldn't distinguish "deadline
 * elapsed" from "you passed garbage". A NULL event must yield
 * AXL_ERR; a valid-but-unsignalled event with a short timeout must
 * yield AXL_TIMEOUT — distinct values, distinct meaning.
 */
static void
test_event_timeout_distinct_from_error(void)
{
    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        test_fail("event: status-disambig: new failed");
        return;
    }

    AxlStatus terr  = axl_event_wait_timeout(NULL, NULL, 20000);
    AxlStatus tout  = axl_event_wait_timeout(e, NULL, 20000);

    test_check(terr == AXL_ERR,
               "event: NULL event returns AXL_ERR (-1)");
    test_check(tout == AXL_TIMEOUT,
               "event: deadline returns AXL_TIMEOUT (-3)");
    test_check(terr != tout,
               "event: AXL_ERR and AXL_TIMEOUT are distinct");

    axl_event_free(e);
}

// ---------------------------------------------------------------------------
// axl_wait_for_flag
// ---------------------------------------------------------------------------

static void
test_wait_for_flag_already_true(void)
{
    g_flag = true;
    test_check(axl_wait_for_flag(&g_flag, NULL, 1000000) == AXL_OK,
               "wait_for_flag: already-true returns AXL_OK immediately");
    g_flag = false;
}

static void
test_wait_for_flag_timeout(void)
{
    g_flag = false;
    test_check(axl_wait_for_flag(&g_flag, NULL, 20000) == AXL_TIMEOUT,
               "wait_for_flag: never-set returns AXL_TIMEOUT");
}

static void
test_wait_for_flag_null(void)
{
    test_check(axl_wait_for_flag(NULL, NULL, 1000) == AXL_ERR,
               "wait_for_flag: NULL flag returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_wait_for_word
// ---------------------------------------------------------------------------

static void
test_wait_for_word_already_ready(void)
{
    g_word = 0xCAFEBABEULL;
    test_check(axl_wait_for_word(&g_word, 0, NULL, 1000000) == AXL_OK,
               "wait_for_word: non-not-ready returns AXL_OK immediately");
}

static void
test_wait_for_word_timeout(void)
{
    g_word = 0;
    test_check(axl_wait_for_word(&g_word, 0, NULL, 20000) == AXL_TIMEOUT,
               "wait_for_word: never-changed returns AXL_TIMEOUT");
}

static void
test_wait_for_word_null(void)
{
    test_check(axl_wait_for_word(NULL, 0, NULL, 1000) == AXL_ERR,
               "wait_for_word: NULL word returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_wait_ms
// ---------------------------------------------------------------------------

static void
test_wait_ms_zero(void)
{
    test_check(axl_wait_ms(NULL, 0) == AXL_OK,
               "wait_ms: zero returns AXL_OK immediately");
}

static void
test_wait_ms_short(void)
{
    /* Should return AXL_OK after ~10ms, no busy-wait. */
    test_check(axl_wait_ms(NULL, 10) == AXL_OK,
               "wait_ms: 10ms elapsed returns AXL_OK");
}

// ---------------------------------------------------------------------------
// axl_wait_for_with_tick -- mock state machine
// ---------------------------------------------------------------------------

typedef struct {
    size_t  step;
    size_t  target;
} MockSm;

static bool
mock_is_done(void *ctx)
{
    MockSm *sm = (MockSm *)ctx;
    return sm->step >= sm->target;
}

static void
mock_advance(void *ctx)
{
    MockSm *sm = (MockSm *)ctx;
    sm->step++;
    g_tick_count++;
}

static void
test_wait_for_with_tick_advances(void)
{
    MockSm    sm;
    AxlStatus rc;

    sm.step = 0;
    sm.target = 3;
    g_tick_count = 0;

    /* Tick every 2ms, timeout at 500ms (generous). */
    rc = axl_wait_for_with_tick(mock_is_done, &sm,
                                mock_advance, &sm,
                                2000, NULL, 500000);
    test_check(rc == AXL_OK, "wait_for_with_tick: state machine completes");
    test_check(sm.step >= 3, "wait_for_with_tick: advanced past target");
    test_check(g_tick_count >= 3, "wait_for_with_tick: tick fired");
}

static void
test_wait_for_null_cond(void)
{
    test_check(axl_wait_for(NULL, NULL, NULL, 1000) == AXL_ERR,
               "wait_for: NULL cond returns AXL_ERR");
    test_check(axl_wait_for_with_tick(NULL, NULL, NULL, NULL, 1000,
                                      NULL, 1000) == AXL_ERR,
               "wait_for_with_tick: NULL cond returns AXL_ERR");
}

/*
 * Double-free / UAF detection. Mirrors the AxlCancellable UAF test.
 * First free leaves AXL_EVENT_DEAD in the struct's magic; subsequent
 * ops see the dead magic, log an error, and return without
 * scribbling over freed memory.
 */
static AxlEvent *
alloc_event_and_free_then_reuse_slot(void)
{
    AxlEvent *e = axl_event_new();
    axl_event_free(e);
    return e;
}

static void
test_event_use_after_free_detection(void)
{
    AxlEvent *stale = alloc_event_and_free_then_reuse_slot();

    axl_event_signal(stale);
    axl_event_reset(stale);
    test_check(axl_event_is_set(stale) == false,
               "event: use-after-free reports !is_set");
    test_check(axl_event_handle(stale) == NULL,
               "event: use-after-free returns NULL handle");
    test_check(axl_event_wait_timeout(stale, NULL, 1000) == AXL_ERR,
               "event: use-after-free wait returns AXL_ERR");
    axl_event_free(stale);  /* double-free -- also caught */
    test_pass("event: use-after-free ops don't crash");
}

// ---------------------------------------------------------------------------
// Backend double-close guard
//
// axl_backend_event_close keeps a ring of recently-closed handles and REFUSES
// a second close of the same one. That is not a diagnostic: DxeCore's
// CoreCloseEvent #GPs on a stale handle, so the refusal is what keeps the
// machine up. It had NO test -- it is reached only from internal teardown
// paths, so no public call site drives it. These two are the safety net for
// its storage layout.
//
// Observed through the LOG RING rather than as "it did not crash": the guard
// announces each refusal, so the count of DOUBLE-CLOSE lines is a value.
// ---------------------------------------------------------------------------

/* Count log entries whose message carries @a needle. */
static size_t
count_log_matches(AxlLogRing *ring, const char *needle)
{
    AxlLogEntry entry;
    size_t      n     = 0;
    size_t      total = axl_log_ring_count(ring);

    for (size_t i = 0; i < total; i++) {
        if (axl_log_ring_get(ring, i, &entry)
            && axl_strstr(entry.message, needle) != NULL) {
            n++;
        }
    }
    return n;
}

static void
test_event_backend_double_close_is_refused(void)
{
    AxlLogRing     *ring = axl_log_ring_new(32, 256);
    AxlEventHandle  ev   = NULL;

    test_check(ring != NULL, "double-close: log ring allocated");
    if (ring == NULL) {
        return;
    }
    axl_log_ring_attach(ring);

    test_check(axl_backend_event_create(&ev) == AXL_OK && ev != NULL,
               "double-close: backend event created");
    if (ev == NULL) {
        axl_log_ring_attach(NULL);
        axl_log_ring_free(ring);
        return;
    }

    /* The first close is real, and must announce nothing. */
    axl_backend_event_close(ev);
    test_check(count_log_matches(ring, "DOUBLE-CLOSE") == 0,
               "double-close: the first close announces nothing");

    /* The second close of the SAME handle must be refused rather than passed
       to the firmware. Reaching CoreCloseEvent here is the #GP the ring exists
       to prevent, so surviving this line is part of the assertion. */
    axl_backend_event_close(ev);
    test_check(count_log_matches(ring, "DOUBLE-CLOSE") == 1,
               "double-close: the second close is refused, exactly once");

    /* A third, to prove the record is not consumed by the first refusal. */
    axl_backend_event_close(ev);
    test_check(count_log_matches(ring, "DOUBLE-CLOSE") == 2,
               "double-close: still refused on a third attempt");

    axl_log_ring_attach(NULL);
    axl_log_ring_free(ring);
}

static void
test_event_recycled_handle_closes_normally(void)
{
    AxlLogRing     *ring  = axl_log_ring_new(32, 256);
    AxlEventHandle  first = NULL;
    AxlEventHandle  again = NULL;

    test_check(ring != NULL, "recycled: log ring allocated");
    if (ring == NULL) {
        return;
    }

    /* Close one, then create another. The firmware commonly hands back the
       SAME address, and the ring's create-side hook must clear the stale
       record -- otherwise the new event's legitimate close is refused as a
       false double-close and the handle leaks for the rest of the boot. */
    test_check(axl_backend_event_create(&first) == AXL_OK && first != NULL,
               "recycled: first event created");
    axl_backend_event_close(first);

    test_check(axl_backend_event_create(&again) == AXL_OK && again != NULL,
               "recycled: second event created");

    axl_log_ring_attach(ring);
    axl_backend_event_close(again);
    test_check(count_log_matches(ring, "DOUBLE-CLOSE") == 0,
               "recycled: a re-created handle closes without a false refusal");
    axl_log_ring_attach(NULL);

    /* That assertion only bites when the address really was reused. Say so
       when it was not, so a silent pass cannot masquerade as coverage on
       firmware that never recycles. */
    if (first != again) {
        axl_debug("recycled: firmware did not reuse the address "
                  "(%p vs %p) -- the clear-on-create path was not exercised",
                  (void *)first, (void *)again);
    }
    axl_log_ring_free(ring);
}

// ---------------------------------------------------------------------------
// AxlCancellable lifecycle + state
// ---------------------------------------------------------------------------

static void
test_cancellable_new_and_free(void)
{
    AxlCancellable *c = axl_cancellable_new();
    test_check(c != NULL, "cancellable: new returns non-NULL");
    axl_cancellable_free(c);
    test_pass("cancellable: free succeeds");

    axl_cancellable_free(NULL);
    axl_cancellable_cancel(NULL);
    axl_cancellable_reset(NULL);
    test_check(axl_cancellable_is_cancelled(NULL) == false,
               "cancellable: is_cancelled(NULL) returns false");
    test_pass("cancellable: NULL ops are no-ops");
}

static void
test_cancellable_fresh_is_not_cancelled(void)
{
    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        test_fail("cancellable: fresh: new failed");
        return;
    }
    test_check(axl_cancellable_is_cancelled(c) == false,
               "cancellable: fresh reports not-cancelled");
    axl_cancellable_free(c);
}

static void
test_cancellable_cancel_sets_flag(void)
{
    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        test_fail("cancellable: cancel: new failed");
        return;
    }
    axl_cancellable_cancel(c);
    test_check(axl_cancellable_is_cancelled(c) == true,
               "cancellable: after cancel reports cancelled");
    axl_cancellable_free(c);
}

static void
test_cancellable_cancel_is_idempotent(void)
{
    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        test_fail("cancellable: idempotent: new failed");
        return;
    }
    axl_cancellable_cancel(c);
    axl_cancellable_cancel(c);
    axl_cancellable_cancel(c);
    test_check(axl_cancellable_is_cancelled(c) == true,
               "cancellable: triple-cancel stays cancelled");
    axl_cancellable_free(c);
}

static void
test_cancellable_reset_clears_flag(void)
{
    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        test_fail("cancellable: reset: new failed");
        return;
    }
    axl_cancellable_cancel(c);
    test_check(axl_cancellable_is_cancelled(c) == true,
               "cancellable: reset: cancelled before reset");

    axl_cancellable_reset(c);
    test_check(axl_cancellable_is_cancelled(c) == false,
               "cancellable: reset clears cancelled flag");

    axl_cancellable_cancel(c);
    test_check(axl_cancellable_is_cancelled(c) == true,
               "cancellable: re-cancel after reset works");
    axl_cancellable_free(c);
}

static void
test_cancellable_reset_without_cancel_is_safe(void)
{
    AxlCancellable *c = axl_cancellable_new();
    if (c == NULL) {
        test_fail("cancellable: reset-fresh: new failed");
        return;
    }
    axl_cancellable_reset(c);
    test_check(axl_cancellable_is_cancelled(c) == false,
               "cancellable: reset on fresh stays not-cancelled");
    axl_cancellable_free(c);
}

/*
 * Double-free / UAF detection. The first free leaves
 * AXL_CANCELLABLE_DEAD in the struct's magic field; subsequent ops
 * see the dead magic, log an error, and return without scribbling
 * over already-freed memory.
 */
static AxlCancellable *
alloc_and_free_then_reuse_slot(void)
{
    AxlCancellable *c = axl_cancellable_new();
    axl_cancellable_free(c);
    return c;
}

static void
test_cancellable_use_after_free_detection(void)
{
    AxlCancellable *stale = alloc_and_free_then_reuse_slot();

    axl_cancellable_cancel(stale);
    axl_cancellable_reset(stale);
    test_check(axl_cancellable_is_cancelled(stale) == false,
               "cancellable: use-after-free reports !is_cancelled");
    axl_cancellable_free(stale);  /* double-free -- also caught */
    test_pass("cancellable: use-after-free ops don't crash");
}

// ---------------------------------------------------------------------------
// Cancellable integration with event waits
// ---------------------------------------------------------------------------

static void
test_event_wait_precancelled(void)
{
    AxlEvent       *e = axl_event_new();
    AxlCancellable *x = axl_cancellable_new();
    if (e == NULL || x == NULL) {
        test_fail("event+cancel: new failed");
        axl_cancellable_free(x);
        axl_event_free(e);
        return;
    }

    axl_cancellable_cancel(x);
    test_check(axl_event_wait_timeout(e, x, 1000000) == AXL_CANCELLED,
               "event: pre-cancelled wait returns AXL_CANCELLED");

    axl_cancellable_free(x);
    axl_event_free(e);
}

static void
test_wait_ms_precancelled(void)
{
    AxlCancellable *x = axl_cancellable_new();
    if (x == NULL) {
        test_fail("wait_ms+cancel: new failed");
        return;
    }
    axl_cancellable_cancel(x);
    test_check(axl_wait_ms(x, 1000) == AXL_CANCELLED,
               "wait_ms: pre-cancelled returns AXL_CANCELLED");
    axl_cancellable_free(x);
}

static void
test_wait_for_flag_precancelled(void)
{
    AxlCancellable *x = axl_cancellable_new();
    volatile bool   flag = false;
    if (x == NULL) {
        test_fail("wait_for_flag+cancel: new failed");
        return;
    }
    axl_cancellable_cancel(x);
    test_check(axl_wait_for_flag(&flag, x, 1000000) == AXL_CANCELLED,
               "wait_for_flag: pre-cancelled returns AXL_CANCELLED");
    axl_cancellable_free(x);
}

/*
 * Mid-wait cancellation: tick_fn signals the cancellable on the
 * third tick; the cancellable's event source fires on the next
 * loop iteration and the primitive returns AXL_CANCELLED.
 */
typedef struct {
    AxlCancellable *cancel;
    size_t          ticks;
} MidCancelCtx;

static bool
mid_cancel_cond_false(void *ctx)
{
    (void)ctx;
    return false;
}

static void
mid_cancel_tick(void *ctx)
{
    MidCancelCtx *c = (MidCancelCtx *)ctx;
    c->ticks++;
    if (c->ticks >= 3) {
        axl_cancellable_cancel(c->cancel);
    }
}

static void
test_wait_for_with_tick_mid_cancel(void)
{
    AxlCancellable *x = axl_cancellable_new();
    if (x == NULL) {
        test_fail("with_tick+mid-cancel: new failed");
        return;
    }

    MidCancelCtx ctx = { x, 0 };
    AxlStatus rc = axl_wait_for_with_tick(mid_cancel_cond_false, &ctx,
                                          mid_cancel_tick, &ctx,
                                          5000, x, 500000);
    test_check(rc == AXL_CANCELLED,
               "wait_for_with_tick: mid-wait cancel returns AXL_CANCELLED");
    test_check(ctx.ticks >= 3,
               "wait_for_with_tick: tick fired at least 3 times before cancel");
    axl_cancellable_free(x);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_event_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlEvent + AxlCancellable + AxlWait");

    /* AxlEvent */
    test_event_new_and_free();
    test_event_fresh_is_not_set();
    test_event_signal_sets_flag();
    test_event_handle_is_stable();
    test_event_signal_before_wait();
    test_event_reset_and_reuse();
    test_event_reset_drops_pending();
    test_event_timeout();
    test_event_timeout_distinct_from_error();
    test_event_cancelled_wait_preserves_is_set();
    test_event_timed_out_wait_preserves_is_set();
    test_event_use_after_free_detection();
    test_event_backend_double_close_is_refused();
    test_event_recycled_handle_closes_normally();

    /* axl_wait_* */
    test_wait_for_flag_already_true();
    test_wait_for_flag_timeout();
    test_wait_for_flag_null();

    test_wait_for_word_already_ready();
    test_wait_for_word_timeout();
    test_wait_for_word_null();

    test_wait_ms_zero();
    test_wait_ms_short();

    test_wait_for_with_tick_advances();
    test_wait_for_null_cond();

    /* AxlCancellable */
    test_cancellable_new_and_free();
    test_cancellable_fresh_is_not_cancelled();
    test_cancellable_cancel_sets_flag();
    test_cancellable_cancel_is_idempotent();
    test_cancellable_reset_clears_flag();
    test_cancellable_reset_without_cancel_is_safe();
    test_cancellable_use_after_free_detection();

    /* Integration */
    test_event_wait_precancelled();
    test_wait_ms_precancelled();
    test_wait_for_flag_precancelled();
    test_wait_for_with_tick_mid_cancel();

    return test_print_results();
}

AXL_APP(test_event_main)
