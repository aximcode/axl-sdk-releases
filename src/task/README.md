AP worker pool, region-based arena allocator, preallocated buffer pool, and
async work helpers.

Headers:

- `<axl/axl-task.h>` — Arena allocator and AP task pool
- `<axl/axl-buf-pool.h>` — Preallocated buffer pool (LIFO free-stack)
- `<axl/axl-async.h>` — AP-offloaded async work

## Overview

UEFI systems have multiple CPU cores, but only one — the **Bootstrap
Processor (BSP)** — can call Boot Services (I/O, networking, protocol
calls). The other cores are **Application Processors (APs)** that can
run compute-heavy tasks in parallel.

```text
BSP (main core)              APs (worker cores)
├─ Boot Services (gBS)       ├─ CRC/checksum
├─ Protocol calls            ├─ Decompression
├─ Network I/O               ├─ Hash computation
├─ File I/O                  ├─ Data parsing
├─ axl_printf                └─ Memory-only work
└─ Event loop                   (no Boot Services!)
```

**AP constraints**: APs cannot call Boot Services, `axl_printf`,
`axl_fopen`, or any UEFI protocol function. They can only access
memory (including the arena allocator, which uses lock-free CAS).

**AP SIMD state is per-core.** `CR4.OSXSAVE` and `XCR0` are not shared
between processors, so AVX enabled on the BSP is not enabled on a
worker. A task that executes AVX without calling `axl_cpu_enable_avx()`
on that core takes a `#UD`:

```c
void compute(AxlArena *arena, void *arg) {
    if (axl_cpu_enable_avx()) {
        // AVX is live on THIS core now
    }
}
```

It is idempotent and reads live hardware state, so only the first task
on a given core pays for the enable. Note that `axl_cpu_features()`
describes the machine, while the `axl_cpu_enable_*` functions answer for
the calling core — on a hybrid CPU those differ, so branch on the
return value rather than the feature bits.

**Workers are stopped before OS handoff.** The pool registers a
before-ExitBootServices handler that parks its workers, because an AP
still spinning in consumer code when the firmware hands off can hang the
boot outright (the firmware's own AP-relocation handler waits for an
acknowledgement that never comes). After it fires the pool keeps
working, synchronously.

**A worker slot is not the worker's memory.** The two are easy to
conflate, and they have opposite lifetimes:

| | Worker slot | `AxlArena` |
|---|---|---|
| What it is | The pool's per-AP control block — task pointer, state word, flags | The memory a task actually allocates from |
| How big | Fixed, one cache line; one per AP | **Whatever you pass** to `axl_arena_new(capacity)` |
| Who sizes it | Nobody — it follows the AP count | You do, per arena |
| Where it lives | Fixed storage, capped by `AXL_TASK_MAX_WORKERS` | Heap |

So the cap bounds how many workers can run at once (256 — a machine with
more enabled APs uses the first 256 and warns), **not** how much memory a
task may use. The slot holds only an `AxlArena *`; give tasks arenas as
large as you like, and different arenas to different tasks.

Slots live in fixed storage rather than the heap because the firmware may
deliver a dispatched AP into its worker *after* the pool that dispatched
it has been freed. Heap memory cannot express that lifetime: freeing the
slot is a cross-CPU use-after-free, and keeping it is an allocator leak.
A slot whose AP never acknowledged exit is simply never reissued.

### Arena Allocator

Lock-free bump allocator for AP-safe memory. Pre-allocates a contiguous
block; allocations are O(1) pointer bumps with CAS (no locks needed
for concurrent AP access).

```c
AXL_AUTOPTR(AxlArena) arena = axl_arena_new(4096);

// AP-safe: can be called from any core
void *buf = axl_arena_alloc(arena, 256);

// BSP-only: reset frees all allocations at once
axl_arena_reset(arena);
```

### Task Pool

Submit work to an AP and get a callback on the BSP when it completes:

```c
// Create pool (discovers available APs)
AxlTaskPool *pool = axl_task_pool_new();

// AP work function (runs on a worker core)
void compute(AxlArena *arena, void *arg) {
    Result *r = axl_arena_alloc(arena, sizeof(Result));
    r->crc = calculate_crc(arg);
}

// BSP completion callback (runs on main core)
void on_done(AxlArena *arena, void *arg) {
    axl_printf("CRC computed on AP\n");
}

AxlArena *arena = axl_arena_new(1024);
axl_task_pool_submit(pool, compute, data, arena, on_done);

// Poll for completions (call from event loop or main loop)
axl_task_pool_poll(pool);

// Single-core fallback: if no APs available, submit runs
// the work synchronously on the BSP.

axl_task_pool_free(pool);
```

### AxlBufPool

Preallocated fixed-size buffer pool with LIFO free-stack. Zero-copy:
`get` returns a buffer pointer, `put` returns it to the pool.
No allocation or freeing in the hot path.

```c
AXL_AUTOPTR(AxlBufPool) pool = axl_buf_pool_new(4, 64 * 1024);
//                                                ^   ^
//                                          4 buffers, 64KB each

void *buf = axl_buf_pool_get(pool);   // grab a buffer (NULL if exhausted)
// ... use buf ...
axl_buf_pool_put(pool, buf);          // return to pool
```

### AxlAsync

Convenience wrapper: submit AP work and get a BSP callback via the
event loop (combines **AxlTask** + **AxlLoop** idle source).

```c
AxlAsync *async = axl_async_new(loop, 4);  // max 4 pending jobs

axl_async_submit(async, work_fn, data, arena, done_fn);
// work_fn runs on AP, done_fn fires on BSP via loop idle

axl_async_free(async);
```

## See also

- [`docs/AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md) — the
  full primitive-selection taxonomy. `AxlTask` and `AxlAsync` are the
  "work offload" axis; the doc also covers `AxlLoop` (dispatch),
  `AxlEvent` / `AxlCancellable` / `AxlWait` (coordination), and
  `AxlPubsub` (notification).
- [`src/event/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) — the signalling
  primitive to rendezvous with completed AP work.
