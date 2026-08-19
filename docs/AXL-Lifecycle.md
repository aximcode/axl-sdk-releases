# AXL Lifecycle

This doc describes the **program lifecycle** — the arc from
firmware entry through `main` to cleanup and exit, and the
services that live around `main`: a default event loop,
Linux-style Ctrl-C handling, `axl_yield()` as a first-class
cooperative escape hatch, atexit, and a tier-1 resource sweep on
exit. It also calls out the hard limits we can't paper over
(UEFI BSP has no preemption).

A few items from the original design — release-mode heap sweep
and an opt-in watchdog — remain deferred and are called out in
[§10](#deferred-items). The history of how the lifecycle landed
(Phase A7, April 2026, commits `3789aea`...`4368256`) and the
decisions locked in along the way are kept for posterity in
[§9](#design-decisions-locked-in) and the
[Appendix](#appendix-decision-log).

## Where things live

It's easy to muddle "CRT0" and "the runtime" because both run
around `main`. They are different layers:

| Layer | Source | Scope |
|---|---|---|
| **CRT0** (the entry stub) | [`src/crt0/axl-crt0-native.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/crt0/axl-crt0-native.c) — ~17 lines | Bridges UEFI's `_AxlEntry(ImageHandle, SystemTable)` to `int main(argc, argv)`. Sets `gST`/`gBS`/`gRT`, calls `_axl_init`, calls `main`, calls `_axl_cleanup`. Owns nothing beyond the firmware-table globals. |
| **The AXL runtime** | [`src/runtime/`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/runtime/) — `axl-runtime.c`, `axl-signal.c`, `axl-atexit.c`, `axl-registry.c` | The library invoked by CRT0 at the boundary calls. Owns the default-loop singleton, the atexit registry, the signal subsystem, the tier-1 resource registry, and the cooperative yield mechanism. |
| **Public API** | [`<axl/axl-runtime.h>`](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-runtime.h), [`<axl/axl-signal.h>`](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-signal.h), [`<axl/axl-atexit.h>`](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-atexit.h) | What apps call: `axl_loop_default`, `axl_yield`, `axl_signal_install`, `axl_atexit`, `axl_exit`, `axl_interrupted`. |
| **Loop primitives** | [`src/loop/`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/), [`<axl/axl-loop.h>`](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-loop.h) | Independent module. The runtime owns the *default-loop singleton*, but loop semantics (source kinds, dispatch, nested wait) live in the loop module's own design. This doc refers out to it. |

When this doc says **"CRT0 invokes X"** it means the entry stub
calls `_axl_init` / `_axl_cleanup`. When it says **"the runtime
owns X"** it means the implementation lives in `src/runtime/` and
travels with the library. CRT0 doesn't *own* the default loop or
the atexit registry — the runtime does, and CRT0 wakes it up.

The shorthand "the runtime" without further qualification refers
to the in-process services, not to UEFI Runtime Services (`gRT`)
or to the language runtime — three different "runtimes" we have
to keep straight. Where the distinction matters this doc is
explicit.

Related reading:
- [`AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md) — the four-axis
  primitive taxonomy (dispatch / coordination / notification /
  offload). This doc proposes *the runtime under those primitives.*
- [`AXL-Design.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Design.md) — overall library architecture.
- [`AXL-SDK-Design.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-SDK-Design.md) — CRT0 / `axl-cc` / entry
  point flow.
- [`src/loop/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) — the loop module's
  own reference; this doc only covers the parts of the loop that
  the runtime touches (default-loop singleton, nested wait).

---

## 1. Motivation

Today, Ctrl-C handling is scattered and cooperative-in-a-bad-way:

- The event loop observes the shell break event and returns -1.
- `axl_wait_*` / `axl_event_wait_*` map that to `AXL_CANCELLED`.
- Every caller has to notice the magic return code and unwind.
- Apps that don't use these primitives (pure CPU loops, or naive
  reads from a file) aren't interruptible at all.
- There is no centralized "on Ctrl-C, clean up and exit" path;
  each app reinvents it.

Linux developers reaching for AXL will expect:

1. `Ctrl-C` ends the program by default.
2. A signal-install API for apps that want custom cleanup.
3. Long-running operations feel responsive to interruption.
4. Resources get freed when the program exits.

We can't give them POSIX signals — UEFI BSP has no preemption, a
tight CPU loop is inherently uninterruptible. But we can give them
**a cooperative runtime that feels Linux-shaped** for any app that
uses AXL APIs, which is effectively all of them (consumers link
against `libaxl.a` for almost everything — printf, malloc, file
I/O, networking, all yield through AXL).

The key insight: **we control every AXL API**. If every slow API
checks a flag, the app gets Linux-like responsiveness *without*
needing preemption.

## 2. Lifecycle model

### 2.1 Who owns what

```
_AxlEntry  (CRT0 entry stub, src/crt0/)
  ├─ set gST / gBS / gRT from firmware
  ├─ _axl_init()                                  → enters runtime
  │    ├─ initialize memory, console, backend
  │    ├─ install shell-break notify → sets g_axl_interrupted
  │    ├─ initialize tier-1 resource registry
  │    ├─ initialize atexit registry
  │    └─ walk .init_array → fires C++ global constructors
  │    (UEFI watchdog / livelock guard: deferred — see §10.2)
  ├─ _axl_get_args() → argc/argv
  ├─ main(argc, argv)                             ← app runs here
  └─ _axl_cleanup()                               → re-enters runtime
       ├─ run atexit callbacks in reverse order
       │  (includes C++ static destructors registered via
       │   __cxa_atexit, which routes through axl_atexit)
       ├─ axl_loop_free(default_loop) if one was created
       ├─ sweep tier-1 registry (close leaked events/loops/...)
       └─ memory leak report (AXL_MEM_DEBUG)
```

The **runtime** owns: the break notify, the atexit registry, the
tier-1 resource registry, the watchdog timer. Those live from
`_axl_init` through `_axl_cleanup`. CRT0 invokes the runtime at
both boundaries but holds none of the state itself.

### 2.1.1 C++ static initializer timing

UEFI doesn't run `.init_array` automatically — the firmware loader
just transfers control to `_AxlEntry` and lets the binary handle
its own startup. axl-sdk's `_axl_init` walks the array as its last
step, so C++ global constructors fire **after** memory / console /
registry / atexit are all live.  This means a `static MyClass thing;`
declaration in a `.cpp` source can safely call `axl_malloc`,
`axl_printf`, `axl_atexit`, etc. from its constructor.

C++ destructors register via `__cxa_atexit`, which the runtime
routes through `axl_atexit` — so `static` dtors and explicit
`axl_atexit` callbacks share the same LIFO drain during
`_axl_cleanup`.  See `src/runtime/axl-cxxabi.c` for the shims.

Pure-C apps pay zero cost: the `.init_array` walk loop body never
executes (the linker-script-emitted bounds are equal when no C++
source contributes a static initializer), and `__cxa_atexit` is
only referenced from compiler-generated C++ code.  The
`axl-cxxabi.o` symbols ship in `libaxl.a`; archive selection means
they're only pulled in when something references them.

For the full C++ subset / toolchain story (which freestanding
libstdc++ headers work, what's forbidden, axl-c++ vs axl-cc) see
[`AXLMM-Design.md` §"Toolchain & constraints"](AXLMM-Design.md#toolchain--constraints).

The **default loop is *not* eagerly created** — it is a lazy
singleton inside the runtime, materialized the first time any
code calls `axl_loop_default()` and freed during `_axl_cleanup`
if anyone created it.

The **app** owns anything it allocates. It can register
`axl_atexit` handlers to free them automatically when `main`
returns or when an interrupt drives `axl_exit`.

### 2.2 Signal subsystem

Shell break handling moves out of `axl_loop_run` and the wait
helpers. Instead:

- The runtime registers a **notify callback** on the shell break
  event during `_axl_init` (called from CRT0).
- The notify sets `g_axl_interrupted = true` and invokes any user
  handler registered via `axl_signal_install`.
- Default policy (no handler installed): interrupted flag is set,
  next yield point observes it and initiates clean exit
  (`_axl_cleanup` + `gBS->Exit`).

Public API:

```c
/* Signal handler runs in a limited context — set flags, log,
 * return. Do not allocate, do not call Boot Services that mutate
 * state. Any cleanup should happen at the next yield point or in
 * an axl_atexit handler. */
typedef void (*AxlSignalHandler)(void);

void axl_signal_install(AxlSignalHandler on_interrupt);
void axl_signal_default(void);               /* restore auto-exit */
bool axl_interrupted(void);                  /* poll the flag */
```

Rationale: matches Linux `signal(SIGINT, handler)` shape while
acknowledging the UEFI constraint that handlers run at raised TPL
and can't do much. In practice, a handler typically sets a
per-app "please unwind" flag and returns; the main thread's next
yield exits through the normal path.

**Naming note.** The `axl_signal_*` prefix was previously occupied
by a GObject-style pub/sub bus. Pre-1.0, that bus is renamed to
`axl_pubsub_*` in [axl-pubsub.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-pubsub.h) (~90
identifiers across 14 files; mechanical rename) specifically to
free up the `axl_signal_*` namespace for this POSIX-flavored
interrupt API — the meaning users' muscle memory reaches for
first. "Break" remains in the internal plumbing (backend helpers
`axl_backend_shell_break_event` / `axl_backend_shell_break_flag`,
UEFI's own "ExecutionBreak") because that's the mechanism-level
name of the firmware event. "Signal" is what the API surface
offers the app author. See [§9](#design-decisions-locked-in) and [Appendix](#appendix-decision-log).

### 2.3 The default loop

`axl_loop_default()` is a **lazy singleton**:

- `_axl_init` never touches it — neither CRT0 nor the runtime's
  init path calls `axl_loop_default()`, so until user code asks
  for it the loop doesn't exist.
- The first caller of `axl_loop_default()` materializes the loop
  via `axl_loop_new()`; subsequent callers get the same handle.
- When the loop exists, `_axl_cleanup` frees it during teardown
  (skipping the sweep warning the registry would otherwise emit).

Apps interact with it in three shapes, each of which is "on" for
different pieces of behavior:

1. **Never ask for it.** Pure CPU tool apps (`hello`, `cat`, a
   straight-line digest). The singleton stays NULL. `axl_yield()`
   still works — it detects `mDefaultLoop == NULL` and polls the
   shell-break flag directly — so Ctrl-C still routes through
   `axl_exit`. No loop overhead, no source machinery, zero setup.

2. **Materialize it for passive dispatch.** Register timers /
   timeouts / defers on it, then run a synchronous CPU loop that
   calls `axl_yield()` on each iteration. You **do not** call
   `axl_loop_run`. Every yield calls `axl_loop_dispatch(loop,
   blocking=false)`, which walks registered sources via UEFI's
   `CheckEvent` and fires any whose event has signaled since the
   last yield. Timers with elapsed intervals fire in line; nothing
   else fires. This is the shape the next subsection documents.

3. **Run it explicitly.** Call `axl_loop_run(axl_loop_default())`
   from `main` when the app's primary role is event-driven
   (`http-server.c`, `echo-server.c`). The loop is live; yield
   points become redundant because every source is serviced on
   the blocking-wait path.

All three are valid. Picking one is about what main is for — CPU
work with side timers, pure event-driven service, or neither.
Nested loops are a real concern — see [§5](#nested-loops).

**Why the singleton exists at all** (honest accounting). As of
Phase A7 the default loop carries exactly one live responsibility:
it is the scheduler that `axl_yield()` dispatches when someone has
registered a source on it. That single integration is what makes
the shape-2 pattern ([§2.4](#tight-loop-yield-timer-worked-example)) possible — without it, `axl_yield()`
reduces to "poll the break flag and maybe `axl_exit`," and the
tight-loop-with-timer pattern does not work. Everything else the
singleton *could* be used for (library-internal background work,
watchdog pets, ambient periodic reports) is option value that
nothing in-tree has spent yet. No production AXL library code
calls `axl_loop_default()` today; `AxlAsync`, HTTP, TCP, and the
sync waits all either take an explicit `AxlLoop *` from the
caller or spin up throwaway loops.

Practical consequence: if your app never enters shape-2 or
shape-3 and never materializes the singleton, the default loop is
zero bytes, zero cycles, zero firmware events. The runtime
module's footprint in that case is the break-notify, the atexit
list (empty until you register), and the tier-1 registry
(tracking whatever you allocate). Dropping `axl_loop_default()`
entirely was considered; keeping it costs nothing when unused and
preserves the `axl_yield`-as-scheduler design, which is the
cornerstone of cooperative interruptibility for tight CPU loops
([§3](#axl-yield-cooperative-escape-hatch), [§2.4](#tight-loop-yield-timer-worked-example)).

### 2.4 "Tight-loop + yield + timer" worked example

The pattern that comes up most often and *was not obvious from the
earlier revision of this document*:

```c
static bool on_tick(void *d) {
    (void)d;
    axl_printf("tick\n");
    return AXL_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Materializes the default loop and registers a 500 ms timer.
       axl_loop_run is NEVER called. */
    axl_loop_add_timer(axl_loop_default(), 500, on_tick, NULL);

    size_t result = 0;
    for (size_t i = 1; i < 1000000000; ++i) {
        result += do_work(result);
        axl_yield();   /* services the timer + observes Ctrl-C */
    }

    axl_printf("Result: %zu\n", result);
    return 0;
}
```

What happens at runtime:

- Every iteration of the `for` loop calls `axl_yield()`.
- `axl_yield` sees `mDefaultLoop != NULL` and dispatches the loop
  non-blocking: one pass through all registered sources, each
  checked with UEFI's `CheckEvent`. No waiting, no blocking.
- When the 500 ms timer event has signaled since the last pass,
  its callback runs in line and prints `tick`; otherwise the
  dispatch returns in a few hundred nanoseconds.
- On Ctrl-C the dispatch detects the shell-break event, sets
  `g_axl_interrupted`, and `axl_yield` calls `axl_exit(1)`.
  `_axl_cleanup` runs (atexit callbacks, registry sweep, leak
  report) and the program terminates without executing the
  `Result:` print.

**The key property:** you do not need `axl_loop_run` for the
default loop to be useful. You need it only when you want the
loop itself to be the top-level driver — as in an HTTP server
where `main` registers handlers and hands control over.

### 2.5 Which sources fire under passive dispatch vs running

Not all source types behave the same when the loop is serviced
via `axl_yield` (non-blocking dispatch) versus `axl_loop_run`
(blocking wait). Keep this in mind when picking a source.

| Source | `axl_yield` dispatch (passive) | `axl_loop_run` (active) |
|---|---|---|
| **Timer** (`axl_loop_add_timer`) | fires when its interval has elapsed since the previous check — bounded by wall clock, not yield rate | same |
| **Timeout** (`axl_loop_add_timeout`) | fires once at its deadline, then self-removes | same |
| **Raw event** (`axl_loop_add_event`) | fires when the underlying `EFI_EVENT` is signaled (TCP completion tokens, protocol notifications, `AxlEvent`, cancellables) | same |
| **Defer** (`axl_defer`) | drained before source checks — pending defers run each yield | same |
| **Idle** (`axl_loop_add_idle`) | **fires every yield, including inside tight CPU loops** — see [§2.6](#idle-callbacks-and-yield-driven-loops) | fires every loop iteration (unbounded frequency) |
| **Key press** (`axl_loop_add_key_press`) | checks the console non-blocking; a pressed key dispatches | polls on each wakeup |
| **Protocol notify** | fires when the watched protocol is installed | same |

The only footgun in this table is **idle**; see next subsection.

### 2.6 Idle callbacks and yield-driven loops

`axl_loop_add_idle` registers a callback that runs on *every*
`axl_loop_next_event` pass, whether blocking or non-blocking. In
a normal `axl_loop_run` that's fine — one pass per `WaitForEvent`
wakeup, naturally throttled. In a tight-loop + yield app, that's
one idle invocation per loop iteration, possibly millions per
second. Almost always not what the caller intended.

**Rule of thumb:** if you're writing a tight CPU loop with
`axl_yield`, do not register idle sources on the default loop.
Reach for a `axl_loop_add_timer` with an explicit interval, an
`axl_defer` for one-shot soon-after-now work, or `axl_yield`
itself if the goal is "make my loop interruptible." Idle should
be reserved for apps that truly need "do this whenever the loop
has no higher-priority work" — a model that only makes sense
when `axl_loop_run` is the top-level driver.

Whether to change idle's semantics so it skips non-blocking
dispatch passes (firing only under `axl_loop_run` proper) is an
open design question; see `docs/ROADMAP.md`.

## 3. `axl_yield()`: cooperative escape hatch

The public API:

```c
/**
 * @brief Cooperative yield point.
 *
 * Call inside tight loops to make them interruptible AND to
 * service the default loop without committing to axl_loop_run.
 * Per call, in order:
 *
 * 1. If the default loop has been materialized (someone called
 *    axl_loop_default() — typically to register a timer, defer,
 *    or raw event), dispatch it non-blocking for one pass:
 *    elapsed timers fire, pending defers drain, signaled raw
 *    events dispatch their callbacks, shell-break is observed.
 *
 *    If the default loop has NOT been materialized, directly
 *    poll the shell-break flag. Keeps pure CPU-loop apps
 *    interruptible without paying for a loop they never asked
 *    for.
 *
 * 2. If axl_interrupted() is now true (because step 1 saw the
 *    break event) and no user signal handler is installed,
 *    axl_exit(1) runs -- _axl_cleanup fires atexit callbacks,
 *    sweeps the registry, and exits. A user handler that
 *    returns normally lets axl_yield return; the caller can
 *    react via axl_interrupted().
 *
 * Cost: ~nanoseconds when nothing fires (one flag read or one
 * CheckEvent per registered source). Safe from any context
 * except raised-TPL notify handlers.
 */
void axl_yield(void);
```

### 3.1 Where AXL APIs inject yields automatically

Every AXL public API that can take noticeable time should call
`axl_yield()`. The guideline:

> If the function can execute for longer than a few microseconds
> under reasonable inputs, and it doesn't already use `axl_loop_*`
> internally, instrument it with `axl_yield()`.

| Area | Functions | Pattern | Status |
|---|---|---|---|
| **File I/O** | `axl_file_get_contents`, `axl_file_set_contents` | yield at entry | **landed** |
| **HTTP upload/download** | `axl_http_get` body-read loop | yield per chunk | **landed** |
| **Data operations** | `axl_checksum_update` (MD5/SHA-1/SHA-256), `axl_array_sort`, `axl_array_sort_with_data` | chunk + yield per 64 KiB digest, per 1024 sort iters | **landed** |
| **IPMI KCS** | `kcs_wait_ibf_clear`, `kcs_wait_obf_set` | yield every 100 poll iters (~10 ms) during a stuck-BMC 5 s poll | **landed** |
| **Network blocking** | Already use `AxlLoop`; break observed via `axl_loop_next_event` | no extra yield needed | n/a |
| **Task pool polling** | Already loop-driven | no extra yield needed | n/a |
| **Format / printf**, **directory iteration**, **SSIF inter-command delay** | Low measured impact so far | — | deferred |

Not worth instrumenting:

- O(1) or short-O(log n) operations (hash-table insert, list push,
  str-copy-small) — overhead would dwarf the work.
- Pure arithmetic helpers.
- Anything under a few µs typical.
- SMBIOS walk — the table is typically under 5 KiB total.

### 3.2 App code using `axl_yield`

```c
int main(int argc, char **argv) {
    /* CPU-heavy scan with no AXL calls in the hot loop */
    for (size_t i = 0; i < huge; i++) {
        crunch(&state, i);
        if ((i & 0xFFF) == 0) axl_yield();   /* every 4k iterations */
    }
    return 0;
}
```

Callers choose their own cadence. AXL never demands a minimum —
it's the same contract Rust's `.await` and Node's microtask queue
expose: "the runtime can act at your yield points, and only there."

## 4. Resource cleanup when `main` returns

### 4.1 UEFI vs POSIX exit semantics

This is where UEFI diverges sharply from Linux. On Linux, when
`main` returns or the process calls `exit()`, the kernel reclaims
the entire address space — heap, file descriptors, signal
registrations, everything. Sloppy programs don't crash the OS;
they just waste memory until exit.

**UEFI has no process model.** There is no per-application address
space. There is no teardown. When an AXL app returns, control
flows back to the Shell (or BDS), which has no knowledge of what
the app allocated. Specifically:

| Resource | On Linux `exit()` | On UEFI app return |
|---|---|---|
| Heap (`axl_malloc`) | kernel reclaims | **leaks until reboot** — each allocation is a separate `gBS->AllocatePool` call from a firmware-global pool |
| `EFI_EVENT` / `AxlEvent` | closed | **crash hazard** — firmware keeps the event registered; if a later `SignalEvent` calls a notify function whose code pages were unloaded with your image, system crashes post-exit |
| Installed protocols | N/A | **crash hazard** — firmware holds the vtable forever |
| File handles | closed | filesystem driver keeps state pinned |
| Loaded child images | N/A | stay in memory |
| UEFI variables, network handles, registered callbacks | N/A | all leak |

The **firmware-facing** resources (events, protocols, registered
callbacks) are the dangerous class. A crash two minutes after the
app exits — triggered by a timer firing into unloaded code — is
one of the harder UEFI bugs to diagnose.

Today `_axl_cleanup` ([src/posix/axl-app.c:92](https://github.com/aximcode/axl-sdk-releases/blob/main/src/posix/axl-app.c#L92))
only:
1. Frees the argv/argc it allocated in `_axl_init`.
2. Under `AXL_MEM_DEBUG`, dumps the leak report — a
   **diagnostic report**, not cleanup. It names what leaked; it
   doesn't free anything. (Since the leak gate landed this is
   `_axl_mem_dump_leaks_at_exit()`, whose header omits the
   `(live allocations)` infix that the public `axl_mem_dump_leaks()`
   carries; the QEMU harness fails a run on the former.)

Phase A7 fixes this by making the library responsible for
firmware-facing resources it handed to the user, and for running
a guaranteed cleanup path on every exit type.

### 4.2 The internal resource registry

Design principle: **every library function that creates a
firmware-facing resource registers it. On exit, a sweep closes
whatever's left.** This is not garbage collection or refcounting —
it's a safety net for sloppy app code.

#### Two-tier policy

**Tier 1 — firmware-facing *or* container-owned (always tracked,
always swept).**

| Creator | What enters the registry | Removed by |
|---|---|---|
| `axl_event_new` | one event (crash hazard if leaked) | `axl_event_free` (removes before teardown) or `_axl_cleanup` sweep |
| `axl_cancellable_new` | the wrapped event | `axl_cancellable_free` or sweep |
| `axl_loop_new` | the loop + each internal event it creates | `axl_loop_free` or sweep |
| `axl_arena_new` | the arena (covers all sub-allocations inside it — see below) | `axl_arena_free` or sweep |
| (future) `axl_file_open`, `axl_http_client_new`, `axl_tcp_*` | respective handle | respective `_free` or sweep |

On sweep, each remaining entry's type determines its teardown
call. Sweep order is LIFO (reverse registration order), matching
`atexit` semantics and letting containers (loops) tear down
before their contents (events they registered as sources).

**Tier 2 — heap (`axl_malloc` et al.).**

`axl_malloc` already tracks every allocation under `AXL_MEM_DEBUG`
via a doubly-linked list (see
[src/mem/axl-mem.c:100](https://github.com/aximcode/axl-sdk-releases/blob/main/src/mem/axl-mem.c#L100)). Extend the
cleanup path:

- **Under `AXL_MEM_DEBUG`:** keep current behavior — report on
  cleanup, don't free. Dev sees bugs and fixes them.
- **In release builds:** walk the same list, `axl_free` each
  entry. Heap returns to the firmware pool cleanly. *(Status:
  deferred — see [§10.1](#release-mode-heap-auto-sweep). The tier-2 sweep is not wired in today;
  release builds rely on firmware reboot to reclaim pool memory.)*

Rationale: heap leaks waste memory but don't crash firmware.
Auto-freeing in debug would hide bugs; auto-freeing in release is
the production safety net. Tier 1 is different — leaks there can
crash the system, so safety wins in every mode.

**Arena sub-allocations** (`axl_arena_alloc`) do not produce
individual tracker entries — they're pure bump-pointer offsets
into the arena's backing buffer, not separate heap blocks. The
**arena itself** is what gets tracked (tier-1 registry above),
and freeing it reclaims every sub-allocation it handed out at
once. Callers who lean on `AxlArena` for scoped lifetimes get
implicit coverage: thousands of sub-allocations, one registry
entry, one sweep call clears them all.

### 4.2.1 Caller attribution for sweep warnings

Sweep warnings are most useful when they name **user-code**
file:line, not the library wrapper. Today, `axl_calloc` inside
`axl_arena_new` records `src/mem/axl-arena.c` as the alloc site —
technically accurate, practically useless for debugging.

Same trick the allocator already uses: the public APIs become
macros that capture `__FILE__` / `__LINE__` at the user call
site, forward to an `_impl` function that accepts them:

```c
/* include/axl/axl-arena.h */
#define axl_arena_new(cap)  axl_arena_new_impl((cap), __FILE__, __LINE__)
AxlArena *axl_arena_new_impl(size_t capacity, const char *file, int line);
```

Extend to `axl_event_new`, `axl_loop_new`, `axl_cancellable_new`,
and the future file/http wrappers. Sweep output goes from:

```
[WARN] runtime: 1 MB heap leaked  (src/mem/axl-arena.c:48)
```

to:

```
[WARN] runtime: auto-closing 1 leaked AxlArena  (main.c:17, 1 MB)
```

Much more actionable.

#### Registry structure (sketch)

```c
/* src/runtime/axl-registry.c (new under Phase A7) */

typedef enum {
    AXL_RES_EVENT,
    AXL_RES_LOOP,
    AXL_RES_FILE,
    /* grows as more library wrappers are added */
} AxlResourceKind;

/* Called by library wrappers in their new/free functions */
uint32_t _axl_registry_add(AxlResourceKind kind, void *resource,
                           const char *file, int line);
void     _axl_registry_remove(uint32_t handle);

/* Called from _axl_cleanup after user atexit handlers have run */
void     _axl_registry_sweep(void);
```

Each tier-1 wrapper changes from:
```c
AxlEvent *axl_event_new(void) {
    /* ...existing init... */
    return e;
}
```
to:
```c
AxlEvent *axl_event_new(void) {
    /* ...existing init... */
    e->_registry_handle = _axl_registry_add(AXL_RES_EVENT, e,
                                            __FILE__, __LINE__);
    return e;
}

void axl_event_free(AxlEvent *e) {
    if (e == NULL || e->magic != AXL_EVENT_MAGIC) return;
    _axl_registry_remove(e->_registry_handle);
    /* ...existing teardown... */
}
```

#### Sweep logging

When the sweep finds anything, loudly log it — the user's code
should be fixed, not silently rescued:

```
[WARN] runtime: auto-closing 3 leaked AxlEvent instances
   event@0x7FE12340  allocated at src/myapp.c:42 by axl_event_new
   event@0x7FE12380  allocated at src/myapp.c:58 by axl_loop_new
   event@0x7FE12400  allocated at src/myapp.c:91 by axl_tcp_accept_async
[WARN] runtime: 1024 bytes of heap auto-freed on exit (set
       AXL_MEM_DEBUG to get per-allocation detail)
```

Same pattern `axl_mem_dump_leaks` uses; just extend to tier-1
resources.

#### Double-close safety

The sweep walks resources that slipped past explicit `_free`
calls. Magic-number guards on `AxlEvent` and `AxlCancellable`
catch any ordering bug (loop frees before its child events are
swept, etc.) by no-oping on dead magic with a logged warning.

### 4.3 `axl_atexit` — POSIX-flavored cleanup registry

```c
/**
 * @brief Register a callback to run during _axl_cleanup.
 *
 * Callbacks fire in LIFO order (last-registered-first-run), which
 * matches C's atexit() and matches stack-unwinding intuition for
 * "tear down the newest thing first." Each callback receives the
 * user data pointer supplied at registration.
 *
 * Use cases: free top-level resources (loops, caches, HTTP
 * clients, open files) that would leak if not explicitly released.
 *
 * Storage: AxlArray-backed, grows as callbacks are registered.
 * Returns a handle so handlers can be removed early via
 * axl_atexit_remove.
 */
typedef void (*AxlAtexitFn)(void *data);

uint32_t axl_atexit(AxlAtexitFn fn, void *data);
void     axl_atexit_remove(uint32_t handle);
```

### 4.4 `axl_exit(rc)` — the guaranteed-cleanup exit path

Today, app code that calls `gBS->Exit` directly (or aborts through
some other path) **bypasses `_axl_cleanup` entirely** — argv isn't
freed, leak report doesn't fire, and once the registry lands,
events won't be swept either. This is a landmine.

Phase A7 introduces:

```c
/**
 * @brief Terminate the application with cleanup guaranteed.
 *
 * Runs atexit callbacks (LIFO), sweeps the resource registry,
 * runs heap cleanup per build mode (debug: report; release: free),
 * then calls gBS->Exit(image, status, 0, NULL). Does not return.
 *
 * This is the ONLY blessed exit path. Apps that return from main
 * take the same path via the AXL_APP entry wrapper. Apps that
 * call gBS->Exit directly bypass cleanup -- don't.
 */
AXL_NORETURN void axl_exit(int rc);
```

All the exit flows funnel through it:

| Entry | Path |
|---|---|
| `main` returns | `AXL_APP` wrapper → `_axl_cleanup` → `gBS->Exit` |
| App calls `axl_exit(rc)` | `_axl_cleanup` → `gBS->Exit` |
| App calls `exit(rc)` (POSIX compat) | thin wrapper to `axl_exit` |
| Default break handler fires | `_axl_cleanup` → `gBS->Exit(..., EFI_ABORTED, ...)` |
| Installed break handler returns | flag set → next yield / wait returns `AXL_CANCELLED` → caller unwinds → `main` returns → wrapper path |

The landed `_axl_cleanup` (`src/runtime/axl-runtime.c`):

```c
void _axl_cleanup(void) {
    if (mCleanupRan) return;        /* double-run guard */
    mCleanupRan = true;

    _axl_atexit_run_all();          /* user callbacks, LIFO */
    _axl_args_free();               /* argv strings */

    if (mDefaultLoop != NULL) {     /* clean unregister */
        axl_loop_free(mDefaultLoop);
        mDefaultLoop = NULL;
    }

    _axl_registry_sweep();          /* tier-1 firmware resources */

#ifdef AXL_MEM_DEBUG
    axl_mem_dump_leaks();           /* diagnose */
#endif
    /* Release-mode heap auto-free (axl_mem_sweep_free_all) is the
       tier-2 safety net proposed in §4.2; deferred per §10.1 and
       not wired in today. */
}
```

### 4.5 What fires when

**Normal exit path** (`main` returns):
1. Entry wrapper captures rc from `main`.
2. Calls `axl_exit(rc)` (or inlines the body).
3. `axl_exit` runs `_axl_cleanup`, calls `gBS->Exit`.

**Explicit exit** (`axl_exit(rc)` or `exit(rc)`):
1. Same as above from step 2. Unwinding stack *above* the call
   does not happen — AXL_AUTOPTR in outer scopes does **not**
   run. Apps that need scope cleanup must register via
   `axl_atexit`.

**Break-driven exit, default handler** (no `axl_signal_install`):
1. Break notify fires at raised TPL → sets `g_axl_interrupted`,
   calls registered default handler.
2. Default handler returns; next yield/wait observes the flag.
3. Yield path calls `axl_exit(1)` — `_axl_cleanup` runs, then
   `gBS->Exit(image, EFI_ABORTED, 0, NULL)` from the backend.

**Break-driven exit, user handler installed:**
1. Break notify fires → sets flag → calls user handler.
2. User handler does limited work (set local flag, log) and
   returns.
3. Next yield or wait returns `AXL_CANCELLED` to the caller.
4. Caller unwinds normally through AXL_AUTOPTR etc.
5. `main` returns; entry wrapper path runs.

The user-installed handler is **never expected to do cleanup
itself**. It can't reliably — it runs at raised TPL with limited
services available. Cleanup happens on the normal unwind path,
same as any other exit.

### 4.6 What AXL_AUTOPTR handles already

Scope-bound resources (declared with `AXL_AUTOPTR(AxlEvent)` etc.)
automatically free on scope exit — including when a wait returns
`AXL_CANCELLED` and the caller unwinds back through the scope. No
atexit entry needed for those.

`axl_atexit` is specifically for **long-lived resources** that
outlive function scope and would leak at process exit.

In C++ there is a third option between the two, and it is usually the
right one for a resource that outlives a function but not the process:
`axl::unique_handle<T>` (`<axl/axl-handle.hpp>`) is the same ownership
as `AXL_AUTOPTR` for the cases a `cleanup` attribute cannot express —
a class member, a moved value, a factory's return. A resource owned
that way is released when its owner is destroyed, so it needs no
atexit entry either. `axl_atexit` remains the answer for a genuinely
process-lifetime resource, and for all C code.

## 5. Nested loops

> *"What happens when a user embeds an Axl main loop within the
> runtime's default loop?"*

Scenarios and their semantics:

### 5.1 App doesn't use the default loop at all

```c
int main(int argc, char **argv) {
    AxlLoop *loop = axl_loop_new();
    /* ... register sources ... */
    axl_loop_run(loop);
    axl_loop_free(loop);
    return 0;
}
```

**Semantics:** fine. The default loop sits idle (it's a lazy
singleton inside the runtime; nothing has materialized it yet).
Break is still detected via the runtime's notify callback, not
via loop dispatch. App's loop is the active one; it picks up the
break flag via its own sources (the break-event poll continues to
register there too, under the hood).

### 5.2 App uses default loop directly

```c
int main(int argc, char **argv) {
    AxlLoop *loop = axl_loop_default();
    axl_loop_add_timer(loop, 1000, on_tick, NULL);
    axl_loop_run(loop);
    /* no axl_loop_free — the runtime owns this one */
    return 0;
}
```

**Semantics:** fine. One loop, no nesting. The runtime tears
down the default loop in `_axl_cleanup` (which CRT0 invokes after
`main` returns).

### 5.3 App creates its own loop *alongside* the default

```c
int main(int argc, char **argv) {
    /* default loop exists, idle */
    AxlLoop *my_loop = axl_loop_new();
    axl_loop_add_timer(my_loop, 1000, on_tick, NULL);
    axl_loop_run(my_loop);  /* drives my_loop, not the default */
    axl_loop_free(my_loop);
    return 0;
}
```

**Semantics:** the two loops are independent. The running one
dispatches its sources; the default sits idle. Sources registered
with the default loop (e.g., if CRT0 has a watchdog timer there)
do *not* fire while `my_loop` is running. This is OK because CRT0
shouldn't rely on the default loop being driven — break is
notify-based, not loop-based.

### 5.4 True nested loops (inner loop runs while outer is running)

This happens inside the library today. Two classes of API create a
throwaway loop while the caller's outer loop is blocked in a
callback:

- `axl_wait_*` and `axl_event_wait_*`, by design — a wait is a
  synchronous shape on top of a source.
- The blocking TCP and socket wrappers: `axl_tcp_connect` /
  `_accept` / `_send` / `_recv`, and the corresponding
  `axl_socket_*` sync variants. Each call allocates its own
  `AxlLoop`, submits the async op against it, runs the loop until
  the op completes or times out, and frees it. The header-side
  contract lives in the "Blocking TCP API" block in
  [axl-tcp.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-tcp.h) and "Blocking operations"
  in [axl-socket.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-socket.h).

```
outer axl_loop_run
  └─ source fires → cb is running
       └─ cb calls axl_wait_for_flag(...)
            └─ creates throwaway inner loop, runs it
                 └─ inner dispatches inner sources until flag is true
            └─ inner freed, axl_wait_for_flag returns
       └─ cb returns
  └─ outer resumes dispatch
```

**Semantics:** fine. Throwaway loops are a known pattern. Inner
loop has its own sources (event, timeout, cancel event). Outer
loop's sources are *not* dispatched during the inner run —
that's the nesting cost, accepted.

**Avoiding it inside a server callback.** The preferred shape for
an event-driven server (accept → recv → send → recv…) is to use
the `*_async` variants exclusively and let each callback's `bool`
return drive re-arm of the next step.
[`sdk/examples/echo-server.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/echo-server.c) is
the worked example: `on_data` fires `axl_socket_send_async`,
`on_echo_sent` re-arms recv, `on_accept` returns `true` to stay
armed. One loop, no nested dispatch, Ctrl-C observed on every
iteration. Reach for the blocking wrappers above only from
top-level `main`-body code or from contexts where paying the
nesting cost is fine; inside a loop callback they freeze every
other source on the outer loop for the duration of the call.

**When the blocking shape is the right choice.**
[`sdk/examples/echo-client.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/echo-client.c) and
[`sdk/examples/echo-server-sync.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/echo-server-sync.c)
show the counterpart: top-level linear code, no event loop, no
callbacks. The per-call temporary loops are invisible because
there is nothing outer to freeze. This is the right default for
CLI tools and single-client utilities. `echo-server-sync.c` also
carries the footgun disclaimer: a sync server can only service
one client at a time.

### 5.5 Rule

**The default loop is never used as a wait-helper throwaway.**
Wait/event-wait always create their own ephemeral loops. This
prevents source leaks between unrelated waits, and keeps the
default loop's invariants (for the runtime's own use) intact.

### 5.6 Nested-wait primitive: `axl_loop_iterate_until`

The throwaway-loop pattern in [§5.4](#true-nested-loops-inner-loop-runs-while-outer-is-running) has a real cost: while the inner
loop is running, the outer loop's sources are frozen. Confirmed by
the Phase A7 prototype (scenario 5, April 2026): a `timeout`
source added to the outer loop inside a callback cannot fire until
the callback returns, because the outer loop's WaitForEvent is
paused.

For callers that want the opposite behavior — drive *the current
loop* until a condition fires, without quitting it — the library
exposes an iteration primitive:

```c
/** Iterate `loop` until `done` is signalled, `timeout_us` elapses,
 *  or Ctrl-C. Does NOT set the loop's quit flag — the caller's
 *  outer run continues after this returns.
 *
 *  @return AXL_OK on `done`, AXL_TIMEOUT on timeout, AXL_CANCELLED on Ctrl-C. */
AxlStatus axl_loop_iterate_until(
    AxlLoop  *loop,
    AxlEvent *done,         /* NULL = only timeout / cancel wakes */
    uint64_t  timeout_us);  /* 0 = wait forever */
```

The landed signature takes a three-argument shape: just the loop,
an `AxlEvent *` (or NULL), and the timeout. An earlier design
sketch proposed an extra `AxlIteratePred` callback — that was
dropped before landing on the observation that every real caller
either wants an event-driven wake (use `done`) or a time-bounded
wake (use `timeout_us`); composite predicates are better built on
top of this primitive by the caller.

Usage split:

- **Library-internal waits** that don't know the caller's loop →
  keep using ephemeral loops (safe default, no coupling).
- **Waits inside a callback** of a known loop → call
  `axl_loop_iterate_until(loop, done, ..., timeout)`. Outer sources
  continue to fire. This is the primitive users would otherwise
  reach for loop-inheritance to get.

**Loop inheritance (e.g., `axl_loop_set_parent`) is explicitly
deferred.** Inheritance solves the same symptom but introduces
ambiguity (which loop owns a source, double-dispatch, lifetime
coupling) and conflicts with the "ephemeral loop for unknown
callers" default. The iterate-until primitive gives the same
ergonomics opt-in at the call site where the caller already knows
which loop they're inside. Revisit inheritance only if a specific
use case demands it.

## 6. Public API surface

```c
/* axl-signal.h -- interrupt handler + blessed exit path */
typedef void (*AxlSignalHandler)(void);

void              axl_signal_install(AxlSignalHandler on_interrupt);
void              axl_signal_default(void);
bool              axl_interrupted(void);
AXL_NORETURN void axl_exit(int rc);

/* axl-runtime.h -- default loop + yield + registry inspection */
AxlLoop *axl_loop_default(void);
void     axl_yield(void);
size_t   axl_registry_count(void);

/* axl-atexit.h -- LIFO cleanup registry */
typedef void (*AxlAtexitFn)(void *data);

uint32_t axl_atexit(AxlAtexitFn fn, void *data);
void     axl_atexit_remove(uint32_t handle);

/* axl-loop.h -- nested-wait primitive, see §5.6 */
AxlStatus axl_loop_iterate_until(
    AxlLoop  *loop,
    AxlEvent *done,        /* NULL = no done event */
    uint64_t  timeout_us); /* 0 = wait forever */
```

Source layout:

```
src/runtime/
  axl-runtime.c    _axl_init / _axl_cleanup; axl_loop_default; axl_yield
  axl-registry.c   tier-1 resource registry (internal)
  axl-atexit.c     LIFO callback registry
  axl-signal.c     signal install / interrupted / axl_exit
```

`axl_loop_iterate_until` lives in `src/loop/axl-loop.c` alongside
`axl_loop_run`.

**Pre-landing rename (merged as PR #1, commit `eba18a3`).** The
existing `axl-signal.h` pub/sub bus was renamed to `axl_pubsub_*`
in [axl-pubsub.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-pubsub.h) specifically to
free the `axl_signal_*` namespace for this interrupt API. The
new [axl-signal.h](https://github.com/aximcode/axl-sdk-releases/blob/main/include/axl/axl-signal.h) houses
`AxlSignalHandler` / `axl_signal_install` / `axl_signal_default` /
`axl_interrupted` / `axl_exit`. Blast radius: ~90 identifiers
across 14 files, entirely mechanical. Identifier map:

| Old | New |
|-----|-----|
| `AxlSignalCallback` | `AxlPubsubCallback` |
| `axl_signal_new(loop, name)` | `axl_pubsub_register(loop, name)` |
| `axl_signal_reset` | `axl_pubsub_reset` |
| `axl_signal_connect` | `axl_pubsub_subscribe` |
| `axl_signal_disconnect` | `axl_pubsub_unsubscribe` |
| `axl_signal_emit` | `axl_pubsub_publish` |
| `AXL_SIGNAL_H` | `AXL_PUBSUB_H` |

## 7. What we are **not** doing

- **`setjmp`/`longjmp` from the break notify.** Classic footgun;
  skips all destructors and leaks resources; corrupts invariants.
- **UEFI watchdog as a signal mechanism.** Watchdog is reset-only;
  can't be repurposed. Optional use as a library-livelock guard
  only.
- **NMIs, hardware interrupts, or firmware-specific preemption
  hooks.** Platform-dependent, unreliable, out of AXL's scope.
- **Any claim that CPU-bound app code that ignores AXL is
  interruptible.** It isn't, and that's honest. Document loudly.

## 8. Landed as Phase A7

The runtime prototype in `sdk/examples/runtime-demo.c` validated
the API shape end-to-end; the real module then landed as a seven-
commit series on `main` (April 2026):

| Commit | Scope |
|---|---|
| `3789aea` | `axl_loop_iterate_until` promoted from prototype |
| `0990ae2` | `src/runtime/` skeleton + default loop + `axl_yield` |
| `a09fe38` | Tier-1 registry + caller-attribution macros |
| `8a0f275` | `axl_atexit` LIFO cleanup registry |
| `dc37daa` | `axl_signal_install` + `axl_exit` + `axl_backend_boot_exit` |
| `64bad90` | `AxlTestRuntime` unit test binary |
| `4368256` | `runtime-demo` migrated off the mini-runtime to real APIs |

The eight `runtime-demo` scenarios now drive the real runtime:

| # | Subcommand | Validates |
|---|---|---|
| 1 | `signal` | `axl_signal_install`; user handler fires on Ctrl-C |
| 2 | `atexit` | LIFO drain during `_axl_cleanup` |
| 3 | `yield` | `axl_yield` + `axl_interrupted`; ≤100 ms break response |
| 4 | `default-loop` | Singleton loop teardown |
| 5 | `nested-loop` | Ephemeral-loop contract (outer freezes during wait) |
| 5b | `iterate-until` | `axl_loop_iterate_until` (outer sources keep firing) |
| 6 | `leak-event` | Registry sweep catches leaks with user file:line |
| 7 | `axl-exit-vs-return` | Identical cleanup on both exit paths |

Regression state at the end of the series: 1332/1332 unit tests on
X64 and AARCH64, CPU-idle ratio 0.39 (threshold 0.60).

## 9. Design decisions locked in

Captured here so they don't re-surface as questions during
implementation:

- **Registry is always on.** No `AXL_NO_RUNTIME_REGISTRY` escape
  hatch. Drivers and runtime images rarely create resources
  through the `axl_event_*` public API — they work directly with
  backend or EDK2 primitives — so the registry cost falls on the
  app-level consumers who benefit from it.
- **Heap sweep is mode-dependent:** debug reports, release frees.
  Debug must not auto-free or developers never see their bugs.
- **Sweep order is LIFO** registration order. Matches `atexit` and
  lets containers tear down before their contents.
- **`axl_exit` is the only blessed exit path.** Bypassing it (raw
  `gBS->Exit`, explicit PE return) is documented as unsafe and
  skips all cleanup.
- **User break handlers don't do cleanup.** They run at raised TPL
  where cleanup isn't safe; cleanup runs on the unwind.
- **Interrupt API uses `axl_signal_*`** (the POSIX-flavored name
  users' muscle memory reaches for first). The `axl_signal_*`
  namespace is freed by renaming the existing pub/sub bus — see
  next bullet.
- **Pub/sub bus renamed to `axl_pubsub_*`.** Happens as a separate
  pre-landing PR specifically to free up `axl_signal_*` for the
  interrupt API. Pre-1.0, ~90 identifiers across 14 files; see [§6](#public-api-surface)
  for the identifier map.
- **Loop inheritance / `axl_loop_set_parent` deferred.** The
  nested-wait use case is covered by `axl_loop_iterate_until` ([§5.6](#nested-wait-primitive-axl-loop-iterate-until)),
  which is opt-in at the call site. Revisit only if a concrete use
  case demands inheritance semantics.
- **Registry storage: dynamic (AxlArray-backed), not fixed-size.**
  The prototype used fixed-16 and never hit the cap, but apps with
  hundreds of live resources (HTTP clients, cached connections)
  could; the cost is one arena-backed AxlArray that rarely grows
  past initial capacity.
- **`axl_yield` dispatches the default loop only when pending work
  is immediately ready** (non-blocking poll). No wait, no iteration
  count.
- **`axl_interrupted()` reports Ctrl-C only**, not cancellables.
  `AxlCancellable` waits continue to return `AXL_CANCELLED` as
  today.
- **Break during `axl_yield` with an installed handler that returns
  normally: yield returns.** Caller reacts via `axl_interrupted()`.
  Matches POSIX signal-handler semantics.
- **Watchdog default: off.** Opt-in via `axl_watchdog_enable(60)`
  for apps that want the livelock guard.

## 10. Deferred items

Phase A7 landed the runtime surface end-to-end. Two design-doc
items are deferred to a follow-up phase when the motivating use
case appears:

### 10.1 Release-mode heap auto-sweep

[§4.2](#the-internal-resource-registry) tier-2 proposed that release builds walk `mAllocList` at
`_axl_cleanup` and `axl_free` each entry — so apps that leak
heap on exit don't bleed memory into the firmware pool across
many invocations.

**Status: not implemented.** The tier-1 (firmware-resource)
registry sweep IS implemented and handles the crash-hazard class
(events, loops, cancellables, arenas). The tier-2 heap sweep was
skipped because `mAllocList` only exists under `AXL_MEM_DEBUG`
today — release builds use a single-word header with no linked
list. Implementing auto-sweep requires promoting the `prev`/`next`
pointers out of the debug gate (cost: ~16 bytes per allocation on
x64 in release).

**Implement when:** we have a long-running app (e.g. SoftBMC,
axl-webfs running as a persistent service) where leaked heap survives
long enough to matter. Short-lived tool-style apps (fetch, sysinfo,
etc.) don't benefit meaningfully — the firmware reboot reclaims
pool memory anyway.

### 10.2 Watchdog as library-livelock guard

[§9](#design-decisions-locked-in) locked in "watchdog default: off, opt-in via
`axl_watchdog_enable(seconds)`". The API doesn't exist yet. No
concrete caller has asked for it. Implement when needed.

### 10.3 `axl_yield()` instrumentation of AXL APIs

**Status: initial batch landed 2026-04-20.** See the status column
in [§3.1](#where-axl-apis-inject-yields-automatically). The high-impact retry and CPU-bound loops now yield:
`axl_file_get_contents` / `_set_contents`, the `axl_http_get` body-
read loop, `axl_checksum_update` (chunked at 64 KiB), both
`axl_array_sort` variants (every 1024 outer iters), and the IPMI
KCS `IBF_clear` / `OBF_set` busy polls (every 100 iters, ~10 ms).

**Deferred** (not important today, add when a caller hits them):
directory iteration in `axl_dir_read` on huge listings, format
engine streaming to slow sinks, SSIF 60 ms retry loops, JSON parse
on multi-MB documents.

### 10.4 Minimal runtime opt-out

**Status: landed 2026-04-20; made genuinely minimal 2026-08-19.**
`src/crt0/axl-crt0-minimal.c` ships as a peer to
`axl-crt0-native.c` and is selected via `axl-cc --minimal-runtime`.
The minimal CRT0 sets the firmware globals, calls `main`, honours a
pending `axl_set_exit_status`, returns. It skips
`_axl_registry_init`, `_axl_atexit_init`, `_axl_signal_init`, and
default-loop creation.

Everything else is opted back in by FEATURE, and each feature is a
link decision rather than a code path — `axl-cc` turns it into a
`-u SYMBOL` that pulls the archive member which defines it:

| feature | what it restores |
|---|---|
| `stdio` | force the stream layer in (it also self-initialises whenever the app references `axl_printf` and friends) |
| `args`  | `main()` gets its `argc`/`argv` |
| `noargs`| confirm `main()` does not need them |
| `log`   | the log engine, so `axl_error` and friends emit |
| `nolog` | confirm you want them to no-op, and keep the bytes |

`--minimal-runtime=stdio,args` is the behaviour the flag had before.

**Two of those are questions the driver will not answer for you.** An
app whose own objects call a log macro must name `log` or `nolog`, and
one whose `main` is declared with parameters must name `args` or
`noargs`; `axl-cc` refuses the link otherwise. An image cannot go quiet,
or lose its arguments, without someone having chosen it. `main(void)`
needs neither — it has nothing to declare.

The two are read by different instruments, and the reason is worth
knowing when a refusal surprises you. Logging is visible to `nm`:
`axl_error` expands to a call, so the caller's object carries an
undefined `axl_log_full`. **Reading `argv` emits no symbol at all** —
it arrives in registers, and `main(void)` and `main(int, char **)` are
one name with one linkage — so `main`'s arity is read from DWARF
(`-g -gdwarf` is on in `DEBUG` *and* `RELEASE`). When it cannot be read
— a prebuilt object compiled without debug info — that is its own
refusal, because "could not look" and "takes no arguments" are the same
empty answer and guessing wrong ships an image with a silently empty
`argv`.

> **The `argv` guarantee is currently a coincidence, which is why it is
> asked about.** On today's tree a bare `--minimal-runtime` image *does*
> receive `argv`: `axl-cxxrt-stubs.o` is on every link (the porting layer
> travels with the C library), it strongly references `axl_file_info`,
> and that drags `axl-fs.o` → `axl-driver.o` → `axl-app.o`, which defines
> `_axl_args_init`. The CRT0's weak reference binds through an edge that
> has nothing to do with `argv`. Declare what you use; the coincidence is
> not a contract.

The registry and atexit APIs no-op safely when their storage is
NULL (`_axl_registry_add` returns 0, `axl_atexit` returns 0), so
`libaxl.a` stays unchanged — the library doesn't need to know
which CRT0 linked it.

Behavior contrast on a `runtime-demo leak-event` debug build:

- Full runtime: `registry: sweep: AxlEvent leaked at ... — closing`
  followed by `mem: no leaks detected` (sweep freed the resource).
- Minimal runtime: no sweep; the debug leak report prints the raw
  allocation site. Apps that opt out own their cleanup.

**Size.** This paragraph used to say the flag was not a size knob,
and it was right about the code as it then stood: everything the
CRT0 skipped was already elided by `-ffunction-sections` +
`--gc-sections`, and the pieces it *kept* — the stream layer, and a
log engine that 27 archive members referenced — were the floor. Both
are opt-in now. A do-nothing x64 RELEASE image measures **30,720
bytes** against **36,864** for the full runtime; before this work the
same pair was 36,864 against 37,376. See
`docs/AXL-Minimal-Image-Notes.md` for where the remaining 30 KB goes
and for the ~4.6 KB an image that links no `libaxl` at all reaches.

It remains a *behaviour* opt-out as well (exit semantics, resource
tracking), and that half is unchanged. Drivers and runtime images
are unaffected: they supply their own entry and don't link either
CRT0, same as before — and every ordinary link, theirs included,
carries the log engine by default.

## 11. What this doesn't help with

- CPU-bound app code with no `axl_yield` and no AXL calls: still
  uninterruptible. Document with a specific example.
- Code hung inside a firmware call (UEFI protocol deadlock): not
  our problem; watchdog reset is the only option.
- Bugs in firmware event handling: platform-specific; document
  workarounds as they come up.

---

## Appendix: Decision log

Captures the high-level choices made in our design conversations
so future contributors don't re-litigate them.

- **No longjmp.** Rejected in the signals discussion for async-
  signal-unsafety reasons. See [§7](#what-we-are-not-doing).
- **No watchdog repurpose.** Watchdog is reset-only on every
  platform; not useful for signal-like semantics. See [§7](#what-we-are-not-doing).
- **Yes a library-side runtime.** Controlling every AXL API is
  the right leverage point — cooperative yields in library code
  approximate POSIX signal responsiveness. CRT0 stays a thin
  entry stub; the runtime, invoked by CRT0, does the work. See
  [§1](#motivation) and [§3](#axl-yield-cooperative-escape-hatch).
- **Default loop is optional, not mandatory.** Apps that already
  manage their own don't have to change. See [§5](#nested-loops).
- **Sleep is Ctrl-C interruptible.** Landed in commit `72ae173`,
  documented in `axl-wait.h`. This doc builds on that assumption.
- **Interrupt API prefix is `axl_signal_*`.** The POSIX-flavored
  name is what users' muscle memory reaches for first. The
  existing pub/sub bus occupying that namespace is renamed out of
  the way (see next entry). Internal plumbing keeps "break" where
  it refers to the UEFI mechanism (`axl_backend_shell_break_*`,
  the firmware event's own name); the user-facing API is "signal".
  See [§2.2](#signal-subsystem).
- **Pub/sub bus renamed to `axl_pubsub_*`.** The pre-1.0 rename
  specifically frees the `axl_signal_*` prefix for the interrupt
  API — that's the whole justification for paying the rename
  cost. Prefix + verbs change together: `publish` / `subscribe` /
  `unsubscribe` / `register`. See [§6](#public-api-surface).
- **Loop inheritance rejected in favor of `axl_loop_iterate_until`.**
  Inheritance solves the nested-wait outer-loop-starved symptom
  but introduces lifetime/ownership ambiguity and conflicts with
  the ephemeral-loop default. The explicit iterate-until primitive
  gives the same ergonomics with opt-in at the call site where the
  caller already knows which loop they're in. See [§5.6](#nested-wait-primitive-axl-loop-iterate-until).
- **Phase A7 landed.** `sdk/examples/runtime-demo.c` now drives
  the real runtime (not the pre-landing mini-runtime). Validates
  atexit LIFO, tier-1 registry sweep with caller attribution,
  `axl_yield` interruption (≤100 ms response), default-loop
  teardown, nested-wait pattern, and identical cleanup on both
  `return` and `axl_exit` paths. Regression state at landing:
  1332/1332 unit tests on X64 and AARCH64, CPU-idle ratio 0.39
  (threshold 0.60).
