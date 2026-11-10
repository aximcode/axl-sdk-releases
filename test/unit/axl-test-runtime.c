/** @file axl-test-runtime.c
    Unit tests for the runtime module: axl_atexit, axl_yield,
    axl_loop_default, axl_registry_count, axl_interrupted.

    axl_exit itself calls gBS->Exit and can't be unit-tested from
    within a test binary -- integration coverage lives in
    sdk/examples/runtime-demo.c.
**/

#include "axl-test.h"

#include <axl/axl-args.h>
#include <axl/axl-atexit.h>
#include <axl/axl-cancellable.h>
#include <axl/axl-console.h>
#include <axl/axl-event.h>
#include <axl/axl-loop.h>
#include <axl/axl-runtime.h>
#include <axl/axl-signal.h>
#include <axl/axl-sort.h>
#include <axl/axl-task.h>

/* Internal backend header (test build passes -Isrc/backend): the exit-status
 * resolver + disarm are internal, not public API. */
#include "axl-backend.h"

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

/* A registration the table cannot hold returns 0 -- never a handle that is
 * someone else's. axl_atexit ignored axl_array_append's failure and returned
 * the table length, which then named the LAST registered slot: removing that
 * "handle" would have unregistered another caller's handler, and a caller
 * checking for 0 (AXL_SERVICE_DRIVER does) could not see the failure.
 *
 * Every registration here runs with the next allocation failing, so the ones
 * that need the table to grow must fail; the ones that land in existing
 * capacity succeed. 64 is past any initial capacity, so both happen. */
static void
test_atexit_full_table_returns_zero(void)
{
    uint32_t got[64];
    unsigned failed = 0;
    bool     unique = true;

    for (unsigned i = 0; i < 64; i++) {
        axl_mem_fail_next_alloc(1);
        got[i] = axl_atexit(atexit_cb_a, NULL);
        axl_mem_fail_next_alloc(0);
        if (got[i] == 0) {
            failed++;
            continue;
        }
        for (unsigned j = 0; j < i; j++) {
            if (got[j] == got[i]) {
                unique = false;
            }
        }
    }
    test_check(failed > 0,
               "atexit: a registration that needs the table to grow can fail");
    test_check(unique,
               "atexit: a failed registration returns 0, never another slot's handle");
    for (unsigned i = 0; i < 64; i++) {
        axl_atexit_remove(got[i]);
    }
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

// Pseudo-random, definitely-unsorted fill so introsort does ~n log n
// comparisons (> 65536) and trips axl_qsort's internal yield several times.
static int g_yield_sort_arr[20000];

static int
yield_int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

// Library code that yields for Ctrl-C responsiveness must NOT re-dispatch the
// consumer's default loop (it observes the break flag via _axl_poll_break,
// never re-entering the loop). The public axl_yield() keeps its documented
// yield-as-scheduler behavior. axl_qsort over a large array trips its internal
// yield many times; if that still dispatched, the registered idle would fire.
static void
test_library_yield_does_not_dispatch(void)
{
    AxlLoop *loop = axl_loop_default();
    if (loop == NULL) {
        test_fail("lib-yield: loop_default NULL");
        return;
    }

    const size_t n = sizeof(g_yield_sort_arr) / sizeof(g_yield_sort_arr[0]);
    for (size_t i = 0; i < n; i++) {
        g_yield_sort_arr[i] = (int)((unsigned)i * 2654435761u);
    }

    yield_tick_count = 0;
    axl_loop_add_idle(loop, yield_tick_cb, NULL);   /* one-shot idle */

    axl_qsort(g_yield_sort_arr, n, sizeof(int), yield_int_cmp);

    test_check(yield_tick_count == 0,
               "library yield (axl_qsort) does NOT dispatch the default loop");

    /* The idle never fired, so it is still registered. A DIRECT axl_yield()
       must still dispatch it — the public contract is unchanged. */
    axl_yield();
    test_check(yield_tick_count == 1,
               "direct axl_yield() still dispatches (public contract intact)");
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
// The cycle-counter calibration's arithmetic (axl_backend_counter_freq_
// from_samples). gBS->Stall never returns early, so a disturbed sample --
// the vCPU descheduled, an SMI -- only ever counts MORE ticks than the
// Stall's length is worth, and a frequency taken from it makes the
// monotonic clock run slow for the rest of the boot. Every Stall also
// costs the same few microseconds whatever its length (the call, the
// reference timer's reads); differencing a short and a long Stall cancels
// that. A 1 GHz counter is 100,000 ticks per 100 us and 1,900,000 per
// 1900 us, so the difference is 1,800,000 per 1800 us.
//
// An overshoot on the SHORT Stall errs the other way, so the difference is
// bounded by what the long Stall alone gives, which can only overstate:
// never above it, and never more than 2 % below it.
// ---------------------------------------------------------------------------

static void
test_counter_freq_from_samples(void)
{
    static const uint64_t s_equal[5] = {
        100000, 100000, 100000, 100000, 100000
    };
    static const uint64_t l_equal[5] = {
        1900000, 1900000, 1900000, 1900000, 1900000
    };
    /* Every Stall 12 us over, short and long alike: a fixed cost, which
       read off the long Stall alone would be 0.6 % of the frequency. */
    static const uint64_t s_fixed[5] = {
        112000, 112000, 112000, 112000, 112000
    };
    static const uint64_t l_fixed[5] = {
        1912000, 1912000, 1912000, 1912000, 1912000
    };
    /* One long Stall 10 ms over, first; one short Stall 10 ms over, last. */
    static const uint64_t l_first_over[5] = {
        11900000, 1900000, 1900000, 1900000, 1900000
    };
    static const uint64_t s_last_over[5] = {
        100000, 100000, 100000, 100000, 10100000
    };
    /* Every sample a little over, as real Stalls are: the least of each. */
    static const uint64_t s_jitter[5] = {
        100300, 100050, 100900, 100000, 100120
    };
    static const uint64_t l_jitter[5] = {
        1900400, 1900100, 1903000, 1900000, 1900250
    };
    static const uint64_t s_with_zero[3] = { 0, 100000, 0 };
    static const uint64_t l_with_zero[3] = { 1900000, 0, 0 };
    static const uint64_t zeros[5]       = { 0, 0, 0, 0, 0 };
    /* A long sample no larger than the short one: the difference is nothing,
       so the floor under the long Stall alone is what is left. 100,000
       ticks per 1900 us is 52,631,578 Hz; 2 % under that is 51,578,947. */
    static const uint64_t l_too_small[5] = {
        100000, 100000, 100000, 100000, 100000
    };
    /* Every short Stall 1 ms over: differenced, 800,000 ticks per 1800 us
       is 0.44 GHz, a clock running 2.25x fast. The long Stall alone says
       1 GHz, and the result may sit no more than 2 % under that. */
    static const uint64_t s_all_over[5] = {
        1100000, 1100000, 1100000, 1100000, 1100000
    };
    /* The same with the 12 us fixed cost on every Stall: the long Stall
       alone says 1,006,315,789 Hz, 2 % under that is 986,189,474 -- within
       2 % of the truth, on the fast side. */
    static const uint64_t s_all_over_fixed[5] = {
        1112000, 1112000, 1112000, 1112000, 1112000
    };
    /* Every long Stall 10 ms over: differenced, 11,800,000 per 1800 us is
       6.56 GHz, above the 6,263,157,894 the long Stall alone gives -- which
       already overstates, so nothing above it can be right. */
    static const uint64_t l_all_over[5] = {
        11900000, 11900000, 11900000, 11900000, 11900000
    };
    /* 100 GHz, the ceiling the published value is bounded by: no overflow. */
    static const uint64_t s_fast[1] = { 10000000ull };
    static const uint64_t l_fast[1] = { 190000000ull };

    test_check(axl_backend_counter_freq_from_samples(s_equal, l_equal, 5,
                                                     100, 1900)
                   == 1000000000ull,
               "counter freq: five exact pairs -> exactly 1 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_fixed, l_fixed, 5,
                                                     100, 1900)
                   == 1000000000ull,
               "counter freq: a fixed 12 us cost per Stall cancels exactly");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_first_over,
                                                     5, 100, 1900)
                   == 1000000000ull,
               "counter freq: one long Stall 10 ms over -> still 1 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_last_over, l_equal,
                                                     5, 100, 1900)
                   == 1000000000ull,
               "counter freq: one short Stall 10 ms over -> still 1 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_jitter, l_jitter, 5,
                                                     100, 1900)
                   == 1000000000ull,
               "counter freq: every sample a little over -> the least of each");
    test_check(axl_backend_counter_freq_from_samples(s_with_zero,
                                                     l_with_zero, 3, 100,
                                                     1900)
                   == 1000000000ull,
               "counter freq: a zero sample is not a measurement -> skipped");
    test_check(axl_backend_counter_freq_from_samples(s_fast, l_fast, 1, 100,
                                                     1900)
                   == 100000000000ull,
               "counter freq: one 100 GHz pair -> exactly 100 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_all_over, l_equal, 5,
                                                     100, 1900)
                   == 980000000ull,
               "counter freq: every short Stall 1 ms over -> 2 % under the "
               "long alone, not 0.44 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_all_over_fixed,
                                                     l_fixed, 5, 100, 1900)
                   == 986189474ull,
               "counter freq: every short Stall 1 ms over, with the fixed "
               "cost -> within 2 % of 1 GHz");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_all_over, 5,
                                                     100, 1900)
                   == 6263157894ull,
               "counter freq: every long Stall 10 ms over -> no more than "
               "the long alone");
    test_check(axl_backend_counter_freq_from_samples(zeros, l_equal, 5, 100,
                                                     1900) == 1000000000ull,
               "counter freq: every short sample zero -> the long alone");
    test_check(axl_backend_counter_freq_from_samples(s_equal, zeros, 5, 100,
                                                     1900) == 0,
               "counter freq: every long sample zero -> 0");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_too_small,
                                                     5, 100, 1900)
                   == 51578947ull,
               "counter freq: long no larger than short -> 2 % under the "
               "long alone, not 0");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_equal, 0,
                                                     100, 1900) == 0,
               "counter freq: no samples -> 0");
    test_check(axl_backend_counter_freq_from_samples(NULL, l_equal, 5, 100,
                                                     1900) == 1000000000ull,
               "counter freq: NULL short samples -> the long alone");
    test_check(axl_backend_counter_freq_from_samples(s_equal, NULL, 5, 100,
                                                     1900) == 0,
               "counter freq: NULL long samples -> 0");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_equal, 5,
                                                     1900, 1900) == 0,
               "counter freq: equal lengths -> 0");
    test_check(axl_backend_counter_freq_from_samples(s_equal, l_equal, 5,
                                                     1900, 100) == 0,
               "counter freq: lengths reversed -> 0");
}

// ---------------------------------------------------------------------------
// axl_set_exit_status: arm a verbatim EFI_STATUS that both exit paths (CRT0
// return + axl_exit) honor. We can't actually gBS->Exit in-process, so test
// the shared resolver the CRT0 + boot_exit paths both call. The end-to-end
// %lasterror% passthrough is covered by test-exit-status-qemu.sh.
// ---------------------------------------------------------------------------

static void
test_exit_status_resolver(void)
{
    /* Unarmed: rc 0 -> EFI_SUCCESS, and a nonzero rc -> a small POSIX-style
       code (1..255, top bit clear) so the shell shows %lasterror%=N instead of
       collapsing every failure to EFI_ABORTED. Masked to a byte. */
    test_check(axl_backend_resolve_exit_status(0) == (uint64_t)AXL_EFI_SUCCESS,
               "exit-status: unarmed rc 0 -> EFI_SUCCESS");
    test_check(axl_backend_resolve_exit_status(1) == 1,
               "exit-status: unarmed rc 1 -> 1 (small-int, not EFI_ABORTED)");
    test_check(axl_backend_resolve_exit_status(2) == 2,
               "exit-status: unarmed rc 2 -> 2");
    test_check(!AXL_EFI_ERROR(axl_backend_resolve_exit_status(1)),
               "exit-status: unarmed nonzero is non-error-class (top bit clear)");
    test_check(axl_backend_resolve_exit_status(256) == 1,
               "exit-status: rc with a zero low byte maps to 1, never 0");
    test_check(axl_backend_resolve_exit_status(-1) == 0xFF,
               "exit-status: negative rc masks to 0xFF, not a giant value");

    /* Armed: a verbatim status wins for ANY rc — including a non-error-class
       code (top bit clear), which the old path could never produce. */
    axl_set_exit_status(0x34);
    test_check(axl_backend_resolve_exit_status(0) == 0x34,
               "exit-status: armed 0x34 returned verbatim for rc 0");
    test_check(axl_backend_resolve_exit_status(1) == 0x34,
               "exit-status: armed 0x34 overrides nonzero rc (not EFI_ABORTED)");
    test_check(!AXL_EFI_ERROR(0x34),
               "exit-status: 0x34 is non-error-class (top bit clear)");

    /* An armed AXL_EFI_SUCCESS forces success even after a nonzero rc. */
    axl_set_exit_status(AXL_EFI_SUCCESS);
    test_check(axl_backend_resolve_exit_status(7) == (uint64_t)AXL_EFI_SUCCESS,
               "exit-status: armed AXL_EFI_SUCCESS forces success over rc 7");

    /* Error-class values pass through verbatim too. */
    axl_set_exit_status(AXL_EFI_ACCESS_DENIED);
    test_check(axl_backend_resolve_exit_status(0) == (uint64_t)AXL_EFI_ACCESS_DENIED,
               "exit-status: armed error-class status returned verbatim");

    /* CRITICAL: disarm so the pending status can't leak into THIS test
       binary's own exit (CRT0 resolves it on return). */
    axl_backend_clear_exit_status();
    test_check(axl_backend_resolve_exit_status(1) == 1,
               "exit-status: clear() disarms; small-int map restored");
}

// ---------------------------------------------------------------------------
// axl_app_argv0 — runtime accessor for the captured invocation path
// ---------------------------------------------------------------------------

/* Inside an AxlArgs verb handler, the framework presents the verb
   selector as argv[0] in the parsed view (via axl_args_get_pos and
   friends) — the original program-level argv[0] must keep flowing
   from axl_app_argv0() unchanged. The verb stashes a captured
   axl_app_argv0() value here so the outer test can verify it
   matches the pre-dispatch capture. */
static const char *g_verb_app_argv0     = NULL;

static int
argv0_test_verb(AxlArgs *a)
{
    (void)a;
    g_verb_app_argv0 = axl_app_argv0();
    return 0;
}

static void
test_app_argv0_is_stable(void)
{
    /* The runtime guarantees at least argv[0] = "app" as a fallback,
       so axl_app_argv0() must never return NULL after _axl_init. */
    const char *captured = axl_app_argv0();
    test_check(captured != NULL,
               "app_argv0: returns non-NULL (runtime supplies fallback)");
    test_check(captured != NULL && captured[0] != '\0',
               "app_argv0: returns non-empty string");
    test_check(axl_app_argv0() == captured,
               "app_argv0: returns the same pointer across calls");

    /* The real stability test: drive axl_args_run through a stub
       verb that captures the value axl_app_argv0() returns. AxlArgs
       presents its own parsed argv view to the handler; the
       app-level argv[0] (what the runtime captured at startup) must
       remain unchanged. If g_verb_app_argv0 != captured here,
       axl_app_argv0 is silently following the dispatcher — a
       regression. */
    g_verb_app_argv0 = NULL;
    static const AxlArgsNode verbs[] = {
        { .name = "vstub", .handler = argv0_test_verb,
          .help = "argv0 stability test stub" },
        {0}
    };
    char *fake_argv[] = { (char *)"axl-test-runtime", (char *)"vstub", NULL };
    int rc = axl_args_run(2, fake_argv, &(AxlArgsNode){
        .name  = "axl-test-runtime",
        .verbs = verbs,
    });

    test_check(rc == 0, "app_argv0: AxlArgs verb dispatch ran the stub");
    test_check(g_verb_app_argv0 != NULL,
               "app_argv0: stub captured a non-NULL value");
    test_check(g_verb_app_argv0 == captured,
               "app_argv0: stable through AxlArgs dispatch (still program path)");
}

static void
test_app_image_path_is_canonical(void)
{
    /* axl_app_image_path is decoded from EFI_LOADED_IMAGE_PROTOCOL's
       FilePath device-path chain — orthogonal to argv[0], reliable
       regardless of how the shell invoked the binary. Under the
       integration runner, the EFI shell loads AxlTestRuntime.efi
       from the disk image, so the decoded path must contain that
       filename. */
    const char *image_path = axl_app_image_path();
    test_check(image_path != NULL,
               "app_image_path: returns non-NULL under EFI shell load");
    test_check(image_path != NULL
               && axl_strstr(image_path, "AxlTestRuntime") != NULL,
               "app_image_path: contains 'AxlTestRuntime' (the running .efi)");

    /* Volume-mapping prefix: under the shell, the image path is
       prepended with the EFI_SHELL_PROTOCOL.GetMapFromDevicePath
       mapping (e.g. "FS0:" or "fs0:"). Without the prefix, sidecar
       discovery would resolve "\\<sidecar>" against the shell's cwd,
       which breaks when startup.nsh launches the .efi without first
       doing `cd \`. The colon is the canonical mapping marker —
       grep for it as a stable shape check rather than the case-
       sensitive "fs0:" form (different shell builds vary the case). */
    test_check(image_path != NULL
               && axl_strchr(image_path, ':') != NULL,
               "app_image_path: carries the volume-mapping prefix (':' present)");

    /* Stable across calls — pointer-equality check, not just
       string-equality. The runtime owns the buffer; consumers shouldn't
       see it move. */
    test_check(axl_app_image_path() == image_path,
               "app_image_path: returns the same pointer across calls");
}

// ---------------------------------------------------------------------------
// Universal -b / --page pagination flag
// (axl_args_run recognizes it; axl_console_set_page_break toggles the mode)
// ---------------------------------------------------------------------------

static const char *g_page_pos = NULL;
static bool        g_page_ran = false;

static int
page_pos_handler(AxlArgs *a)
{
    g_page_ran = true;
    g_page_pos = axl_args_get_string(a, "path");
    return 0;
}

static void
test_universal_page_flag_stripped(void)
{
    /* A tool that does NOT declare -b: the framework must recognize the
       universal page-break option, consume it, and leave the tool's
       positional intact — never reject it as an unknown flag. */
    static const AxlArgDesc pos[] = {
        { .name = "path", .type = AXL_ARG_STRING, .required = true },
        {0}
    };
    g_page_ran = false; g_page_pos = NULL;
    char *argv[] = { (char *)"tool", (char *)"-b", (char *)"hello", NULL };
    int rc = axl_args_run(3, argv, &(AxlArgsNode){
        .name = "tool", .positionals = pos, .handler = page_pos_handler,
    });
    test_check(rc == 0, "page-flag: -b accepted (not unknown), handler ran");
    test_check(g_page_ran, "page-flag: handler invoked");
    test_check(g_page_pos != NULL && axl_strcmp(g_page_pos, "hello") == 0,
               "page-flag: positional 'hello' survived -b stripping");
    axl_console_set_page_break(false);   /* don't leak the mode into later tests */
}

static bool g_page_own_b  = false;
static bool g_page_own_ran = false;

static int
page_own_b_handler(AxlArgs *a)
{
    g_page_own_ran = true;
    g_page_own_b   = axl_args_get_bool(a, "basic");
    return 0;
}

static void
test_tool_own_b_flag_wins(void)
{
    /* A tool that declares its own -b keeps it: the universal page
       option must DEFER, so an existing tool's -b is unaffected. */
    static const AxlArgDesc flags[] = {
        { .name = "basic", .short_name = 'b', .type = AXL_ARG_BOOL },
        {0}
    };
    g_page_own_ran = false; g_page_own_b = false;
    char *argv[] = { (char *)"tool", (char *)"-b", NULL };
    int rc = axl_args_run(2, argv, &(AxlArgsNode){
        .name = "tool", .flags = flags, .handler = page_own_b_handler,
    });
    test_check(rc == 0, "page-flag: tool with own -b dispatches");
    test_check(g_page_own_ran, "page-flag: own-b handler invoked");
    test_check(g_page_own_b,
               "page-flag: tool's own -b set (universal option deferred)");
    axl_console_set_page_break(false);
}

static void
test_universal_b_survives_unrelated_page_flag(void)
{
    /* The two spellings defer INDEPENDENTLY: a tool that declares an
       unrelated `page` flag (different short) must still get the universal
       `-b` — `page` must not revoke `-b`, or the tool would see an "unknown
       flag" error the feature promises never happens. */
    static const AxlArgDesc flags[] = {
        { .name = "page", .short_name = 'p', .type = AXL_ARG_STRING },
        {0}
    };
    static const AxlArgDesc pos[] = {
        { .name = "path", .type = AXL_ARG_STRING, .required = true },
        {0}
    };
    g_page_ran = false; g_page_pos = NULL;
    char *argv[] = { (char *)"tool", (char *)"-b", (char *)"hello", NULL };
    int rc = axl_args_run(3, argv, &(AxlArgsNode){
        .name = "tool", .flags = flags, .positionals = pos,
        .handler = page_pos_handler,
    });
    test_check(rc == 0, "page-flag: -b accepted despite unrelated 'page' flag");
    test_check(g_page_ran, "page-flag: independent-defer handler ran");
    test_check(g_page_pos != NULL && axl_strcmp(g_page_pos, "hello") == 0,
               "page-flag: -b stripped, positional survived (independent defer)");
    axl_console_set_page_break(false);
}

// ---------------------------------------------------------------------------
// axl_argv_drop — in-place argv slot removal (pre-stripper primitive)
// ---------------------------------------------------------------------------

static void
test_argv_drop_semantics(void)
{
    /* Middle slot: {a,b,c} drop 1 -> {a,c}, argc 2, NULL-terminated. */
    char *v1[] = { (char *)"a", (char *)"b", (char *)"c", NULL };
    int n1 = 3;
    axl_argv_drop(&n1, v1, 1);
    test_check(n1 == 2, "argv_drop: middle -> argc 2");
    test_check(axl_strcmp(v1[0], "a") == 0 && axl_strcmp(v1[1], "c") == 0,
               "argv_drop: middle removed, tail shifted down");
    test_check(v1[2] == NULL, "argv_drop: NULL-terminated at new end");

    /* First slot. */
    char *v2[] = { (char *)"a", (char *)"b", (char *)"c", NULL };
    int n2 = 3;
    axl_argv_drop(&n2, v2, 0);
    test_check(n2 == 2 && axl_strcmp(v2[0], "b") == 0
               && axl_strcmp(v2[1], "c") == 0 && v2[2] == NULL,
               "argv_drop: first removed, tail shifted");

    /* Last slot. */
    char *v3[] = { (char *)"a", (char *)"b", (char *)"c", NULL };
    int n3 = 3;
    axl_argv_drop(&n3, v3, 2);
    test_check(n3 == 2 && axl_strcmp(v3[0], "a") == 0
               && axl_strcmp(v3[1], "b") == 0 && v3[2] == NULL,
               "argv_drop: last removed");

    /* Out-of-range index is a no-op (argc + slots unchanged). */
    char *v4[] = { (char *)"a", (char *)"b", NULL };
    int n4 = 2;
    axl_argv_drop(&n4, v4, 2);    /* i == argc */
    axl_argv_drop(&n4, v4, -1);   /* i < 0    */
    test_check(n4 == 2 && axl_strcmp(v4[0], "a") == 0
               && axl_strcmp(v4[1], "b") == 0,
               "argv_drop: out-of-range index is a no-op");

    /* NULL argc/argv are no-ops (no crash). */
    axl_argv_drop(NULL, v4, 0);
    axl_argv_drop(&n4, NULL, 0);
    test_check(n4 == 2, "argv_drop: NULL argc/argv is a no-op");
}

// ---------------------------------------------------------------------------
// TPL balance
//
// Only SAFE levels are exercised. TPL_HIGH_LEVEL is deliberately absent:
// at that level a pool allocation or a console write HANGS rather than
// failing, so a test that reached it would wedge the whole binary — and
// test-axl.sh runs every binary in ONE boot under ONE timeout, so the
// hang would starve every later test too.
// ---------------------------------------------------------------------------

static void
test_tpl_raise_restore_round_trip(void)
{
    unsigned base = axl_tpl_current();
    unsigned prev = axl_tpl_raise(AXL_TPL_NOTIFY);

    test_check(prev == base, "tpl: raise returns the previous level");
    test_check(axl_tpl_current() == AXL_TPL_NOTIFY,
               "tpl: current reads back the raised level");

    axl_tpl_restore(prev);
    test_check(axl_tpl_current() == base, "tpl: restore returns to baseline");

    /* Nested raise/restore, strictly LIFO. */
    unsigned p1 = axl_tpl_raise(AXL_TPL_CALLBACK);
    unsigned p2 = axl_tpl_raise(AXL_TPL_NOTIFY);
    test_check(p2 == AXL_TPL_CALLBACK, "tpl: nested raise sees the inner level");
    axl_tpl_restore(p2);
    test_check(axl_tpl_current() == AXL_TPL_CALLBACK,
               "tpl: inner restore drops one level only");
    axl_tpl_restore(p1);
    test_check(axl_tpl_current() == base, "tpl: outer restore returns to baseline");

    /* Asking to "raise" below the current level must not lower it —
       otherwise the paired restore would raise, which UEFI rejects. */
    unsigned p3 = axl_tpl_raise(AXL_TPL_NOTIFY);
    unsigned p4 = axl_tpl_raise(AXL_TPL_CALLBACK);
    test_check(axl_tpl_current() == AXL_TPL_NOTIFY,
               "tpl: raise to a LOWER level leaves the level alone");
    axl_tpl_restore(p4);
    axl_tpl_restore(p3);
    test_check(axl_tpl_current() == base, "tpl: level intact after no-op raise");
}

static void
test_tpl_current_is_application_in_foreground(void)
{
    test_check(axl_tpl_current() == AXL_TPL_APPLICATION,
               "tpl: a foreground app runs at TPL_APPLICATION");
}

static void
test_tpl_restore_baseline_is_noop_when_balanced(void)
{
    unsigned leaked = 0xFFFFu;

    test_check(axl_tpl_restore_baseline(&leaked) == false,
               "tpl: restore_baseline reports false when already balanced");
    test_check(leaked == 0xFFFFu,
               "tpl: out_leaked is left untouched when balanced");
    test_check(axl_tpl_restore_baseline(NULL) == false,
               "tpl: restore_baseline accepts a NULL out pointer");
    test_check(axl_tpl_current() == AXL_TPL_APPLICATION,
               "tpl: a balanced restore_baseline changes nothing");
}

static void
test_tpl_restore_baseline_repairs_a_leaked_raise(void)
{
    unsigned leaked = 0;

    /* Leak a raise on purpose — the shape an unwind through a raw
       RaiseTPL frame leaves behind, and the shape that wedges the
       machine when it reaches the shell. */
    (void)axl_tpl_raise(AXL_TPL_NOTIFY);

    test_check(axl_tpl_restore_baseline(&leaked) == true,
               "tpl: restore_baseline reports the repair");
    test_check(leaked == AXL_TPL_NOTIFY,
               "tpl: restore_baseline names the level that was leaked");
    test_check(axl_tpl_current() == AXL_TPL_APPLICATION,
               "tpl: the machine is back at baseline after the repair");

    /* And it is idempotent — a second call finds nothing to do. */
    test_check(axl_tpl_restore_baseline(NULL) == false,
               "tpl: repair is idempotent");

    /* A leak from the LOWEST raised level is repaired too. CALLBACK
       wedges the shell exactly like NOTIFY does, so it must not be
       treated as harmless. */
    (void)axl_tpl_raise(AXL_TPL_CALLBACK);
    leaked = 0;
    test_check(axl_tpl_restore_baseline(&leaked) == true,
               "tpl: a leaked TPL_CALLBACK is repaired, not tolerated");
    test_check(leaked == AXL_TPL_CALLBACK,
               "tpl: restore_baseline names TPL_CALLBACK");
    test_check(axl_tpl_current() == AXL_TPL_APPLICATION,
               "tpl: baseline restored after a CALLBACK leak");
}

// ---------------------------------------------------------------------------
// Deferred exit: which callback it may unwind
//
// An exit asked for while a child image runs is deferred by abandoning the
// innermost AXL notify callback (axl_backend_exit_defer). Only a callback
// entered AFTER the innermost child started may be abandoned: one that was
// already running when the child started -- it opened the bracket itself, a
// ConnectController whose binding Start calls axl_exit -- sits under the
// firmware's frames for that child, and jumping to it would abandon them
// and the bracket's own close, leaving the child count raised for good.
// Both directions, with real trampolines (TPL_CALLBACK timer callbacks) and
// a recording exit function in place of gBS->Exit.
// ---------------------------------------------------------------------------

static volatile int  g_dx_phase;        /* the case the next tick runs; 0 = none */
static volatile bool g_dx_ran_to_end;   /* the callback was NOT abandoned */
static volatile int  g_dx_exit_rc;      /* what the recording exit was given */
static volatile int  g_dx_exit_calls;

static void
dx_record_exit(
    int rc
    )
{
    g_dx_exit_rc = rc;
    g_dx_exit_calls++;
}

static void
dx_tick(
    void *ctx
    )
{
    (void)ctx;
    int phase = g_dx_phase;
    if (phase == 0) {
        return;
    }
    g_dx_phase = 0;
    if (phase == 1) {
        /* OLDER than the child: this callback opens the bracket itself. */
        axl_backend_child_image_enter();
        axl_backend_exit_defer(7, dx_record_exit);
        axl_backend_child_image_leave();
    } else {
        /* NEWER: the bracket was opened in the foreground. */
        axl_backend_exit_defer(9, dx_record_exit);
    }
    g_dx_ran_to_end = true;
}

/* Arm @p phase and wait (up to 2 s) for a tick to take it. */
static bool
dx_run(
    int phase
    )
{
    g_dx_ran_to_end = false;
    g_dx_phase      = phase;
    for (int i = 0; i < 400 && g_dx_phase != 0; i++) {
        axl_msleep(5);
    }
    return g_dx_phase == 0;
}

static void
test_deferred_exit_unwinds_only_newer_callbacks(void)
{
    AxlEventHandle ev = NULL;
    g_dx_exit_rc    = -1;
    g_dx_exit_calls = 0;
    if (axl_backend_event_create_notify_timer(dx_tick, NULL, 100000, &ev) != AXL_OK) {
        test_check(false, "deferred exit: the notify timer is created");
        return;
    }

    /* A callback that opened the child bracket is not abandoned. */
    bool took = dx_run(1);
    test_check(took, "deferred exit (older callback): the tick ran");
    test_check(g_dx_ran_to_end,
               "deferred exit (older callback): not abandoned -- the exit cannot be deferred there");
    if (!g_dx_ran_to_end) {
        axl_backend_child_image_leave();   /* the abandoned bracket's close */
    }
    test_check(!axl_backend_child_images_running(),
               "deferred exit (older callback): the bracket closed");
    axl_backend_exit_if_deferred();
    test_check(g_dx_exit_calls == 0,
               "deferred exit (older callback): nothing was deferred");

    /* A callback entered while the child runs is abandoned, and the exit
       happens once the bracket closes, with that callback's code. */
    axl_backend_child_image_enter();
    took = dx_run(2);
    test_check(took, "deferred exit (newer callback): the tick ran");
    test_check(!g_dx_ran_to_end,
               "deferred exit (newer callback): abandoned at the exit");
    test_check(g_dx_exit_calls == 0,
               "deferred exit (newer callback): not performed while the child runs");
    axl_backend_child_image_leave();
    axl_backend_exit_if_deferred();
    test_check(g_dx_exit_calls == 1 && g_dx_exit_rc == 9,
               "deferred exit (newer callback): performed once the child returned, rc 9");

    axl_backend_event_close(ev);
    axl_backend_clear_exit_status();   /* the deferral armed the status */
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
    test_atexit_full_table_returns_zero();

    test_registry_count_tracks_events();
    test_registry_count_tracks_mixed_kinds();

    test_loop_default_is_stable();
    test_yield_dispatches_ready_work();
    test_library_yield_does_not_dispatch();

    test_interrupted_is_false_at_startup();

    test_signal_install_accepts_handler_and_null();
    test_exit_status_resolver();
    test_counter_freq_from_samples();

    test_app_argv0_is_stable();
    test_app_image_path_is_canonical();

    test_universal_page_flag_stripped();
    test_tool_own_b_flag_wins();
    test_universal_b_survives_unrelated_page_flag();

    test_argv_drop_semantics();

    test_tpl_raise_restore_round_trip();
    test_tpl_current_is_application_in_foreground();
    test_tpl_restore_baseline_is_noop_when_balanced();
    test_tpl_restore_baseline_repairs_a_leaked_raise();

    test_deferred_exit_unwinds_only_newer_callbacks();

    return test_print_results();
}

AXL_APP(test_runtime_main)
