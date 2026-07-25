/** @file axl-test-task.c
    Test application for AxlTask — arena, task pool, buffer pool, async.
    Runs in QEMU (single-core) to verify the fallback path.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-task.h>
#include <axl/axl-buf-pool.h>
#include <axl/axl-async.h>
#include <axl/axl-loop.h>

// ---------------------------------------------------------------------------
// Arena tests
// ---------------------------------------------------------------------------

static void
test_arena_basic(void)
{
    AxlArena *a;
    void *p1;
    void *p2;
    size_t rem;

    a = axl_arena_new(1024);
    test_check(a != NULL, "arena: create");
    if (a == NULL) {
        return;
    }

    test_check(axl_arena_capacity(a) == 1024, "arena: capacity is 1024");
    test_check(axl_arena_remaining(a) == 1024, "arena: remaining is 1024");

    p1 = axl_arena_alloc(a, 100);
    test_check(p1 != NULL, "arena: alloc 100");
    test_check(((uintptr_t)p1 & 7) == 0, "arena: alloc aligned");

    if (p1 != NULL) {
        axl_memset(p1, 0xAB, 100);
        test_check(*((uint8_t *)p1) == 0xAB, "arena: write/read");
    }

    rem = axl_arena_remaining(a);
    test_check(rem < 1024, "arena: remaining decreased");

    p2 = axl_arena_alloc(a, 200);
    test_check(p2 != NULL, "arena: alloc 200");
    test_check(p2 != p1, "arena: different pointers");

    axl_arena_free(a);
}

static void
test_arena_reset(void)
{
    AxlArena *a;

    a = axl_arena_new(512);
    if (a == NULL) {
        return;
    }

    axl_arena_alloc(a, 256);
    test_check(axl_arena_remaining(a) < 512, "arena: used after alloc");

    axl_arena_reset(a);
    test_check(axl_arena_remaining(a) == 512, "arena: reset restores capacity");

    axl_arena_free(a);
}

static void
test_arena_exhaustion(void)
{
    AxlArena *a;
    void *p;

    a = axl_arena_new(64);
    if (a == NULL) {
        return;
    }

    p = axl_arena_alloc(a, 48);
    test_check(p != NULL, "arena: alloc 48 fits");

    p = axl_arena_alloc(a, 48);
    test_check(p == NULL, "arena: exhaustion returns NULL");

    axl_arena_free(a);
}

// ---------------------------------------------------------------------------
// Task pool tests (single-core fallback)
// ---------------------------------------------------------------------------

static bool task_ran;
static bool complete_fired;

static void
task_test_proc(AxlArena *arena, void *arg)
{
    size_t *result;

    task_ran = true;

    if (arena != NULL) {
        result = axl_arena_alloc(arena, sizeof(size_t));
        if (result != NULL) {
            *result = 42;
        }
    }
}

static void
task_test_complete(AxlArena *arena, void *arg)
{
    complete_fired = true;
}

static void
test_task_pool_single_core(void)
{
    AxlTaskPool *pool;
    AxlTaskId id;
    AxlArena *arena;

    pool = axl_task_pool_new();
    test_check(pool != NULL, "task: pool created");
    if (pool == NULL) {
        return;
    }

    test_check(axl_task_pool_worker_count(pool) == 0
               || !axl_task_pool_is_single_core(pool),
               "task: worker count");

    task_ran = false;
    complete_fired = false;

    arena = axl_arena_new(256);
    test_check(arena != NULL, "task: arena for submit");

    id = axl_task_pool_submit(pool, task_test_proc, NULL, arena,
                              task_test_complete);
    test_check(id != AXL_TASK_ID_INVALID, "task: submit returns valid ID");
    test_check(task_ran, "task: proc ran synchronously");
    test_check(complete_fired, "task: on_complete fired");
    test_check(axl_task_pool_done(pool, id), "task: done returns true");

    axl_arena_free(arena);
    axl_task_pool_free(pool);
}

// ---------------------------------------------------------------------------
// Buffer pool tests
// ---------------------------------------------------------------------------

static void
test_buf_pool_basic(void)
{
    AxlBufPool *pool;
    void *buf;

    pool = axl_buf_pool_new(4, 64);
    test_check(pool != NULL, "bufpool: new 4x64");
    if (pool == NULL) {
        return;
    }

    test_check(axl_buf_pool_available(pool) == 4, "bufpool: available is 4");
    test_check(axl_buf_pool_buf_size(pool) == 64, "bufpool: buf_size is 64");

    buf = axl_buf_pool_get(pool);
    test_check(buf != NULL, "bufpool: get returns non-NULL");
    test_check(axl_buf_pool_available(pool) == 3, "bufpool: available is 3");

    axl_buf_pool_put(pool, buf);
    test_check(axl_buf_pool_available(pool) == 4, "bufpool: put restores count");

    axl_buf_pool_free(pool);
}

static void
test_buf_pool_exhaustion(void)
{
    AxlBufPool *pool;
    void *a;
    void *b;
    void *c;

    pool = axl_buf_pool_new(2, 32);
    if (pool == NULL) {
        return;
    }

    a = axl_buf_pool_get(pool);
    b = axl_buf_pool_get(pool);
    test_check(a != NULL, "bufpool: get 1 of 2");
    test_check(b != NULL, "bufpool: get 2 of 2");

    c = axl_buf_pool_get(pool);
    test_check(c == NULL, "bufpool: exhaustion returns NULL");
    test_check(axl_buf_pool_available(pool) == 0, "bufpool: available is 0");

    axl_buf_pool_put(pool, a);
    test_check(axl_buf_pool_available(pool) == 1, "bufpool: put restores 1");

    c = axl_buf_pool_get(pool);
    test_check(c != NULL, "bufpool: get after put succeeds");

    axl_buf_pool_free(pool);
}

static void
test_buf_pool_distinct(void)
{
    AxlBufPool *pool;
    void *a;
    void *b;
    void *c;

    pool = axl_buf_pool_new(3, 128);
    if (pool == NULL) {
        return;
    }

    a = axl_buf_pool_get(pool);
    b = axl_buf_pool_get(pool);
    c = axl_buf_pool_get(pool);

    test_check(a != b && b != c && a != c, "bufpool: 3 buffers are distinct");

    axl_buf_pool_free(pool);
}

static void
test_buf_pool_lifo(void)
{
    AxlBufPool *pool;
    void *a;
    void *b;
    void *c;

    pool = axl_buf_pool_new(2, 64);
    if (pool == NULL) {
        return;
    }

    a = axl_buf_pool_get(pool);
    b = axl_buf_pool_get(pool);
    (void)b;  /* drain pool; only 'a' matters for the LIFO check */

    axl_buf_pool_put(pool, a);
    c = axl_buf_pool_get(pool);
    test_check(c == a, "bufpool: LIFO order (put A, get returns A)");

    axl_buf_pool_free(pool);
}

static void
test_buf_pool_null_safety(void)
{
    test_check(axl_buf_pool_get(NULL) == NULL,
               "bufpool: get(NULL) returns NULL");
    test_check(axl_buf_pool_available(NULL) == 0,
               "bufpool: available(NULL) returns 0");
    test_check(axl_buf_pool_buf_size(NULL) == 0,
               "bufpool: buf_size(NULL) returns 0");

    /* These should not crash */
    axl_buf_pool_put(NULL, NULL);
    axl_buf_pool_free(NULL);
    test_pass("bufpool: NULL-safe put/free");
}

// ---------------------------------------------------------------------------
// Async tests (single-core fallback)
// ---------------------------------------------------------------------------

static bool async_work_ran;
static bool async_done_fired;

static void
async_test_work(AxlArena *arena, void *data)
{
    (void)arena;
    async_work_ran = true;
    if (data != NULL) {
        *(int *)data = 99;
    }
}

static void
async_test_done(AxlArena *arena, void *data)
{
    (void)arena;
    async_done_fired = true;
}

static bool
on_async_quit_timer(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static void
test_async_init_shutdown(void)
{
    AxlLoop *loop;
    AxlAsync *async;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    test_check(async != NULL, "async: new returns non-NULL");

    axl_async_free(async);
    test_pass("async: free");

    axl_loop_free(loop);
}

static void
test_async_submit_single_core(void)
{
    AxlLoop *loop;
    AxlAsync *async;
    AxlAsyncHandle h;
    int result = 0;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    if (async == NULL) {
        axl_loop_free(loop);
        return;
    }

    async_work_ran = false;
    async_done_fired = false;

    h = axl_async_submit(async, async_test_work, &result, NULL, async_test_done);
    test_check(h != AXL_ASYNC_INVALID, "async: submit returns valid handle");

    /* Single-core: work + done ran synchronously during submit */
    test_check(async_work_ran, "async: work ran");
    test_check(async_done_fired, "async: done_cb fired");
    test_check(result == 99, "async: work modified data");

    axl_async_free(async);
    axl_loop_free(loop);
}

static void
test_async_submit_with_loop(void)
{
    AxlLoop *loop;
    AxlAsync *async;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    if (async == NULL) {
        axl_loop_free(loop);
        return;
    }

    async_work_ran = false;
    async_done_fired = false;

    axl_async_submit(async, async_test_work, NULL, NULL, async_test_done);

    /* Run loop briefly to drain any pending work */
    axl_loop_add_timeout(loop, 100, on_async_quit_timer, loop);
    axl_loop_run(loop);

    test_check(async_work_ran, "async: work ran via loop");
    test_check(async_done_fired, "async: done_cb fired via loop");

    axl_async_free(async);
    axl_loop_free(loop);
}

static void
test_async_cancel(void)
{
    AxlLoop *loop;
    AxlAsync *async;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    if (async == NULL) {
        axl_loop_free(loop);
        return;
    }

    /* Cancel on single-core is a no-op (work already completed) */
    test_check(!axl_async_cancel(async, AXL_ASYNC_INVALID),
               "async: cancel invalid returns false");
    test_check(!axl_async_cancel(async, 999),
               "async: cancel unknown returns false");

    axl_async_free(async);
    axl_loop_free(loop);
}

static void
test_async_pending(void)
{
    AxlLoop *loop;
    AxlAsync *async;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    if (async == NULL) {
        axl_loop_free(loop);
        return;
    }

    /* On single-core, pending should be 0 after submit (completed synchronously) */
    axl_async_submit(async, async_test_work, NULL, NULL, NULL);
    test_check(axl_async_pending(async) == 0,
               "async: pending is 0 after single-core submit");

    axl_async_free(async);
    axl_loop_free(loop);
}

static void
test_async_null_safety(void)
{
    AxlLoop *loop;
    AxlAsync *async;

    loop = axl_loop_new();
    if (loop == NULL) {
        return;
    }

    async = axl_async_new(loop, 4);
    if (async == NULL) {
        axl_loop_free(loop);
        return;
    }

    test_check(axl_async_submit(NULL, async_test_work, NULL, NULL, NULL)
               == AXL_ASYNC_INVALID,
               "async: submit NULL async returns invalid");
    test_check(axl_async_submit(async, NULL, NULL, NULL, NULL)
               == AXL_ASYNC_INVALID,
               "async: submit NULL work_fn returns invalid");

    axl_async_free(async);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_task_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlTask");

    test_arena_basic();
    test_arena_reset();
    test_arena_exhaustion();
    test_task_pool_single_core();
    test_buf_pool_basic();
    test_buf_pool_exhaustion();
    test_buf_pool_distinct();
    test_buf_pool_lifo();
    test_buf_pool_null_safety();
    test_async_init_shutdown();
    test_async_submit_single_core();
    test_async_submit_with_loop();
    test_async_cancel();
    test_async_pending();
    test_async_null_safety();

    return test_print_results();
}

AXL_APP(test_task_main)
