/**
 * yield-test.c — Proof that Ctrl-C routes through the yield +
 * default-exit path.
 *
 * Sequence:
 *   1. Build a large sorted-in-reverse array and run axl_array_sort on
 *      it (instrumented with axl_yield every 1024 outer iters).
 *   2. Print "entering idle -- ready for Ctrl-C" so the test harness
 *      knows when to drop idle.
 *   3. Enter axl_loop_run(axl_loop_default()) and stay there.
 *   4. An axl_atexit handler prints a SHUTDOWN-MARKER line that the
 *      harness greps for.
 *
 * With the argument `defer-chain` (or built with -DYIELD_TEST_DEFER_CHAIN,
 * for a boot with no Shell to pass arguments), step 3 runs with a deferred
 * callback that re-queues itself for ever: the loop never waits, so a
 * serial Ctrl-C must be noticed by the events it CHECKS each iteration.
 *
 * Run with scripts/run-qemu.sh --background, wait for the idle
 * marker, wait for QEMU CPU to drop near zero, send a 0x03 byte
 * into QEMU's stdin FIFO. The shell's break handler should fire,
 * axl_loop_run returns, main returns, _axl_cleanup runs the atexit
 * list, and the SHUTDOWN marker appears in the serial log.
 */

#include <axl.h>

static int
cmp_int(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

/* A deferred callback that re-queues itself: the loop always has deferred
   work, so it only ever checks its events and never waits on them. */
static void
on_chain(void *data)
{
    if (axl_defer((AxlLoop *)data, on_chain, data) == 0) {
        axl_printf("yield-test: defer queue full; the chain stopped\n");
    }
}

static void
on_shutdown(void *data)
{
    (void)data;
    axl_printf("YIELD-TEST: SHUTDOWN-MARKER (atexit fired)\n");
}

int
main(int argc, char **argv)
{
#ifdef YIELD_TEST_DEFER_CHAIN
    bool chain = true;
    (void)argc;
    (void)argv;
#else
    bool chain = argc > 1 && axl_strcmp(argv[1], "defer-chain") == 0;
#endif

    axl_atexit(on_shutdown, NULL);

    axl_printf("yield-test: start\n");

    const size_t N = 50000;
    AxlArray *a = axl_array_new(sizeof(int));
    if (a == NULL) {
        axl_printf("yield-test: axl_array_new failed\n");
        return 1;
    }

    /* Reverse-sorted input is the worst case for insertion sort,
       ensuring the yield branch inside the outer loop is exercised
       many times. */
    for (size_t i = 0; i < N; i++) {
        int v = (int)(N - i);
        axl_array_append(a, &v);
    }
    axl_printf("yield-test: sorting %zu elements (worst case)\n", N);

    axl_array_sort(a, cmp_int);

    axl_printf("yield-test: sort complete\n");
    axl_array_free(a);

    if (chain && axl_defer(axl_loop_default(), on_chain, axl_loop_default()) == 0) {
        axl_printf("yield-test: the defer chain could not start\n");
        return 1;
    }
    axl_printf("yield-test: entering idle -- ready for Ctrl-C%s\n",
               chain ? " (defer chain running)" : "");
    axl_loop_run(axl_loop_default());

    /* If we got here, the loop quit cleanly (e.g., on break). */
    axl_printf("yield-test: loop returned, exiting main\n");
    return 0;
}
