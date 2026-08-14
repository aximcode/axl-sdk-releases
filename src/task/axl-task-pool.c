/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-task-pool.c
    Persistent AP worker pool with lock-free task dispatch.
    Single-core systems get transparent synchronous fallback.

    Migrated from AxlTaskPool.c(EDK2-style) to GLib-style API.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-task.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("task");

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

/* Spin bounds for the two points where the BSP waits on a worker: coming
   up (axl_task_pool_new) and going down (axl_task_pool_free). Both are
   backstops against a wedged AP, not expected costs — an AP that is going
   to arrive does so almost immediately. */
#define WORKER_READY_TIMEOUT_SPINS  1000000u
#define WORKER_EXIT_TIMEOUT_SPINS   1000000u

static inline void
cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/* Slot lifecycle, encoded in ONE word so every observer reads a single,
   consistent value. The previous design used three separate volatile flags
   (task/running/done); available() and submit() read them as three loads, so
   a worker completing concurrently (done 0->1 then running 1->0) between the
   `done` read and the `running` read made a just-completed slot look idle.
   That let available() over-report and submit() clobber an unreaped
   completion -> a dropped task -> hang. A single state word makes that torn
   read structurally impossible.

   Transitions (only the BSP does FREE<->SUBMITTED and DONE->FREE; only the
   owning worker does SUBMITTED->DONE), so no observer ever sees a partial
   slot:
       FREE      --submit-->  SUBMITTED        (BSP)
       SUBMITTED --worker-->  DONE             (owning AP)
       DONE      --poll---->  FREE             (BSP) */
typedef enum {
    SLOT_FREE      = 0,   /* idle; calloc leaves slots here */
    SLOT_SUBMITTED = 1,   /* task assigned, worker will run it */
    SLOT_DONE      = 2,   /* task complete, awaiting poll */
    SLOT_UNUSABLE  = 3    /* AP never reached the loop; never dispatch here */
} SlotState;

/* Exactly one cache line, and aligned to one.

   Workers spin on `state` from different processors, so two slots
   sharing a line means every worker's poll invalidates its neighbour's
   copy. The BSP-only flags below sit in the padding after `state`
   rather than being appended, because growing past 64 bytes makes slots
   straddle lines and reintroduces exactly that sharing — worth ~15% of
   dispatch latency when it was measured. Two bytes of that padding are
   still free; keep using them before extending the struct. */
typedef struct {
    volatile uint32_t    state;       /* SlotState — the single sync point */
    bool                 dispatched;  /* an AP was successfully sent here */
    volatile uint8_t     probe;       /* BSP sets, live worker clears */
    AxlTaskProc          proc;        /* task fn (published before SUBMITTED) */
    void                *arg;
    AxlArena            *arena;
    AxlTaskComplete      on_complete;
    volatile uint32_t    ready;       /* 1 = worker reached its spin loop */
    volatile uint32_t    quit;        /* 1 = shutdown */
    volatile uint32_t    exited;      /* 1 = worker has exited */
    AxlTaskId            id;
    size_t               ap_number;   /* processor number for this worker */
} __attribute__((aligned(64))) WorkerSlot;

_Static_assert(sizeof(WorkerSlot) == 64,
               "WorkerSlot must stay one cache line - see comment above");

// ---------------------------------------------------------------------------
// Worker slot storage
//
// Slots are the landing zone an AP writes into, so their lifetime is not
// the pool's to decide. The firmware may deliver a dispatched AP into
// worker_proc at any point after StartupThisAP accepted it — including
// after the pool that dispatched it has been torn down. Heap memory
// cannot express that: freeing a slot an AP may still write to is a
// cross-CPU use-after-free, and keeping it instead is an allocator leak
// the build gate rightly fails.
//
// Static storage is neither. A slot whose worker never confirmed exit is
// simply never handed out again — no allocator is involved, so there is
// nothing to leak, and the memory stays valid forever, which makes a
// late arrival land somewhere harmless. Retention is bounded by the
// machine's AP count and is reported when it happens.
// ---------------------------------------------------------------------------

static WorkerSlot  g_worker_slots[AXL_TASK_MAX_WORKERS];
static bool        g_worker_slot_taken[AXL_TASK_MAX_WORKERS];

/* Reserve up to @a count slots into @a out, zeroed.

   Deliberately NOT a contiguous run. Retention punches permanent holes
   in the table, so a contiguity requirement fails while much of the
   table is still free: one slot retained per pool generation degrades
   the free space into a comb of length-1 gaps. Callers address slots
   through @a out, so their addresses need no relationship to each other.

   Measured, by injecting one retained slot per generation and cycling
   3-worker pools until creation fails: a contiguous allocator dies at
   generation 126 with roughly half the table unused, while this one
   reaches 253 of 256 — i.e. it runs out only when the slots are
   genuinely spent, not when they are merely scattered.

   Note this property is NOT guarded by the test suite. Discriminating
   between the two allocators requires injecting retention, which no
   public API can do (it needs an AP that never acknowledges exit), and
   unit-testing this table directly would mean a test including internal
   headers. The evidence above is a sabotage experiment, not a gate.
   Anyone reintroducing a contiguity requirement here will not be caught
   by a red test.

   @return number of slots taken (0 if none are free). */
static
size_t
worker_slots_take(
    WorkerSlot **out,
    size_t       count
    )
{
    size_t  taken;
    size_t  i;

    taken = 0;
    for (i = 0; i < AXL_TASK_MAX_WORKERS && taken < count; i++) {
        if (g_worker_slot_taken[i]) {
            continue;
        }
        g_worker_slot_taken[i] = true;
        axl_memset(&g_worker_slots[i], 0, sizeof(WorkerSlot));
        out[taken] = &g_worker_slots[i];
        taken++;
    }

    return taken;
}

/* Return slots for reuse.

   A slot is reusable only once no AP can ever write to it again: either
   it was never successfully dispatched, or its worker acknowledged exit.
   Anything else is retained permanently — there is no way to ask the
   firmware to take back a dispatch, so the only safe answer is to stop
   using that memory. */
static
void
worker_slots_release(
    WorkerSlot **slots,
    size_t       count
    )
{
    size_t  i;
    size_t  retained;

    if (slots == NULL) {
        return;
    }

    retained = 0;
    for (i = 0; i < count; i++) {
        if (slots[i] == NULL) {
            continue;
        }
        if (!slots[i]->dispatched || slots[i]->exited) {
            g_worker_slot_taken[slots[i] - g_worker_slots] = false;
            continue;
        }
        retained++;
    }

    if (retained > 0) {
        axl_warning("retaining %llu worker slot(s) whose AP never "
                    "acknowledged exit; they cannot be reused",
                    (unsigned long long)retained);
    }
}

// ---------------------------------------------------------------------------
// Pool struct
// ---------------------------------------------------------------------------

struct AxlTaskPool {
    AxlMpContext   *mp_ctx;
    WorkerSlot    **slots;         /* indirection into the static table */
    size_t          slot_reserved; /* slots taken from the static pool */
    size_t          slot_count;    /* slots dispatched — the iteration bound */
    size_t          worker_count;  /* slots whose worker reached its loop */
    uint32_t        next_task_id;
    bool            single_core;
    AxlEventHandle  pre_ebs;       /* before-ExitBootServices quiesce hook */
};

// ---------------------------------------------------------------------------
// AP worker loop
// ---------------------------------------------------------------------------

static
void
EFIAPI
worker_proc(
    void *arg
    )
{
    WorkerSlot  *slot;
    AxlTaskProc  proc;

    slot = (WorkerSlot *)arg;

    /* Publish arrival before doing anything else. axl_task_pool_new blocks
       on this: a slot whose AP never gets here is retired UNUSABLE rather
       than counted, so submit() can't hand a task to a worker that will
       never run it (which reads as a task that never completes). */
    slot->ready = 1;
    __sync_synchronize();

    while (!slot->quit) {
        /* Liveness answer. Clearing this from inside the loop is what
           distinguishes a worker that is still running from one that
           merely got far enough to set `ready` before the firmware
           terminated it. Costs one load per spin on a line this core
           already owns; the store happens only when probed. */
        if (slot->probe) {
            slot->probe = 0;
        }
        if (slot->state == SLOT_SUBMITTED) {
            __sync_synchronize();          /* acquire — pair with submit's release
                                              so proc/arg/arena are visible */
            proc = slot->proc;
            proc(slot->arena, slot->arg);
            __sync_synchronize();          /* release — task results visible before DONE */
            slot->state = SLOT_DONE;
        }
        cpu_pause();
    }

    slot->exited = 1;               /* signal that this worker has exited */
}

/* Wait for dispatched workers to reach their spin loop.

   Retires every slot that does not arrive as SLOT_UNUSABLE: submit()
   only ever fills a SLOT_FREE slot and poll() only reaps SLOT_DONE, so
   an UNUSABLE slot is inert to the rest of the pool without needing a
   check at each site. The slot itself stays allocated for the pool's
   lifetime — a late AP still owns it and would write ready/exited into
   it — and teardown only waits on workers that actually arrived.

   @return number of workers that reached their loop. */
static
size_t
wait_for_workers(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  ready;
    size_t  waited;

    for (waited = 0; waited < WORKER_READY_TIMEOUT_SPINS; waited++) {
        ready = 0;
        for (i = 0; i < pool->slot_count; i++) {
            ready += pool->slots[i]->ready;
        }
        if (ready == pool->slot_count) {
            break;
        }
        cpu_pause();
    }

    /* Arrival is not liveness. `ready` is a sticky latch set by the
       worker's first instruction, so it stays 1 even if the firmware
       terminated the worker immediately afterwards -- which the PI spec
       explicitly permits it to do when a blocking StartupThisAP times
       out ("then Procedure on the AP is terminated"). Probe for an echo
       so a counted worker is one that is demonstrably still spinning,
       not merely one that once started. */
    for (i = 0; i < pool->slot_count; i++) {
        pool->slots[i]->probe = pool->slots[i]->ready ? 1 : 0;
    }
    __sync_synchronize();          /* publish probes before sampling echoes */

    for (waited = 0; waited < WORKER_READY_TIMEOUT_SPINS; waited++) {
        ready = 0;
        for (i = 0; i < pool->slot_count; i++) {
            ready += (pool->slots[i]->probe == 0);
        }
        if (ready == pool->slot_count) {
            break;
        }
        cpu_pause();
    }

    ready = 0;
    for (i = 0; i < pool->slot_count; i++) {
        /* An unanswered probe means dispatched-but-not-running: either it
           never arrived, or it arrived and was killed. Both are the same
           thing to us -- no task may go there. */
        if (pool->slots[i]->ready && pool->slots[i]->probe == 0) {
            ready++;
            continue;
        }
        axl_warning("AP #%llu was dispatched but is not running its worker "
                    "loop; retiring the slot",
                    (unsigned long long)pool->slots[i]->ap_number);
        pool->slots[i]->state = SLOT_UNUSABLE;
    }

    return ready;
}

/* Stop every worker and wait for the ones that arrived to acknowledge.

   Shared by the before-ExitBootServices hook and axl_task_pool_free —
   both want exactly this, and a second copy would be a second place to
   get the ready/exited distinction wrong. Idempotent: quit is a latch
   and an exited worker stays exited. */
static
void
stop_workers(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  timeout;
    bool    all_exited;

    if (pool->slots == NULL) {
        return;
    }

    /* Signal every dispatched slot, not only the ready ones: a slot
       retired UNUSABLE may still be carrying a late AP, and quit is the
       only thing that gets it back out of the loop. */
    for (i = 0; i < pool->slot_count; i++) {
        pool->slots[i]->quit = 1;
    }
    __sync_synchronize();

    /* Wait only on workers that arrived. A slot that never became ready
       has no worker to acknowledge — waiting on it would burn the whole
       timeout on every teardown. */
    all_exited = false;
    for (timeout = 0; timeout < WORKER_EXIT_TIMEOUT_SPINS; timeout++) {
        all_exited = true;
        for (i = 0; i < pool->slot_count; i++) {
            if (pool->slots[i]->ready && !pool->slots[i]->exited) {
                all_exited = false;
                break;
            }
        }
        if (all_exited) {
            break;
        }
        cpu_pause();
    }

    if (!all_exited) {
        axl_warning("not all AP workers exited cleanly");
    }
}

/* Park the workers before the firmware tears down Boot Services.

   An AP still inside our spin loop at OS handoff is a boot-stopper, not
   a leak. MpInitLib's own ExitBootServices handler broadcasts a
   relocation request to every AP and then spins unconditionally until
   each acknowledges — and an AP that never returns to the firmware's
   idle loop never does. Whether that hangs is a platform property: the
   default PcdCpuApLoopMode (Hlt-loop) rescues it with INIT-SIPI-SIPI,
   Mwait-loop and Run-loop platforms do not. Stopping our own workers
   first is cheap and does not bet on a PCD.

   Any completion still unreaped is abandoned deliberately: on_complete
   runs on the BSP and is allowed to call Boot Services, which is exactly
   what must not happen from here on. The pool stays usable — submit()
   falls back to running the task synchronously. */
static
void
pre_exit_boot_quiesce(
    void *ctx
    )
{
    AxlTaskPool *pool = (AxlTaskPool *)ctx;

    /* Gated on slots, not on single_core: a pool that fell back to
       synchronous because no AP arrived still DISPATCHED those APs, and
       one of them arriving late is precisely the case that must not be
       left spinning at handoff. */
    if (pool == NULL || pool->slots == NULL) {
        return;
    }

    stop_workers(pool);
    pool->single_core  = true;
    pool->worker_count = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlTaskPool *
axl_task_pool_new(void)
{
    AxlTaskPool  *pool;
    size_t        i;
    size_t        launched;
    size_t        ap_count;

    pool = axl_calloc(1, sizeof(AxlTaskPool));
    if (pool == NULL) {
        return NULL;
    }

    pool->next_task_id = 1;

    pool->mp_ctx = axl_backend_mp_init(&ap_count);
    if (pool->mp_ctx == NULL) {
        axl_debug("MP Services not available -- single-core fallback");
        pool->single_core = true;
        return pool;
    }

    if (ap_count > AXL_TASK_MAX_WORKERS) {
        axl_warning("%llu APs available but only %d worker slots; using the "
                    "first %d", (unsigned long long)ap_count,
                    AXL_TASK_MAX_WORKERS, AXL_TASK_MAX_WORKERS);
        ap_count = AXL_TASK_MAX_WORKERS;
    }

    pool->slots = axl_calloc(ap_count, sizeof(WorkerSlot *));
    if (pool->slots == NULL) {
        axl_error("failed to allocate %llu slot pointers",
                 (unsigned long long)ap_count);
        axl_backend_mp_cleanup(pool->mp_ctx);
        pool->mp_ctx = NULL;
        pool->single_core = true;
        return pool;
    }

    pool->slot_reserved = worker_slots_take(pool->slots, ap_count);
    if (pool->slot_reserved == 0) {
        axl_error("no free worker slots (all %d in use)",
                  AXL_TASK_MAX_WORKERS);
        axl_free(pool->slots);
        pool->slots = NULL;
        axl_backend_mp_cleanup(pool->mp_ctx);
        pool->mp_ctx = NULL;
        pool->single_core = true;
        return pool;
    }
    ap_count = pool->slot_reserved;

    /* Dispatch each AP into its persistent worker loop. Compact slots on
       failure so slots[0..launched-1] are all dispatched. */
    launched = 0;
    for (i = 0; i < ap_count; i++) {
        pool->slots[launched]->ap_number =
            axl_backend_mp_get_ap_number(pool->mp_ctx, i);

        if (axl_backend_mp_start_ap(pool->mp_ctx, i, worker_proc,
                                    pool->slots[launched]) != AXL_OK) {
            axl_warning("failed to start AP #%llu",
                       (unsigned long long)pool->slots[launched]->ap_number);
            /* Burn the slot rather than reusing its address for the next
               AP. A failed dispatch does not prove the AP never entered
               worker_proc -- a firmware that terminates the procedure on
               timeout reports failure for an AP that already ran. Reusing
               the address would put two processors on one slot, which is
               the aliasing this whole storage scheme exists to prevent. */
            pool->slots[launched]->state = SLOT_UNUSABLE;
            launched++;
            continue;
        }
        pool->slots[launched]->dispatched = true;
        launched++;
    }

    pool->slot_count = launched;

    /* A successful dispatch only means the firmware ACCEPTED the AP, not
       that the AP is running our loop — and on firmware that refuses
       non-blocking mode the backend's blocking fallback can report success
       for an AP the firmware then resets. Wait for each worker to announce
       itself, and count only those that do. */
    pool->worker_count = wait_for_workers(pool);
    if (pool->worker_count == 0) {
        /* Fall back to synchronous execution, but leave the slots and the
           MP context owned by the pool rather than tearing them down here:
           a dispatched-but-absent AP may still arrive and write into its
           slot, so nothing may be released until axl_task_pool_free has
           signalled quit and waited. One teardown path, not two. Falling
           through also registers the quiesce hook below — a dispatched AP
           that has not arrived YET is exactly the one that must not still
           be spinning at OS handoff. */
        axl_debug("no APs reached their worker loop -- single-core fallback");
        pool->single_core = true;
    }

    /* Non-fatal if this fails: the pool still works, it just no longer
       guarantees the APs are parked before OS handoff. Say so loudly
       rather than leaving a boot hang to be discovered on a platform
       whose AP loop mode we don't control. */
    if (axl_backend_event_create_before_exit_boot(pre_exit_boot_quiesce, pool,
                                                  &pool->pre_ebs) != AXL_OK) {
        axl_warning("no before-ExitBootServices hook; AP workers must be "
                    "stopped by freeing the pool before OS handoff");
        pool->pre_ebs = NULL;
    }

    axl_debug("%llu AP workers running", (unsigned long long)pool->worker_count);
    return pool;
}

void
axl_task_pool_free(
    AxlTaskPool *pool
    )
{
    if (pool == NULL) {
        return;
    }

    /* Drop the hook before stopping the workers: once the pool memory is
       gone the notify would run against a freed context. */
    if (pool->pre_ebs != NULL) {
        axl_backend_event_close(pool->pre_ebs);
        pool->pre_ebs = NULL;
    }

    stop_workers(pool);

    /* Reclaim the firmware's per-AP dispatch state before the slots: the
       drain in mp_cleanup waits for the firmware to confirm each worker
       has left our procedure, which is the same fact that decides whether
       a slot is safe to hand out again. */
    if (pool->mp_ctx != NULL) {
        axl_backend_mp_cleanup(pool->mp_ctx);
        pool->mp_ctx = NULL;
    }

    worker_slots_release(pool->slots, pool->slot_reserved);
    axl_free(pool->slots);   /* BSP-only indirection table; no AP sees it */

    axl_free(pool);
}

AxlTaskId
axl_task_pool_submit(
    AxlTaskPool    *pool,
    AxlTaskProc     proc,
    void           *arg,
    AxlArena       *arena,
    AxlTaskComplete on_complete
    )
{
    AxlTaskId  id;
    size_t     i;

    if (pool == NULL || proc == NULL) {
        return AXL_TASK_ID_INVALID;
    }

    id = pool->next_task_id++;
    if (id == AXL_TASK_ID_INVALID) {
        id = pool->next_task_id++;
    }

    /* Single-core fallback: run synchronously */
    if (pool->single_core || pool->slots == NULL) {
        proc(arena, arg);
        if (on_complete != NULL) {
            on_complete(arena, arg);
        }
        return id;
    }

    /* Find a free slot. Only the BSP (this thread) moves slots FREE ->
       SUBMITTED and DONE -> FREE, and a single state word means the read is
       never torn, so a slot seen FREE here stays FREE until we fill it. */
    for (i = 0; i < pool->slot_count; i++) {
        if (pool->slots[i]->state == SLOT_FREE) {
            pool->slots[i]->proc = proc;
            pool->slots[i]->arg = arg;
            pool->slots[i]->arena = arena;
            pool->slots[i]->on_complete = on_complete;
            pool->slots[i]->id = id;
            __sync_synchronize();          /* release — fields visible before SUBMITTED */
            pool->slots[i]->state = SLOT_SUBMITTED;
            return id;
        }
    }

    axl_warning("all %llu worker(s) busy, cannot submit task",
               (unsigned long long)pool->worker_count);
    return AXL_TASK_ID_INVALID;
}

size_t
axl_task_pool_poll(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  completed;

    if (pool == NULL || pool->single_core || pool->slots == NULL) {
        return 0;
    }

    completed = 0;
    for (i = 0; i < pool->slot_count; i++) {
        if (pool->slots[i]->state == SLOT_DONE) {
            __sync_synchronize();          /* acquire — ensure task results visible */
            if (pool->slots[i]->on_complete != NULL) {
                pool->slots[i]->on_complete(pool->slots[i]->arena,
                                           pool->slots[i]->arg);
            }
            pool->slots[i]->on_complete = NULL;
            __sync_synchronize();          /* release — reap (on_complete read arg) before FREE */
            pool->slots[i]->state = SLOT_FREE;
            completed++;
        }
    }

    return completed;
}

bool
axl_task_pool_done(
    AxlTaskPool *pool,
    AxlTaskId    id
    )
{
    size_t  i;

    if (pool == NULL || id == AXL_TASK_ID_INVALID || pool->single_core) {
        return true;
    }

    if (pool->slots == NULL) {
        return true;
    }

    for (i = 0; i < pool->slot_count; i++) {
        if (pool->slots[i]->id == id) {
            /* Complete once the slot reaches DONE (awaiting poll) or has been
               reaped back to FREE; still SUBMITTED means in flight. */
            return pool->slots[i]->state != SLOT_SUBMITTED;
        }
    }

    return true;
}

size_t
axl_task_pool_available(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  count;

    if (pool == NULL || pool->single_core || pool->slots == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < pool->slot_count; i++) {
        if (pool->slots[i]->state == SLOT_FREE) {
            count++;
        }
    }

    return count;
}

size_t
axl_task_pool_worker_count(
    AxlTaskPool *pool
    )
{
    if (pool == NULL) {
        return 0;
    }
    return pool->worker_count;
}

bool
axl_task_pool_is_single_core(
    AxlTaskPool *pool
    )
{
    if (pool == NULL) {
        return true;
    }
    return pool->single_core;
}
