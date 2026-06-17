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

**Re-entrancy / nesting** — the one recurring failure mode of this model,
*blocking on a loop that is already running*, and its remediation are tracked
in [`AXL-Loop-Reentrancy-Plan.md`](AXL-Loop-Reentrancy-Plan.md). How the model
is kept honest under test (the topology that surfaced those bugs) is in
[§ Testing the model](#testing-the-model) below.

**Also see:** [`AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md) — the runtime
services that ship around `main` today (Phase A7, April 2026):
lazy default loop, Linux-style signal handling, `axl_yield()` for
tight loops, `axl_atexit` for cleanup, tier-1 resource registry +
sweep. CRT0 invokes the runtime via `_axl_init` / `_axl_cleanup`;
the runtime, not CRT0, owns the state. Apps that run a tight CPU
loop can stay Ctrl-C responsive and fire registered timers by
calling `axl_yield()` alone, without ever
calling `axl_loop_run`; see [AXL-Lifecycle.md §2.4](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#24-tight-loop--yield--timer-worked-example) for the worked
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
| | [`AxlAsync`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/task/README.md) | Fire-and-forget AP work with a BSP callback | registers an idle source on the caller's loop (natural under `axl_loop_run`; under an `axl_yield`-driven main, see [`AXL-Lifecycle.md §2.6`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md)) |

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
become scope soup. The sync wrappers are the right answer **for the
no-loop case** (CLI tools — `fetch`, `netinfo`, ping — own no main
loop, so a sync wrapper's throwaway loop *is* the only loop). If a
concrete pain point surfaces later, a thin `AxlFuture` / promise
layer on top of `AxlEvent` could compose them with `.then()` /
`.all()`. Don't build it speculatively.

**Superseded for long-running reactive services.** The original
stance here — "the sync wrappers are the near-term answer" — held up
for tools but proved to be the *pain point* for a service that runs a
loop (a server, a resident driver pump). A sync wrapper called from a
loop callback blocks on a loop that is already running, which nests an
ephemeral loop and wedges; the SoftBMC port hit this repeatedly
(`e90b87e4`, `d249a9b6`, `adbf5461`, the TLS resident-loop hang). The
remediation — services are async-first; library code never implicitly
re-enters the consumer's dispatch; sync APIs become self-protecting
(loud `AXL_BUSY`, not a silent wedge) — is the subject of
[`AXL-Loop-Reentrancy-Plan.md`](AXL-Loop-Reentrancy-Plan.md), which
this section now defers to.

## Extending the model: which APIs go async, and when

The async HTTP/DNS/UDP work generalized into a reusable shape: **an op that
*blocks waiting on external hardware/firmware completion* gets an async core
driven on the caller's `AxlLoop`, and the sync entry point becomes a thin
ephemeral-loop wrapper over it** (one I/O implementation, both faces public).
The same pattern applies well beyond networking — but only to ops that actually
*wait*. CPU-bound work (hashing, sort, JSON, formatting, table parsing, GOP
Blt) has nothing to defer in a single-threaded environment; cooperative
yielding (`_axl_poll_break`) is its only lever, and it is already wired.

**Already split:** the whole net stack, and serial (`axl_serial_read_async`, a
timer-poll source).

**Candidates, by value — build each *on demand* (when a consumer needs it), the
way the async HTTP client was un-deferred by SoftBMC's webhook. Do NOT build
them speculatively (see "Where this breaks down" and the AxlFuture note).**

| API | How it blocks today | Async shape | Demand |
|---|---|---|---|
| **IPMI / BMC** (`AxlIpmi`) | KCS/SSIF **busy-poll** the BMC (`axl_backend_stall` loop) for the response — ms to seconds | Pure Poll-tick reuse: submit, tick the KCS/SSIF FSM from a loop timer, callback on completion. No firmware event needed; same shape as the DNS4/TCP4 Poll ticks | **On SoftBMC's roadmap** — a BMC issuing/polling IPMI from an HTTP handler or timer hits the webhook's blocking-from-a-callback wall. Cleanest to build (no new infrastructure). |
| **Storage** (`AxlNvme`/`AxlAta`/`AxlScsi`/`AxlBlock`) | PassThru passes `Event = NULL` (sync); BlockIo used over BlockIo2 | The PassThru protocols are async-capable via that `Event`; `EFI_BLOCK_IO2` has token/event reads. Submit with an event, register it on the loop, callback on completion | **On SoftBMC's roadmap.** Poster child: **device self-test** (runs for *minutes* — poll progress) + SMART polling + large reads, run on the service loop while it serves. |
| **MP Services** (parallel AP dispatch) | `StartupAllAPs` blocking mode | Non-blocking mode takes a `WaitEvent` — register it, callback when all APs finish | Aspirational (a consumer fanning work across APs). Lower urgency: callers usually *want* to block until the fan-out completes. |
| **USB transfers** (`AxlUsb`) | sync bulk/interrupt | `EFI_USB_IO` async interrupt transfers are callback-based | Niche — live device I/O (HID polling), not the enumeration AxlUsb mostly does. |
| **TPM** (`AxlTpm`) | TCG2 submit/response | event/poll | Low — usually fast enough that blocking is fine. |

**Not candidates (CPU-bound, no external wait):** mem, format, log, all of data
(hash/sort/json/str/trees), gfx draw + compositor, smbios/acpi/pci/spd +
usb-enumeration, rng, time, fs metadata, path.

When one of these is built, mirror the net contract exactly: `AXL_OK` ⇒ the
callback fires later (deferred, never re-entrant); one op in flight per
handle → `AXL_BUSY`; the callback owns the result; the sync wrapper drives a
Poll tick at raised TPL and clears any per-op loop state before freeing the
ephemeral loop. See `src/net/axl-http-client-async.c` + `axl-tcp-async.c` for
the reference implementation and [AXL-Async-HTTP-Plan.md](AXL-Async-HTTP-Plan.md)
for the contract + review findings.

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

## Testing the model

The concurrency bugs that slipped past the unit suite and surfaced
only when SoftBMC integrated — the WS-teardown wedge (`e90b87e4`),
the WS-over-TLS stream desync (`4563aabf`), the second-server-on-a-
shared-loop dead-accept (`adbf5461`), the TLS resident-loop handshake
hang — share one trait: they live in an **execution topology the
app-shaped tests don't model.** Unit tests run as a standalone app:
one loop, `TPL_APPLICATION`, run-to-completion. SoftBMC runs the
**resident-driver model** — the loop is pumped from an
`axl_loop_attach_driver` tick at raised TPL (`TPL_CALLBACK`), several
servers share one loop, and WS handlers themselves do I/O. Whole bug
classes live only in that gap, so the one consumer that runs that
shape became the de-facto integration test (the multi-day back-and-
forth).

The strategy is to model the topology in-repo and assert the
invariants, so the SDK breaks first — not the consumer. Five parts:

1. **Consumer-emulator harness** — `test-consumer-emulator-qemu.sh`
   over the `AxlTestNet serve-hazard-driver` mode: HTTP **and** HTTPS
   on ONE `attach_driver`-pumped shared loop, with a per-client WS
   endpoint whose handlers send / broadcast / close. This is the
   canonical hazardous shape (it combines what `serve-multi-tls` and
   `serve-tls-ws-driver` exercise separately). New net/loop behavior
   runs through it; most of the escaped bugs reproduce here.
2. **Invariant asserts** — `AXL_DEBUG_ASSERT` (see
   [include/axl/axl-debug.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-debug.h);
   loud in debug/test builds, compiled out under `NDEBUG`) catches the
   *cause* at the fault site instead of the symptom downstream:
   - **re-entrancy** — a synchronous wait nested inside a loop callback
     (the warn-guard `_axl_loop_in_callback()`, Loop-Reentrancy-Plan
     Item 1);
   - **TLS write ordering** — never advance the TLS sequence number
     (`mbedtls_ssl_write`) while a TCP send is already in flight (the
     `4563aabf` desync).

   The `adbf5461` source-id collision was *also* first guarded this way,
   but it has since been fixed at the root instead (see part 5) — a
   reminder that an assert is a stopgap until the hazard can be made
   unrepresentable.
3. **Raised-TPL coverage** — the driver-pump harness exercises every
   scenario at raised TPL, not just `TPL_APPLICATION`. This is the gap
   most of the bugs hid in, and it is inherent to the `attach_driver`
   harness above (no separate test axis needed).
4. **Liveness watchdog** — every harness scenario ends with a probe
   request, and the loop must answer it within a bounded time. These
   bugs manifest as a *wedge*, so a positive liveness probe after each
   scenario turns a hang into a named failure. (The unit harness
   already names a stalled binary via `*** STALLED:` + the ratchet
   "Culprit:" line; this is the integration-side equivalent.)
5. **Make the hazard unrepresentable** — the deepest fix: eliminate the
   bug class structurally rather than guard each instance. Concrete win:
   loop source ids are now allocated from a single **process-global**
   counter, not a per-loop one (`axl-loop.c`). Per-loop ids put the same
   id on every loop, so an id outliving its loop could be removed from a
   different loop and delete an unrelated source (the `adbf5461`
   dead-accept). With globally-unique ids a stale id matches nothing on
   another loop — the cross-loop removal is a no-op and the class is gone,
   which let the sync wrappers' collision-guard id-clearing and its
   asserts be removed (one recv clear remains, now purely next-recv
   hygiene, not a collision guard). The same instinct drives the
   [Loop-Reentrancy-Plan](AXL-Loop-Reentrancy-Plan.md): where a context
   tolerates only async I/O (a handler under a running loop, a driver pump
   at raised TPL), make sync I/O impossible or loud (Item 1 guard →
   async-first handlers → flip to hard `AXL_BUSY`) rather than rely on a
   test to catch it.

Parts 1, 3, 4 are the test apparatus; parts 2 and 5 are the
detection/design that the Loop-Reentrancy-Plan drives. Together they
close the topology gap that made SoftBMC the integration test.

## Background reading

- [GLib `GMainLoop`](https://docs.gtk.org/glib/main-loop.html) — the
  closest cousin. Same shape: loop + sources + `GCancellable`.
- [libuv design](http://docs.libuv.org/en/v1.x/design.html) — the
  single-threaded event loop behind Node.js.
- [Python asyncio pre-await era](https://docs.python.org/3/library/asyncio-protocol.html#callback-based-apis) — protocol + transport callbacks. What `async`/`await` replaced.
- Linux kernel `struct completion` ([docs](https://www.kernel.org/doc/Documentation/scheduler/completion.txt)) — historical name for what AXL calls `AxlEvent`.
- [C++ `std::latch`](https://en.cppreference.com/w/cpp/thread/latch) — closest
  C++ analogue of `AxlEvent`'s one-shot latch semantics.
