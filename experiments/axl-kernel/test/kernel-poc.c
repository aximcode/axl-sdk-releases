/**
 * kernel-poc.c — AxlKernelPoc.efi. POC driver for K1 + K2.
 *
 * Validates the three success criteria from AXL-Kernel-Design.md
 * §13.11:
 *
 *   PING-PONG         two procs yielding to each other; per-switch
 *                     wall-clock cost reported.
 *   STRESS-SPAWN-WAIT many short-lived children returning known
 *                     status codes; parent waits on each.
 *   STACK-CANARY      deliberate stack overflow trips the canary.
 *
 * Emits "PASS:"/"FAIL:" lines the integration test script greps for.
 *
 * Each test decides via argv whether to run. For the QEMU test we run
 * ping-pong + stress by default; canary is gated behind
 * `AxlKernelPoc.efi canary` because tripping the canary exits the
 * kernel (it is an abort test, not a recoverable one).
 */

#include <axl.h>
#include "axl-kernel.h"

// ---------------------------------------------------------------------------
// Test 1 — ping-pong
// ---------------------------------------------------------------------------

#define PINGPONG_ITERS 200000

static volatile int ping_count;
static volatile int pong_count;

static int
pong_proc(int argc, char **argv)
{
    (void)argc; (void)argv;
    while (pong_count < PINGPONG_ITERS) {
        pong_count++;
        axlk_yield();
    }
    return 42;
}

static int
pingpong_main(int argc, char **argv)
{
    (void)argc; (void)argv;

    ping_count = 0;
    pong_count = 0;

    AxlkPid pong = axlk_spawn(pong_proc, 0, NULL, 0);
    if (pong < 0) {
        axl_printf("FAIL: pingpong: axlk_spawn\n");
        return 1;
    }

    uint64_t t0 = axl_time_get_ms();

    while (ping_count < PINGPONG_ITERS) {
        ping_count++;
        axlk_yield();
    }

    int status;
    AxlkPid reaped = axlk_wait(pong, &status);

    uint64_t dt_ms = axl_time_get_ms() - t0;

    if (reaped != pong) {
        axl_printf("FAIL: pingpong: wait returned %d (expected %d)\n",
                   (int)reaped, (int)pong);
        return 1;
    }
    if (status != 42) {
        axl_printf("FAIL: pingpong: status %d (expected 42)\n", status);
        return 1;
    }
    if (ping_count != PINGPONG_ITERS || pong_count != PINGPONG_ITERS) {
        axl_printf("FAIL: pingpong: ping=%d pong=%d expected %d\n",
                   ping_count, pong_count, PINGPONG_ITERS);
        return 1;
    }

    /* 2 * PINGPONG_ITERS switches (each yield = one switch). */
    uint64_t switches = 2ULL * (uint64_t)PINGPONG_ITERS;
    uint64_t ns_per = (dt_ms == 0) ? 0 : (dt_ms * 1000000ULL) / switches;

    axl_printf("PASS: pingpong (%llu switches in %llu ms, ~%llu ns/switch)\n",
               (unsigned long long)switches,
               (unsigned long long)dt_ms,
               (unsigned long long)ns_per);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 2 — spawn/exit/wait stress
// ---------------------------------------------------------------------------

#define STRESS_CHILDREN 10
#define STRESS_ROUNDS   4

static int
worker_proc(int argc, char **argv)
{
    (void)argc;
    int tag = (int)(intptr_t)argv;   /* argv pointer used as integer */
    for (int i = 0; i < 3; i++) {
        axlk_yield();
    }
    return tag;
}

static int
stress_main(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (int round = 0; round < STRESS_ROUNDS; round++) {
        AxlkPid pids[STRESS_CHILDREN];
        int    expected[STRESS_CHILDREN];

        for (int i = 0; i < STRESS_CHILDREN; i++) {
            expected[i] = (round * 100) + i + 1;  /* non-zero tag */
            pids[i] = axlk_spawn(worker_proc, 0,
                                (char **)(intptr_t)expected[i], 0);
            if (pids[i] < 0) {
                axl_printf("FAIL: stress: spawn round=%d i=%d\n", round, i);
                return 1;
            }
        }

        /* Collect all children. Order is scheduler-dependent; map pid→index. */
        int got[STRESS_CHILDREN] = {0};
        for (int i = 0; i < STRESS_CHILDREN; i++) {
            int status = 0;
            AxlkPid r = axlk_wait(AXLK_PID_ANY, &status);
            if (r < 0) {
                axl_printf("FAIL: stress: wait returned -1 round=%d\n", round);
                return 1;
            }
            int idx = -1;
            for (int j = 0; j < STRESS_CHILDREN; j++) {
                if (pids[j] == r) { idx = j; break; }
            }
            if (idx < 0) {
                axl_printf("FAIL: stress: unknown pid %d\n", (int)r);
                return 1;
            }
            if (got[idx]) {
                axl_printf("FAIL: stress: pid %d reaped twice\n", (int)r);
                return 1;
            }
            got[idx] = 1;
            if (status != expected[idx]) {
                axl_printf("FAIL: stress: pid %d status %d (expected %d)\n",
                           (int)r, status, expected[idx]);
                return 1;
            }
        }
    }

    /* Proc count after all rounds: just us (pid 1) + anyone else? */
    size_t live = axlk_proc_count();
    if (live != 1) {
        axl_printf("FAIL: stress: %zu procs still live (expected 1)\n", live);
        return 1;
    }

    axl_printf("PASS: stress (%d rounds x %d procs)\n",
               STRESS_ROUNDS, STRESS_CHILDREN);
    return 0;
}

// ---------------------------------------------------------------------------
// Test 3 — stack canary (deliberate overflow)
// ---------------------------------------------------------------------------

static void
recurse(int depth)
{
    volatile char pad[256];   /* ~256 bytes per frame, burns stack */
    pad[0] = (char)depth;
    (void)pad[0];             /* silence -Wunused-but-set-variable */
    axlk_yield();              /* canary check happens here */
    if (depth > 0) {
        recurse(depth - 1);
    }
}

static int
canary_proc(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* 80 frames * ~256 bytes = ~20 KiB, well past the 16 KiB stack. */
    recurse(80);
    return 0;
}

static int
canary_main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("canary: spawning overflower (expect kernel abort)\n");
    AxlkPid c = axlk_spawn(canary_proc, 0, NULL, 0);
    if (c < 0) {
        axl_printf("FAIL: canary: spawn\n");
        return 1;
    }
    int status;
    axlk_wait(c, &status);
    /* If we reach here without an abort, the canary DIDN'T catch it. */
    axl_printf("FAIL: canary: wait returned (canary did not trip)\n");
    return 1;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

static int
pid1(int argc, char **argv)
{
    /* Default sequence: ping-pong + stress. Canary is opt-in. */
    bool run_canary = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] != NULL && axl_strcmp(argv[i], "canary") == 0) {
            run_canary = true;
        }
    }

    if (run_canary) {
        return canary_main(argc, argv);
    }

    int rc = pingpong_main(argc, argv);
    if (rc != 0) return rc;

    rc = stress_main(argc, argv);
    if (rc != 0) return rc;

    axl_printf("AxlKernelPoc: all tests passed\n");
    return 0;
}

int
main(int argc, char **argv)
{
    axl_printf("AxlKernelPoc: starting\n");

    if (axlk_init() != 0) {
        axl_printf("FAIL: axlk_init\n");
        return 1;
    }

    int rc = axlk_run(pid1, argc, argv);
    axl_printf("AxlKernelPoc: kernel exited rc=%d\n", rc);
    return rc;
}
