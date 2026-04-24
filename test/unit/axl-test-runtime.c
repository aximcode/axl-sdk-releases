/** @file axl-test-runtime.c
    Unit tests for the runtime module: axl_atexit, axl_yield,
    axl_loop_default, axl_registry_count, axl_interrupted.

    axl_exit itself calls gBS->Exit and can't be unit-tested from
    within a test binary -- integration coverage lives in
    sdk/examples/runtime-demo.c.
**/

#include "axl-test.h"

#include <axl/axl-atexit.h>
#include <axl/axl-cancellable.h>
#include <axl/axl-event.h>
#include <axl/axl-loop.h>
#include <axl/axl-runtime.h>
#include <axl/axl-signal.h>
#include <axl/axl-task.h>

// ---------------------------------------------------------------------------
// atexit: LIFO ordering
// ---------------------------------------------------------------------------

static int atexit_log[8];
static size_t atexit_log_len;

static void
atexit_log_push(int label)
{
    if (atexit_log_len < sizeof (atexit_log) / sizeof (atexit_log[0])) {
        atexit_log[atexit_log_len++] = label;
    }
}

static void atexit_cb_a(void *data) { (void)data; atexit_log_push(1); }
static void atexit_cb_b(void *data) { (void)data; atexit_log_push(2); }
static void atexit_cb_c(void *data) { (void)data; atexit_log_push(3); }

/* Private drain -- the real _axl_atexit_run_all is internal, so we
 * mimic its effect by registering + letting _axl_cleanup handle it
 * at test exit. For LIFO verification within a test, we register
 * the callbacks and then inspect via observable side effects. The
 * actual LIFO drain is covered by runtime-demo scenario 2. */
static void
test_atexit_handle_nonzero(void)
{
    uint32_t h = axl_atexit(atexit_cb_a, NULL);
    test_check(h != 0, "atexit: register returns non-zero handle");
    axl_atexit_remove(h);
}

static void
test_atexit_null_fn_fails(void)
{
    test_check(axl_atexit(NULL, NULL) == 0,
               "atexit: NULL fn returns 0");
}

static void
test_atexit_remove_is_idempotent(void)
{
    uint32_t h = axl_atexit(atexit_cb_b, NULL);
    axl_atexit_remove(h);
    axl_atexit_remove(h);           /* safe */
    axl_atexit_remove(0);           /* safe */
    axl_atexit_remove(999999);      /* safe: out-of-range */
    test_pass("atexit: remove is idempotent + bounds-safe");
}

static void
test_atexit_handles_unique_per_register(void)
{
    uint32_t h1 = axl_atexit(atexit_cb_a, NULL);
    uint32_t h2 = axl_atexit(atexit_cb_b, NULL);
    uint32_t h3 = axl_atexit(atexit_cb_c, NULL);
    test_check(h1 != h2 && h2 != h3 && h1 != h3,
               "atexit: consecutive registers return distinct handles");
    axl_atexit_remove(h1);
    axl_atexit_remove(h2);
    axl_atexit_remove(h3);
}

// ---------------------------------------------------------------------------
// registry counting
// ---------------------------------------------------------------------------

static void
test_registry_count_tracks_events(void)
{
    size_t before = axl_registry_count();

    AxlEvent *e1 = axl_event_new();
    test_check(axl_registry_count() == before + 1,
               "registry: event alloc bumps count");

    AxlEvent *e2 = axl_event_new();
    test_check(axl_registry_count() == before + 2,
               "registry: second event alloc bumps count");

    axl_event_free(e1);
    test_check(axl_registry_count() == before + 1,
               "registry: free drops count");

    axl_event_free(e2);
    test_check(axl_registry_count() == before,
               "registry: count returns to baseline");
}

static void
test_registry_count_tracks_mixed_kinds(void)
{
    size_t before = axl_registry_count();

    AxlEvent       *e = axl_event_new();
    AxlLoop        *l = axl_loop_new();
    AxlCancellable *c = axl_cancellable_new();
    AxlArena       *a = axl_arena_new(1024);

    /* cancellable internally allocates an event, so +1 extra. */
    test_check(axl_registry_count() == before + 5,
               "registry: mixed-kind alloc counts each resource (incl. "
               "cancellable's inner event)");

    axl_arena_free(a);
    axl_cancellable_free(c);
    axl_loop_free(l);
    axl_event_free(e);

    test_check(axl_registry_count() == before,
               "registry: mixed-kind free returns to baseline");
}

// ---------------------------------------------------------------------------
// default loop + yield
// ---------------------------------------------------------------------------

static int yield_tick_count;

static bool
yield_tick_cb(void *data)
{
    (void)data;
    yield_tick_count++;
    return AXL_SOURCE_REMOVE;
}

static void
test_loop_default_is_stable(void)
{
    AxlLoop *l1 = axl_loop_default();
    AxlLoop *l2 = axl_loop_default();
    test_check(l1 != NULL, "loop_default: returns non-NULL");
    test_check(l1 == l2,   "loop_default: returns the same singleton");
}

static void
test_yield_dispatches_ready_work(void)
{
    AxlLoop *loop = axl_loop_default();
    if (loop == NULL) {
        test_fail("yield: loop_default NULL");
        return;
    }

    yield_tick_count = 0;

    /* Idle sources fire on every loop iteration (including the
     * non-blocking iteration that axl_yield performs). Return
     * AXL_SOURCE_REMOVE so this is a one-shot. */
    axl_loop_add_idle(loop, yield_tick_cb, NULL);

    axl_yield();

    test_check(yield_tick_count == 1,
               "yield: dispatches pending idle work in one pass");
}

// ---------------------------------------------------------------------------
// axl_interrupted: false until break observed
// ---------------------------------------------------------------------------

static void
test_interrupted_is_false_at_startup(void)
{
    test_check(axl_interrupted() == false,
               "interrupted: false before any Ctrl-C");
}

// ---------------------------------------------------------------------------
// signal_install / default: no-op fast path (handler runtime behavior
// is covered by runtime-demo scenario 1, which actually presses Ctrl-C)
// ---------------------------------------------------------------------------

static int sig_handler_calls;
static void on_signal_test(void) { sig_handler_calls++; }

static void
test_signal_install_accepts_handler_and_null(void)
{
    axl_signal_install(on_signal_test);
    axl_signal_install(NULL);
    axl_signal_default();
    test_pass("signal: install(fn) / install(NULL) / default() are safe");
    test_check(sig_handler_calls == 0,
               "signal: install does not invoke the handler");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_runtime_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlRuntime (atexit + registry + yield + signal)");

    test_atexit_handle_nonzero();
    test_atexit_null_fn_fails();
    test_atexit_remove_is_idempotent();
    test_atexit_handles_unique_per_register();

    test_registry_count_tracks_events();
    test_registry_count_tracks_mixed_kinds();

    test_loop_default_is_stable();
    test_yield_dispatches_ready_work();

    test_interrupted_is_false_at_startup();

    test_signal_install_accepts_handler_and_null();

    return test_print_results();
}

AXL_APP(test_runtime_main)
