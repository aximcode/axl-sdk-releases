/**
 * @file task-demo.c
 *
 * Arena allocator and AP task pool.
 *
 * Section 1: bump-allocating from a fixed arena, resetting, reusing.
 * Section 2: submitting computation to Application Processors and
 * collecting results on the BSP via poll.
 *
 * Build with: axl-cc task-demo.c -o task-demo.efi
 */

#include <axl.h>

/* -- Task definitions ---------------------------------------------------- */

/*
 * Task procs run on Application Processors -- they cannot call
 * axl_printf, axl_malloc, or any Boot Services function.
 * Only pure computation and arena allocation are safe.
 */

/** Input for the sum task. */
typedef struct {
    int start;
    int end;
    int result;
} SumWork;

/**
 * AP task: compute sum of integers [start, end].
 * Writes result into the SumWork struct (shared memory).
 */
static void
sum_task_proc(void *arg, AxlArena *arena)
{
    SumWork *work = (SumWork *)arg;
    int sum = 0;

    (void)arena;

    for (int i = work->start; i <= work->end; i++) {
        sum += i;
    }
    work->result = sum;
}

/**
 * BSP completion callback: prints the result.
 * Runs on the main thread during axl_task_pool_poll().
 */
static void
sum_task_complete(void *arg, AxlArena *arena)
{
    SumWork *work = (SumWork *)arg;

    (void)arena;
    axl_printf("  task complete: sum(%d..%d) = %d\n",
               work->start, work->end, work->result);
}

/* -- Arena demo ---------------------------------------------------------- */

static void
demo_arena(void)
{
    AxlArena *arena;
    void *p1, *p2, *p3;
    size_t cap;

    axl_printf("--- Arena allocator ---\n\n");

    arena = axl_arena_new(4096);
    if (arena == NULL) {
        axl_printf("error: cannot create arena\n");
        return;
    }

    cap = axl_arena_capacity(arena);
    axl_printf("  created arena: capacity=%zu, remaining=%zu\n",
               cap, axl_arena_remaining(arena));

    /* Allocate three blocks. */
    p1 = axl_arena_alloc(arena, 100);
    axl_printf("  alloc 100 bytes: %s, remaining=%zu\n",
               p1 ? "ok" : "FAIL", axl_arena_remaining(arena));

    p2 = axl_arena_alloc(arena, 200);
    axl_printf("  alloc 200 bytes: %s, remaining=%zu\n",
               p2 ? "ok" : "FAIL", axl_arena_remaining(arena));

    p3 = axl_arena_alloc(arena, 500);
    axl_printf("  alloc 500 bytes: %s, remaining=%zu\n",
               p3 ? "ok" : "FAIL", axl_arena_remaining(arena));

    /* Reset -- all allocations freed, capacity restored. */
    axl_arena_reset(arena);
    axl_printf("\n  after reset: remaining=%zu (capacity=%zu)\n",
               axl_arena_remaining(arena), axl_arena_capacity(arena));

    /* Allocate again to show reuse. */
    p1 = axl_arena_alloc(arena, 256);
    axl_printf("  reuse: alloc 256 bytes: %s, remaining=%zu\n",
               p1 ? "ok" : "FAIL", axl_arena_remaining(arena));

    axl_arena_free(arena);
    axl_printf("  arena freed\n\n");
}

/* -- Task pool demo ------------------------------------------------------ */

static void
demo_task_pool(void)
{
    AxlTaskPool *pool;
    size_t workers;
    SumWork work;
    AxlTaskId tid;

    axl_printf("--- Task pool ---\n\n");

    pool = axl_task_pool_new();
    if (pool == NULL) {
        axl_printf("  error: task pool creation failed\n");
        return;
    }

    workers = axl_task_pool_worker_count(pool);
    if (axl_task_pool_is_single_core(pool)) {
        axl_printf("  single-core system: %zu workers (tasks run synchronously)\n",
                   workers);
    } else {
        axl_printf("  initialized: %zu AP workers available\n", workers);
    }

    /* Prepare work item. */
    work.start = 1;
    work.end = 1000;
    work.result = 0;

    axl_printf("  submitting: sum(%d..%d)\n", work.start, work.end);
    tid = axl_task_pool_submit(pool, sum_task_proc, &work, NULL,
                               sum_task_complete);
    if (tid == AXL_TASK_ID_INVALID) {
        axl_printf("  error: submit failed (all workers busy?)\n");
        axl_task_pool_free(pool);
        return;
    }

    /* Poll until done. On single-core the task already ran synchronously,
     * but poll is still needed to fire the completion callback. */
    while (!axl_task_pool_done(pool, tid)) {
        axl_task_pool_poll(pool);
    }
    /* One final poll to ensure completion callback fires. */
    axl_task_pool_poll(pool);

    axl_task_pool_free(pool);
    axl_printf("  task pool shut down\n");
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("task-demo: arena allocator and AP task pool\n\n");

    demo_arena();
    demo_task_pool();

    return 0;
}
