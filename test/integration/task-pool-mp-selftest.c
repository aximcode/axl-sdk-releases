/** @file task-pool-mp-selftest.c
    Multi-core AxlTaskPool stress / correctness regression test.

    Targets the torn-read race in axl_task_pool_available() / _submit():
    the (task, done, running) slot flags were read as three separate
    volatile loads, so a worker completing concurrently (done 0->1, then
    running 1->0) between the `done` read and the `running` read made a
    just-completed slot look idle. Effects:
      - available() OVER-reports free slots, so a consumer that submits
        exactly available() tasks per cycle gets a rejected submit
        (AXL_TASK_ID_INVALID) it had every right to expect to succeed;
      - submit() with the same torn read can assign a new task to a slot
        that still holds an unreaped completion -> the completion count is
        lost -> the wave never reaches n -> hang (the R6725 axbench hang).

    This test drives the pool the way the contract says you may: each wave
    it reads available() and submits exactly that many tiny tasks, trusting
    every submit to succeed; then polls to completion under a watchdog.
    On the buggy pool this produces INVALID submits and/or a stall. On the
    fixed pool every wave completes cleanly.

    The unit harness boots single-core, so this is an integration test run
    under `-smp 4` (test-task-pool-mp-qemu.sh). Single-core boots SKIP.
**/

#include <axl.h>

#define N_CHUNKS   64          /* >> worker count, forces wave refills */
#define WAVES      4000        /* enough to make the race near-certain */
#define WORK_ITERS 4000        /* small per-task work: completions overlap
                                  the BSP's available()/submit window */
#define WAVE_WATCHDOG_US  5000000ull   /* 5 s per wave = stall */

typedef struct {
    volatile uint32_t ran;     /* set to 1 by the task (its own slot) */
    uint32_t          pad;
} Chunk;

static Chunk g_chunks[N_CHUNKS];

/* AP-safe: touches only its own chunk, no Boot Services / alloc / print. */
static void
stress_task(void *arg, AxlArena *arena)
{
    (void)arena;
    Chunk *c = (Chunk *)arg;
    volatile uint64_t acc = 0;
    for (uint32_t i = 0; i < WORK_ITERS; i++) {
        acc += i * 2654435761u;
    }
    (void)acc;
    c->ran = 1;
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    AxlTaskPool *pool = axl_task_pool_new();
    if (pool == NULL) {
        axl_printf("MP-POOL: FAIL pool_new returned NULL\r\n");
        return 1;
    }

    if (axl_task_pool_is_single_core(pool)) {
        axl_printf("MP-POOL: SKIP (single-core; needs -smp >1 + MP Services)\r\n");
        axl_task_pool_free(pool);
        return 0;
    }

    size_t W = axl_task_pool_worker_count(pool);
    axl_printf("MP-POOL: %zu workers; %d waves x %d tasks\r\n",
               W, WAVES, N_CHUNKS);

    uint64_t invalid_submits = 0;   /* available() over-report signature */
    uint64_t corrupt_waves   = 0;   /* a wave where not every task ran    */
    bool     stalled         = false;
    int      wave            = 0;

    for (wave = 0; wave < WAVES && !stalled; wave++) {
        for (size_t i = 0; i < N_CHUNKS; i++) {
            g_chunks[i].ran = 0;
        }

        size_t   submitted = 0, done = 0;
        uint64_t last_progress_us = axl_time_get_us();

        while (done < N_CHUNKS) {
            /* Trust the contract: submit exactly available() tasks. */
            size_t avail = axl_task_pool_available(pool);
            size_t batch = N_CHUNKS - submitted;
            if (batch > avail) { batch = avail; }
            for (size_t b = 0; b < batch; b++) {
                if (axl_task_pool_submit(pool, stress_task, &g_chunks[submitted],
                                         NULL, NULL) == AXL_TASK_ID_INVALID) {
                    invalid_submits++;   /* over-report: a "free" slot wasn't */
                }
                submitted++;             /* a trusting consumer advances here */
            }

            size_t freed = axl_task_pool_poll(pool);
            done += freed;
            if (freed > 0) {
                last_progress_us = axl_time_get_us();
            } else if (axl_time_get_us() - last_progress_us > WAVE_WATCHDOG_US) {
                stalled = true;
                break;
            }
        }

        if (!stalled) {
            for (size_t i = 0; i < N_CHUNKS; i++) {
                if (g_chunks[i].ran != 1) { corrupt_waves++; break; }
            }
        }
    }

    axl_task_pool_free(pool);

    bool pass = !stalled && invalid_submits == 0 && corrupt_waves == 0;
    if (pass) {
        axl_printf("MP-POOL: PASS (W=%zu, %d waves, 0 invalid, 0 stall)\r\n",
                   W, WAVES);
        return 0;
    }
    axl_printf("MP-POOL: FAIL wave=%d invalid_submits=%llu corrupt_waves=%llu "
               "stalled=%s\r\n",
               wave, (unsigned long long)invalid_submits,
               (unsigned long long)corrupt_waves, stalled ? "yes" : "no");
    return 1;
}
