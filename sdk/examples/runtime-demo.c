/**
 * @file runtime-demo.c
 *
 * Phase A7 integration demo. Exercises the production runtime surface
 * end-to-end: axl_signal_install, axl_interrupted, axl_atexit,
 * axl_yield, axl_loop_default, axl_loop_iterate_until, axl_exit,
 * and the tier-1 resource registry's exit-time sweep.
 *
 * This file started life as a stand-alone prototype with its own
 * Demo* mini-runtime (Phase A7 §8 of docs/AXL-Lifecycle.md). That
 * mini-runtime has since moved into src/runtime/ -- what's left here
 * is just scenario code driving the real APIs.
 *
 * Eight subcommand scenarios (see usage()); each maps to one facet
 * of the runtime design.
 */

#include <axl.h>

// ---------------------------------------------------------------------------
// Scenario 1: signal -- install a handler, run a loop, Ctrl-C
// ---------------------------------------------------------------------------

static int sig_ticks;

static bool
sig_on_tick(void *data)
{
    AxlLoop *loop = data;
    sig_ticks++;
    axl_printf("tick %d\n", sig_ticks);
    if (sig_ticks >= 20) {
        axl_printf("no Ctrl-C received after 10s; exiting\n");
        axl_loop_quit(loop);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
sig_on_ctrlc(void)
{
    axl_printf("got Ctrl-C, will exit cleanly\n");
}

static int
scenario_signal(void)
{
    AxlLoop *loop = axl_loop_default();
    if (loop == NULL) {
        axl_printf("error: loop alloc failed\n");
        return 1;
    }
    axl_signal_install(sig_on_ctrlc);
    axl_printf("signal: press Ctrl-C to exit\n");
    axl_loop_add_timer(loop, 500, sig_on_tick, loop);

    /* axl_loop_run returns -1 on Ctrl-C. The user handler has
     * already fired via _axl_signal_on_break -- we just unwind. */
    axl_loop_run(loop);
    axl_printf("bye\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 2: atexit -- LIFO order
// ---------------------------------------------------------------------------

static void atexit_a(void *data) { (void)data; axl_printf("atexit: A\n"); }
static void atexit_b(void *data) { (void)data; axl_printf("atexit: B\n"); }
static void atexit_c(void *data) { (void)data; axl_printf("atexit: C\n"); }

static int
scenario_atexit(void)
{
    axl_atexit(atexit_a, NULL);
    axl_atexit(atexit_b, NULL);
    axl_atexit(atexit_c, NULL);
    axl_printf("registered A, B, C\n");
    /* LIFO drain happens in _axl_cleanup after main returns. */
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 3: yield -- interruptible CPU loop
// ---------------------------------------------------------------------------

static void
yield_on_ctrlc(void)
{
    /* No-op handler: installing one (even empty) suppresses axl_yield's
     * default-policy auto-exit. This lets the CPU loop below observe
     * axl_interrupted() and print a clean summary before returning. */
}

static int
scenario_yield(void)
{
    axl_signal_install(yield_on_ctrlc);

    axl_printf("yield: tight CPU loop -- press Ctrl-C to interrupt\n");
    axl_printf("counting...\n");

    const uint64_t deadline_ms = axl_time_get_ms() + 30000;
    uint64_t       i = 0;
    while (axl_time_get_ms() < deadline_ms) {
        static volatile uint64_t sink;
        for (int j = 0; j < 4096; j++) {
            sink += i * j;
        }
        i++;

        axl_yield();
        if (axl_interrupted()) {
            break;
        }
        if ((i & 0xFF) == 0) {
            axl_printf("  iter %llu\n", (unsigned long long)i);
        }
    }

    if (axl_interrupted()) {
        axl_printf("interrupted at iter=%llu\n", (unsigned long long)i);
    } else {
        axl_printf("deadline hit at iter=%llu (no Ctrl-C)\n",
                   (unsigned long long)i);
    }
    axl_printf("bye\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 4: default-loop -- timer on the runtime's shared loop
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop *loop;
    int      count;
} DefaultLoopCtx;

static bool
default_loop_tick(void *data)
{
    DefaultLoopCtx *ctx = data;
    ctx->count++;
    axl_printf("tick %d\n", ctx->count);
    if (ctx->count >= 5) {
        axl_loop_quit(ctx->loop);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static int
scenario_default_loop(void)
{
    AxlLoop *loop = axl_loop_default();
    if (loop == NULL) {
        axl_printf("error: loop alloc failed\n");
        return 1;
    }

    DefaultLoopCtx ctx = { .loop = loop, .count = 0 };
    axl_loop_add_timer(loop, 200, default_loop_tick, &ctx);

    axl_loop_run(loop);
    axl_printf("clean exit\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 5: nested-loop -- wait inside a callback, outer resumes
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop  *outer;
    AxlEvent *inner;
    int       outer_ticks;
    bool      nested_done;
} NestedCtx;

static bool
nested_outer_tick(void *data)
{
    NestedCtx *ctx = data;
    ctx->outer_ticks++;
    axl_printf("outer tick %d\n", ctx->outer_ticks);

    if (!ctx->nested_done) {
        /* Pre-signal and wait from inside the callback. The wait
         * spins up its own ephemeral loop (the outer one is paused
         * here) and returns immediately via the signalled event.
         * Validates the nested-wait contract in AXL-Lifecycle.md §5.4
         * -- ephemeral loops don't corrupt the outer loop's state. */
        axl_printf("  inner wait armed\n");
        axl_event_signal(ctx->inner);
        int r = axl_event_wait_timeout(ctx->inner, NULL, 1000 * 1000);
        if (r == 0) {
            axl_printf("  inner wait returned ok\n");
        } else {
            axl_printf("  inner wait rc=%d (expected 0)\n", r);
        }
        axl_event_reset(ctx->inner);
        ctx->nested_done = true;
    }

    if (ctx->outer_ticks >= 3) {
        axl_loop_quit(ctx->outer);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static int
scenario_nested_loop(void)
{
    AxlLoop  *outer = axl_loop_new();
    AxlEvent *inner = axl_event_new();
    if (outer == NULL || inner == NULL) {
        axl_printf("error: alloc failed\n");
        axl_event_free(inner);
        axl_loop_free(outer);
        return 1;
    }

    NestedCtx ctx = { .outer = outer, .inner = inner };

    axl_loop_add_timer(outer, 200, nested_outer_tick, &ctx);
    axl_loop_run(outer);

    axl_printf("bye\n");
    axl_event_free(inner);
    axl_loop_free(outer);
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 5b: iterate-until -- nested wait with outer loop still alive
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop  *outer;
    AxlEvent *inner;
    int       outer_ticks;
    int       side_ticks;
    bool      nested_done;
} IterateCtx;

static bool
iter_side_tick(void *data)
{
    IterateCtx *ctx = data;
    ctx->side_ticks++;
    axl_printf("  side-source tick %d (outer loop still alive)\n",
               ctx->side_ticks);
    return AXL_SOURCE_CONTINUE;
}

static bool
iter_inner_signal(void *data)
{
    AxlEvent *inner = data;
    axl_printf("  inner-signal timeout fired; signalling inner event\n");
    axl_event_signal(inner);
    return AXL_SOURCE_REMOVE;
}

static bool
iter_outer_tick(void *data)
{
    IterateCtx *ctx = data;
    ctx->outer_ticks++;
    axl_printf("outer tick %d\n", ctx->outer_ticks);

    if (!ctx->nested_done) {
        /* Arm a 300ms producer and a 150ms repeating "side" timer,
         * both on the OUTER loop. With iterate_until driving the
         * outer loop, BOTH fire during the nested wait. (Contrast
         * with scenario 5, where axl_event_wait_timeout's ephemeral
         * loop freezes the outer's sources.) */
        axl_printf("  arming inner-signal (+300ms) and side-source "
                   "(150ms repeating)\n");
        axl_loop_add_timeout(ctx->outer, 300, iter_inner_signal, ctx->inner);
        AxlSourceId side_id = axl_loop_add_timer(ctx->outer, 150,
                                              iter_side_tick, ctx);

        axl_printf("  entering iterate_until(1s timeout)\n");
        int r = axl_loop_iterate_until(ctx->outer, ctx->inner,
                                       1000 * 1000);
        axl_printf("  iterate_until returned rc=%d, saw %d side ticks\n",
                   r, ctx->side_ticks);

        axl_loop_remove_source(ctx->outer, side_id);
        axl_event_reset(ctx->inner);
        ctx->nested_done = true;
    }

    if (ctx->outer_ticks >= 3) {
        axl_loop_quit(ctx->outer);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static int
scenario_iterate_until(void)
{
    AxlLoop  *outer = axl_loop_new();
    AxlEvent *inner = axl_event_new();
    if (outer == NULL || inner == NULL) {
        axl_printf("error: alloc failed\n");
        axl_event_free(inner);
        axl_loop_free(outer);
        return 1;
    }

    IterateCtx ctx = { .outer = outer, .inner = inner };

    axl_loop_add_timer(outer, 500, iter_outer_tick, &ctx);
    axl_loop_run(outer);

    axl_printf("bye\n");
    axl_event_free(inner);
    axl_loop_free(outer);
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario 6: leak-event -- deliberate leaks, sweep catches + logs
// ---------------------------------------------------------------------------

static int
scenario_leak_event(const char *variant)
{
    if (variant == NULL || axl_strcmp(variant, "event") == 0) {
        AxlEvent *e = axl_event_new();
        axl_printf("leaked AxlEvent at %p (no free)\n", (void *)e);
        (void)e;
        return 0;
    }
    if (axl_strcmp(variant, "loop") == 0) {
        AxlLoop *l = axl_loop_new();
        axl_printf("leaked AxlLoop at %p (no free)\n", (void *)l);
        (void)l;
        return 0;
    }
    if (axl_strcmp(variant, "cancellable") == 0) {
        AxlCancellable *c = axl_cancellable_new();
        axl_printf("leaked AxlCancellable at %p (no free)\n", (void *)c);
        (void)c;
        return 0;
    }
    if (axl_strcmp(variant, "stress") == 0) {
        /* The real registry is AxlArray-backed (unbounded), so there's
         * no "overflow" path like the prototype's fixed-cap table. This
         * variant just exercises that the sweep handles many leaks in
         * one pass. */
        const int n = 20;
        axl_printf("stress: allocating %d events (all deliberately leaked)\n", n);
        for (int i = 0; i < n; i++) {
            axl_event_new();
        }
        return 0;
    }
    axl_printf("usage: runtime-demo leak-event [event|loop|cancellable|stress]\n");
    return 2;
}

// ---------------------------------------------------------------------------
// Scenario 7: axl-exit-vs-return -- same cleanup on both paths
// ---------------------------------------------------------------------------

static int
scenario_axl_exit_vs_return(const char *variant, int rc)
{
    if (variant == NULL) {
        axl_printf("usage: runtime-demo axl-exit-vs-return [return|exit] <rc>\n");
        return 2;
    }
    axl_atexit(atexit_a, NULL);
    axl_atexit(atexit_b, NULL);
    axl_atexit(atexit_c, NULL);
    axl_printf("registered A, B, C\n");

    if (axl_strcmp(variant, "return") == 0) {
        axl_printf("path: return %d from main\n", rc);
        return rc;
        /* CRT0 runs _axl_cleanup on main's return. */
    }
    if (axl_strcmp(variant, "exit") == 0) {
        axl_printf("path: axl_exit(%d)\n", rc);
        axl_exit(rc);
        /* axl_exit is NORETURN: runs _axl_cleanup, gBS->Exit. */
    }
    axl_printf("usage: runtime-demo axl-exit-vs-return [return|exit] <rc>\n");
    return 2;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

static void
usage(void)
{
    axl_printf(
        "usage: runtime-demo <subcommand>\n"
        "\n"
        "  signal                    install handler, press Ctrl-C\n"
        "  atexit                    register 3 callbacks, exit (LIFO)\n"
        "  yield                     tight CPU loop, press Ctrl-C\n"
        "  default-loop              timer on default loop, 5 ticks\n"
        "  nested-loop               inner wait inside outer callback (ephemeral loop)\n"
        "  iterate-until             nested wait keeping outer loop alive\n"
        "  leak-event [kind]         leak event|loop|cancellable|stress; sweep catches\n"
        "  axl-exit-vs-return P RC   P=return|exit; prove identical cleanup\n"
    );
}

static int
dispatch(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];

    if (axl_strcmp(cmd, "signal") == 0)        return scenario_signal();
    if (axl_strcmp(cmd, "atexit") == 0)        return scenario_atexit();
    if (axl_strcmp(cmd, "yield") == 0)         return scenario_yield();
    if (axl_strcmp(cmd, "default-loop") == 0)  return scenario_default_loop();
    if (axl_strcmp(cmd, "nested-loop") == 0)   return scenario_nested_loop();
    if (axl_strcmp(cmd, "iterate-until") == 0) return scenario_iterate_until();
    if (axl_strcmp(cmd, "leak-event") == 0) {
        return scenario_leak_event(argc >= 3 ? argv[2] : NULL);
    }
    if (axl_strcmp(cmd, "axl-exit-vs-return") == 0) {
        const char *variant = (argc >= 3) ? argv[2] : NULL;
        int rc = 0;
        if (argc >= 4) {
            const char *p = argv[3];
            if (*p == '\0') {
                axl_printf("error: empty rc arg\n");
                return 2;
            }
            for (; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    axl_printf("error: rc must be a non-negative integer, "
                               "got '%s'\n", argv[3]);
                    return 2;
                }
                rc = rc * 10 + (*p - '0');
            }
        }
        return scenario_axl_exit_vs_return(variant, rc);
    }

    axl_printf("unknown subcommand: %s\n", cmd);
    usage();
    return 2;
}

int
main(int argc, char **argv)
{
    return dispatch(argc, argv);
}
