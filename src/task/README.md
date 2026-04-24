AP worker pool, region-based arena allocator, preallocated buffer pool, and
async work helpers.

Headers:

- `<axl/axl-task.h>` -- Arena allocator and AP task pool
- `<axl/axl-buf-pool.h>` -- Preallocated buffer pool (LIFO free-stack)
- `<axl/axl-async.h>` -- AP-offloaded async work

## Overview

UEFI systems have multiple CPU cores, but only one -- the **Bootstrap
Processor (BSP)** -- can call Boot Services (I/O, networking, protocol
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
void compute(void *arg, AxlArena *arena) {
    Result *r = axl_arena_alloc(arena, sizeof(Result));
    r->crc = calculate_crc(arg);
}

// BSP completion callback (runs on main core)
void on_done(void *arg, AxlArena *arena) {
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

- [`docs/AXL-Concurrency.md`](../../docs/AXL-Concurrency.md) -- the
  full primitive-selection taxonomy. `AxlTask` and `AxlAsync` are the
  "work offload" axis; the doc also covers `AxlLoop` (dispatch),
  `AxlEvent` / `AxlCancellable` / `AxlWait` (coordination), and
  `AxlPubsub` (notification).
- [`src/event/README.md`](../event/README.md) -- the signalling
  primitive to rendezvous with completed AP work.
