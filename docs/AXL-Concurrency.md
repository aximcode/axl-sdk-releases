# AXL Concurrency Model

AXL targets UEFI, which is cooperatively concurrent and
single-threaded on the BSP. There are no OS threads, no preemption,
no shared-memory locking to design around. Concurrency comes from
callbacks (via `AxlLoop`) and, where available, work offload onto
other cores (via `AxlTask`).

This doc is the single source of truth for **which primitive to
reach for**. It also records **why** AXL picked the libuv / GLib /
asyncio style of callbacks-plus-stop-tokens over GIL-style
threading, stackful coroutines, and protothread macros — so future
contributors don't re-litigate the design.

See [src/event/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) for the prose on
the event primitives themselves, and [src/loop/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md)
for event-loop mechanics.

**Also see:** [`AXL-Runtime.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md) — the CRT0-owned
runtime that ships today (Phase A7, April 2026): lazy default
loop, Linux-style signal handling, `axl_yield()` for tight loops,
`axl_atexit` for cleanup, tier-1 resource registry + sweep. Apps
that run a tight CPU loop can stay Ctrl-C responsive and fire
registered timers by calling `axl_yield()` alone, without ever
calling `axl_loop_run`; see §2.4 of that doc for the worked
example.

## A note on naming

"Event" appears three times in AXL docs:

- The **event loop** (`AxlLoop`) — the dispatcher.
- An **event source** — a thing registered with the loop (timer,
  idle, raw event, ...).
- `AxlEvent` — one kind of source: a one-shot latch.

This mirrors UEFI's own overload. An `AxlEvent` is a one-shot
latch backed by a UEFI event, and the event loop dispatches them.

## The four-axis taxonomy

Every AXL concurrency primitive answers exactly one of four
questions. Overlap is minimal and deliberate.

| Axis | Primitive | Purpose | Loop integration |
|------|-----------|---------|------------------|
| **Dispatch** — "when does my code run?" | [`AxlLoop`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) + sources (timer, timeout, key, idle, proto-notify, raw event) | The event reactor | *is* the loop |
| | [`AxlDefer`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) | "Run this soon, on next tick" — escape a constrained callback | requires a running `AxlLoop` |
| **Coordination** — "how do I wait for X?" | [`axl_wait_for_flag/word`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md), `axl_wait_for`, `axl_wait_for_with_tick` | Interruptible poll of memory (MMIO status, hardware) or a predicate | spins up a throwaway `AxlLoop` |
| | [`AxlEvent`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) | Producer signals → waiter resumes (zero polling, UEFI-event-driven) | spins up a throwaway `AxlLoop` for `axl_event_wait_timeout`, or register its handle with a caller-owned loop via `axl_loop_add_event(loop, axl_event_handle(e), ...)` |
| | [`axl_wait_ms`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) | Interruptible sleep | spins up a throwaway `AxlLoop` |
| **Notification** — "how do I tell others?" | [`AxlCancellable`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) | Stop token shared across async ops; cancel once, many ops abort | typed wrapper over `AxlEvent`; ops register its handle on their loop |
| | [`AxlPubsub`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) | Pub/sub bus — decoupled, many subscribers | delivery is deferred via `AxlDefer` on the caller's loop |
| | Direct callback | Coupled point-to-point | caller-defined |
| | `AxlEventHandle` + `axl_event_signal` | Hand a raw UEFI event to `axl_loop_add_event`; fire via `axl_event_signal(e)` | foreign-event interop (TCP completion tokens, protocol-notify) |
| **Work offload** — "run where?" | [`AxlTask`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/task/README.md) pool | Real parallelism on APs (other cores); falls back single-core | AP dispatch, polled via `axl_task_pool_poll()` |
| | [`AxlAsync`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/task/README.md) | Fire-and-forget AP work with a BSP callback | registers an idle source on the caller's loop (natural under `axl_loop_run`; under an `axl_yield`-driven main, see [`AXL-Runtime.md §2.6`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md)) |

## Decision guide

Pick by what you need to do, not by what primitive looks closest:

| I need to... | Use |
|---|---|
| Run code every N ms | `axl_loop_add_timer` |
| Run code once after a delay | `axl_loop_add_timeout` |
| React to keyboard input | `axl_loop_add_key_press` |
| Do background work between events | `axl_loop_add_idle` |
| React when a UEFI protocol appears | `axl_loop_add_protocol_notify` |
| Integrate a firmware-owned EFI_EVENT | `axl_loop_add_event` |
| Integrate an AxlEvent I own | `axl_loop_add_event(loop, axl_event_handle(e), ...)` |
| Run code safely from a constrained context | `axl_defer` |
| Let my async callback wake the main thread | `AxlEvent` + `axl_event_wait_timeout` |
| Let a caller abort any number of async ops | `AxlCancellable` |
| Poll a hardware status register (CPU idles) | `axl_wait_for_word` / `axl_wait_for_flag` |
| Interruptible sleep | `axl_wait_ms` |
| Wait on a complex condition, driving a state machine | `axl_wait_for_with_tick` |
| Decouple two modules with named events | `AxlPubsub` |
| Offload CPU-heavy work to another core | `AxlTask` / `AxlAsync` |

## Why this model, not another

AXL's shape is **event loop + callbacks + stop-tokens**. That's the
model Node.js (libuv), Nginx, pre-async Python asyncio, libev, and
GLib all chose — the standard answer for cooperative I/O
concurrency in a single-threaded runtime. This section records the
alternatives considered and why they don't fit UEFI.

### Why not Python's GIL model

The GIL is a *lock* that exists only because CPython has real OS
threads and needs to serialize interpreter state. UEFI has no OS
threads on the BSP, so there is nothing to lock. The GIL is a
workaround born of legacy threading; borrowing the name without
the problem would add ceremony without value. Where AXL does touch
real parallelism (APs via `AxlTask`), the primitive is an explicit
submit / poll queue — no shared mutable state, no lock needed.

### Why not stackful coroutines (fibers / green threads)

Each coroutine gets its own stack. A firmware app might juggle 20
async ops; at 16 KB/stack that's 320 KB, on a system where every
KB matters. Debuggers choke on stack swaps. Lifetime management
(what owns the coroutine, when is it reaped) would become a whole
new story AXL doesn't have today. The memory and complexity cost
doesn't buy enough.

### Why not stackless coroutines / protothreads

Macro tricks (`switch`/`case` / computed goto) to fake yield points.
Local variables don't survive yields, so porting existing C code is
painful and error-prone. Debugging a protothread is reading a
generated state machine by hand. Worth considering if the codebase
were greenfield and async flows were deep — ours are rarely more
than three callbacks deep.

### Why not `async`/`await` via macros

C doesn't have it natively. Adding a macro-based emulation (a la
[libasync](https://github.com/naasking/async.h) or
[asyncify](https://github.com/kprotty/zap)) buys brevity at the
cost of legibility and surprises UEFI developers. The counterweight
in AXL is the **sync wrappers** built on top of the callback
primitives: `axl_tcp_connect`, `axl_http_get`, `axl_wait_*`,
`axl_event_wait_timeout`. They let the common flat case be written
synchronously; callback nesting only appears where truly async
composition is needed. That's the right pressure valve for firmware.

### Where this breaks down

Three-level async flows (`connect → TLS handshake → HTTP request`)
become scope soup. The sync wrappers are the near-term answer. If a
concrete pain point surfaces later, a thin `AxlFuture` / promise
layer on top of `AxlEvent` could compose them with `.then()` /
`.all()`. Don't build it speculatively.

## Where the primitives live

```
src/loop/        dispatch       axl-loop.c, axl-defer.c, axl-pubsub.c
src/event/       coordination   axl-event.c, axl-cancellable.c, axl-wait.c
src/task/        offload        axl-task-pool.c, axl-async.c, axl-buf-pool.c
```

This layout is intentional: each directory corresponds to one axis
of the taxonomy. Adding a new concurrency primitive? Pick an axis.
If it doesn't fit any of the four, reconsider whether the primitive
earns its weight.

## Background reading

- [GLib `GMainLoop`](https://docs.gtk.org/glib/main-loop.html) — the
  closest cousin. Same shape: loop + sources + `GCancellable`.
- [libuv design](http://docs.libuv.org/en/v1.x/design.html) — the
  single-threaded event loop behind Node.js.
- [Python asyncio pre-await era](https://docs.python.org/3/library/asyncio-protocol.html#callback-based-apis) — protocol + transport callbacks. What `async`/`await` replaced.
- Linux kernel `struct completion` ([docs](https://www.kernel.org/doc/Documentation/scheduler/completion.txt)) — historical name for what AXL calls `AxlEvent`.
- [C++ `std::latch`](https://en.cppreference.com/w/cpp/thread/latch) — closest
  C++ analogue of `AxlEvent`'s one-shot latch semantics.
