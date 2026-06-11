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
// axl_set_exit_status: arm a verbatim EFI_STATUS that both exit paths (CRT0
// return + axl_exit) honor. We can't actually gBS->Exit in-process, so test
// the shared resolver the CRT0 + boot_exit paths both call. The end-to-end
// %lasterror% passthrough is covered by test-exit-status-qemu.sh.
// ---------------------------------------------------------------------------

static void
test_exit_status_resolver(void)
{
    /* Unarmed: the legacy rc -> EFI_STATUS map (unchanged behavior). */
    test_check(axl_backend_resolve_exit_status(0) == (uint64_t)AXL_EFI_SUCCESS,
               "exit-status: unarmed rc 0 -> EFI_SUCCESS");
    test_check(axl_backend_resolve_exit_status(1) == (uint64_t)AXL_EFI_ABORTED,
               "exit-status: unarmed rc nonzero -> EFI_ABORTED");

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
    test_check(axl_backend_resolve_exit_status(1) == (uint64_t)AXL_EFI_ABORTED,
               "exit-status: clear() disarms; legacy map restored");
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
    test_exit_status_resolver();

    test_app_argv0_is_stable();
    test_app_image_path_is_canonical();

    return test_print_results();
}

AXL_APP(test_runtime_main)
