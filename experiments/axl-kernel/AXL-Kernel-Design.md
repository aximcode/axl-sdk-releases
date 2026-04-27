# AXL Kernel — Design Sketch

**Status:** brainstorm-in-progress as of April 2026. Not implemented.
This doc captures a proposed mini-OS / cooperative-scheduler layer
for UEFI that would sit on top of the existing `axl-sdk` library.
Expect every section to change as the design gets vetted; read it
as a working agreement, not a spec.

**One-line pitch:** give UEFI apps a Linux-shaped process model —
`fork`, `wait`, `exit`, `read`, `write`, `pipe`, `kill` — on top of
a cooperative scheduler. One new primitive (`process = stackful
coroutine`) subsumes the Init/Poll/Cleanup module pattern that
every non-trivial UEFI app ends up reinventing.

**Related reading:**
- [`AXL-Runtime.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md) — the CRT0-owned runtime this
  layer would build on (atexit, signal, registry sweep, default
  loop, `axl_yield`).
- [`AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md) — the four-axis
  taxonomy this layer composes. Notably its "Why not stackful
  coroutines" rejection; see [§6](#6-stack-economics--reopening-the-old-rejection) below for why we're reopening it.
- [`AXL-Design.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Design.md) — overall axl-sdk architecture
  (the library this would sit on top of).

---

## 1. Motivation — what SoftBMC keeps trying to be

`../softbmc/` is the largest axl-sdk-family consumer we have, and
the places it fights the underlying model are the motivation for
this layer. Concretely:

| Symptom (SoftBMC today) | Root cause (the model) |
|---|---|
| HTTP handlers must be fully stateless — SMBIOS re-parsed every request ([SoftBMC-Design.md:295](../../softbmc/docs/SoftBMC-Design.md#L295)) | No stack-local "this request" context; a handler is a single callback with no continuation |
| Per-client state tracked via `SoftBmcSetClientData()` sidecar with manual disconnect cleanup | No per-client address space of any kind; void\* + manual teardown is the only option |
| `RfbPoll()` hand-unrolls VNC tile encoding one 8×8 tile per tick ([RfbServer](../../softbmc/SoftBmcPkg/Application/SoftBmc/Modules/Feature/RemoteKvm/)) | The scheduler doesn't know how to suspend mid-encoding; the code has to split itself across ticks |
| One buggy `gBS->Stall(1s)` freezes HTTP, WS, terminal, VNC, and watchdog tickle | Single event loop, no isolation; a blocking call anywhere stalls everything |
| Cleanup order reverse-hand-coded in [ModuleManager.c:327-328](../../softbmc/SoftBmcPkg/Application/SoftBmc/Core/ModuleManager.c#L327-L328) | No implicit LIFO ownership; modules carry hand-written dependency knowledge |
| `Persist()` bolted onto the module interface to survive OS handoff | No natural "exit hook"; cleanup had to be retrofitted as a callback |
| RemoteShell's scrollback is a module-global `static char[64K]` — two clients see each other's output | No per-connection address space; shared globals are the only storage |

Every row is a consequence of the same fundamental mismatch:
**developers want to write sequential, stateful code; the framework
only lets them write stateless callbacks on a shared tick.**

Fix the primitive, the rest follows.

## 2. Goals and non-goals

### Goals

- **Sequential code model.** A developer writes what looks like a
  `main()` with blocking I/O. Under the hood, every blocking call
  is a yield point and the scheduler multiplexes.
- **Familiar API.** `read`, `write`, `open`, `close`, `fork` (or
  `spawn`), `wait`, `exit`, `pipe`, `kill`, `sleep`. Linux programmers
  find their footing in minutes.
- **Lifecycle by stack unwinding.** Return from `main` → process
  exits → stack unwinds → AXL_AUTOPTRs fire → atexit hooks fire →
  fds close. No separate Init/Poll/Cleanup trio.
- **Isolation by stack.** Each process has its own call stack. Its
  state lives there. Exit releases it wholesale.
- **Fault containment.** A runaway process that refuses to yield
  is flagged and terminated at the next scheduler opportunity;
  other processes keep running. (Best-effort — see [§10](#10-open-questions--unknowns) unknowns.)
- **Composable services.** `axl_spawn(httpd); axl_spawn(kvm);
  axl_spawn(ipmi_watcher);` — no module table, no cleanup ordering.

### Non-goals (hard constraints)

- **No preemption.** UEFI BSP is cooperatively scheduled. A CPU-bound
  process that never yields *will* stall the scheduler. We mitigate
  ([§10](#10-open-questions--unknowns)), we do not pretend we solved it.
- **No MMU isolation.** Processes share one address space. A bug
  in one can scribble another's stack. Same as SoftBMC today; this
  layer doesn't make it worse, but it doesn't fix it either.
- **No fork() semantics that duplicate address space.** `fork()`
  here means "new process with its own stack, sharing heap." Closer
  to Linux `clone(CLONE_VM | CLONE_FS)` or Windows thread than to
  classic `fork()`.
- **No replacement for axl-sdk.** This layer *consumes* axl-sdk
  primitives. Apps that want the raw callback model keep it.

## 3. Core primitive: process = stackful coroutine

A **process** is a cooperatively-scheduled unit of execution with:

- its own **call stack** (fixed size, allocated at spawn),
- a **program counter** (saved/restored via context switch),
- a **process control block (PCB)** — pid, parent, children list,
  state (runnable / waiting / zombie), exit status, signal mask,
  open-fd table,
- optional **atexit hooks** and a **tier-1 registry** scoped to
  this process.

```c
typedef int (*AxlProcMain)(int argc, char **argv);

AxlPid axl_spawn(
    AxlProcMain entry,   ///< entry function (like main)
    int         argc,
    char       **argv,
    size_t      stack_kib  ///< 0 = default (say 16)
);
```

When `entry` returns, the process exits normally (its return value
becomes the exit status). When `axl_exit(rc)` is called, it
short-circuits the return. Either way, the stack unwinds naturally;
any AXL_AUTOPTRs declared in scope fire in reverse order.

### 3.1 Scheduler

The scheduler is an `AxlLoop` (or a thin wrapper around one) with
two extra data structures:

- **Ready queue** — PCBs ready to run. Popped one at a time by the
  scheduler, which context-switches into them.
- **Wait queues** — PCBs blocked on some wake condition (fd ready,
  timer expired, child exit, signal). One queue per wait condition;
  the condition firing moves matching PCBs from the wait queue back
  to the ready queue.

Every `axl_*` syscall that can block:

1. Records the wake condition in the current PCB.
2. Moves the current PCB from running → wait-queue.
3. Context-switches back to the scheduler.

The scheduler:

1. Picks the head of the ready queue.
2. If empty: block on `AxlLoop`'s underlying `axl_backend_event_wait`
   for any registered wake source. When it wakes, fd-ready /
   timer-fired callbacks move waiters to ready. Loop.
3. Context-switches into the chosen PCB.

That's it. The loop *is* the kernel.

### 3.2 Why stackful, not stackless

A stackless coroutine (async/await via macros, protothreads,
generator state machines) lets local variables die at each yield
point unless you manually hoist them into a heap-allocated frame.
That destroys the "feels like Linux" win — you're back to writing
state machines by hand, which is exactly what SoftBMC already does.

Stackful gives you:

- Locals survive yields naturally (they live on the stack).
- Nested function calls yield transparently — you don't have to
  propagate async-ness up the call tree.
- Existing sync-ish C code ports with near-zero change.
- Debuggers see real call stacks.

The cost is one stack per process. That cost was the subject of
axl-sdk's earlier rejection; see [§6](#6-stack-economics--reopening-the-old-rejection) for why it's reopenable.

## 4. Syscall layer — how existing AXL maps

This is the "tractable" half of the pitch. We are not building a
kernel from scratch. We're wrapping existing primitives.

| Kernel concept | Implemented via |
|---|---|
| Scheduler | `AxlLoop` + ready/wait queues on top |
| Wait queue (generic) | `AxlEvent` one-shot latch per waiter |
| Timer wait (`sleep`) | `axl_loop_add_timeout` wakes the waiting PCB |
| fd-ready readiness | TCP/UDP/file completion tokens → fires PCB's wake event |
| Signal delivery | `axl_signal_*` + `AxlPubsub` for process-group fan-out |
| `kill -TERM` | `AxlCancellable` on the target PCB's I/O; process observes `axl_interrupted()` |
| AP offload (blocking CPU) | `AxlTask` — literally the "syscall that dispatches to a co-processor" |
| `atexit` per process | Existing `axl_atexit` scoped per-PCB instead of global |
| Resource cleanup on exit | Tier-1 registry sweep, already landed for global scope |

### 4.1 fd abstraction

One uniform small-int handle for all I/O. A fd table in the PCB
maps `int fd → (kind, resource, ready_event)`:

| Kind | Backed by |
|---|---|
| `AXL_FD_TCP` | `AxlTcp` or `AxlSocket` handle |
| `AXL_FD_UDP` | `AxlSocket` datagram handle |
| `AXL_FD_FILE` | `AxlIoFile` handle |
| `AXL_FD_PIPE` | bounded ring buffer + reader/writer wake events |
| `AXL_FD_TIMER` | `AxlLoop` timer; read returns fire count |
| `AXL_FD_SIGNAL` | signalfd-style: read returns queued signals |

Every fd exposes the same four operations: `axl_read`, `axl_write`,
`axl_close`, `axl_wait_readable` (the last for poll/select-style
composition). Internally each is a syscall — yield, register wake,
resume on ready.

### 4.2 Candidate public API (strawman)

```c
/* Process control */
AxlPid axl_spawn(AxlProcMain, int argc, char **argv, size_t stack_kib);
int    axl_wait(AxlPid, int *status);           /* -1 = any child */
int    axl_waitpid(AxlPid, int *status, int flags); /* AXL_WNOHANG */
void   axl_exit(int status)   AXL_NORETURN;
AxlPid axl_getpid(void);
AxlPid axl_getppid(void);

/* Time */
int    axl_sleep_ms(uint32_t ms);               /* interruptible */
int    axl_yield(void);                         /* cooperative tick */

/* fds — uniform I/O */
int    axl_read(int fd, void *buf, size_t n);   /* blocking, yields */
int    axl_write(int fd, const void *buf, size_t n);
int    axl_close(int fd);

/* Sockets (thin wrappers that hand back fds) */
int    axl_listen(uint16_t port);
int    axl_accept(int listen_fd);               /* yields until conn */
int    axl_connect(const char *host, uint16_t port);

/* Pipes */
int    axl_pipe(int fd[2]);

/* Signals */
int    axl_kill(AxlPid, int signo);
int    axl_signal(int signo, void (*handler)(int));

/* Introspection (debug / shell) */
size_t axl_ps(AxlProcInfo *out, size_t cap);
```

Notable: there's no `fork()`. `axl_spawn` takes an entry function
directly. The no-MMU-duplication reality means `fork()` would just
confuse people. Users who want `fork + exec` shape write
`axl_spawn(my_exec_entry, argc, argv, 0)`.

## 5. Relationship to axl-sdk

New layer on top, not a fork, not in-tree:

```
libaxl-kernel.a     scheduler, PCB, fd table, syscalls, libc-shape API
   ↓ links against
libaxl.a            loop, events, net, task pool, arena   [unchanged]
   ↓ depends on
UEFI spec + backend                                       [unchanged]
```

- axl-sdk stays "GLib for UEFI": raw primitives, sync and async
  APIs, callback-driven.
- axl-kernel adds the process model as a consumer of axl-sdk.
- An app picks its CRT0: `_AxlEntry` (today) or `_AxlKernelEntry`
  (new). The second one spawns `main` as pid 1 and runs the
  scheduler until pid 1 exits.
- Both CRT0s link `libaxl.a`; only the kernel CRT0 links
  `libaxl-kernel.a`.

Why separate repo / package:

- Different audience. Libraries and kernels have different
  release cadences.
- Avoids bloating axl-sdk with a runtime that most consumers
  won't use.
- Lets axl-sdk remain the stable building block; kernel can
  iterate faster.

Proposed final home: `aximcode/axl-kernel` (peer to `aximcode/axl-sdk`,
`aximcode/uefi-devkit`, `aximcode/softbmc`). **Until the POC
passes K2's success criteria** ([§13](#13-poc-specification--what-gets-built-first)), the code incubates in-tree
under `axl-sdk/experiments/axl-kernel/`. That gives the POC free
access to AxlLoop, `axl_printf`, the unit-test harness, QEMU
integration tests, and the build system. Graduating to its own
repo becomes a `git filter-repo` operation once the approach is
validated; if the POC fails we delete a directory instead of
orphaning a repo.

## 6. Stack economics — reopening the old rejection

`AXL-Concurrency.md`'s "Why not stackful coroutines" section cites
"16 KB × 20 tasks = 320 KB on a system where every KB matters."
That framing undersells the target machine.

### The numbers

| Device class | RAM | 16 stacks × 16 KiB | % |
|---|---|---|---|
| QEMU dev default | 512 MiB | 256 KiB | 0.05% |
| Modern server (Dell, HPE) | 64–512 GiB | 256 KiB | ~0% |
| Edge / embedded UEFI | 2 GiB | 256 KiB | 0.01% |

The "every KB matters" concern applies to PEI and DXE-early
stages, not to DXE-late / BDS / Shell / App where this runtime
would actually live.

### Real constraints that survive the re-examination

1. **No MMU → no guard pages.** Stack overflow = silent corruption
   of the next structure in memory. Mitigation: write a canary at
   stack base, check on every yield, force-kill on mismatch. Cheap
   and catches most bugs.
2. **`AllocatePool` fragmentation** if many short-lived processes
   churn stacks. Mitigation: fixed **stack pool** pre-allocated at
   kernel init. A free-list of N stacks, each fixed size. Caps
   concurrency to N but eliminates runtime allocation churn.
3. **Debugger ergonomics.** GDB can follow `setcontext`-style
   stack swaps if you tell it. A 20-line GDB Python helper or a
   convenience macro-in-the-PCB-pointer suffices. Invest early.

### Design bound

A reasonable starting shape: **N=16 stacks × 16 KiB = 256 KiB
pool**. Configurable at kernel init. Processes that exceed 16 KiB
of stack depth can request a larger stack via
`axl_spawn(..., stack_kib=64)` at a cost pulled from a separate
larger-stack pool or direct-alloc. Most processes won't need this.

## 7. Worked example — SoftBMC on axl-kernel

Compare the current module interface…

```c
/* SoftBMC today */
SOFTBMC_MODULE gHwInfoModule = {
    .Type   = SOFTBMC_TYPE_FEATURE,
    .Init   = HwInfoInit,
    .Poll   = HwInfoPoll,
    .Cleanup = HwInfoCleanup,
    .TypeInfo = &gHwInfoFeatureInfo,    /* HTTP routes, WS handler, ... */
};
```

…to the same thing as a process:

```c
/* SoftBMC on axl-kernel */
int hwinfo_service(int argc, char **argv) {
    AxlSnapshot *snap = axl_smbios_snapshot();    /* once, local */

    int listener = axl_listen(8080);
    for (;;) {
        int client = axl_accept(listener);        /* yields */
        if (client < 0) break;                     /* Ctrl-C */
        axl_spawn(hwinfo_handle_one, 1,
                  (char*[]){(char*)(uintptr_t)client}, 0);
    }
    axl_smbios_snapshot_free(snap);
    return 0;
}

int hwinfo_handle_one(int argc, char **argv) {
    int client = (int)(uintptr_t)argv[0];
    AxlHttpReq req;
    if (axl_http_read_request(client, &req) == 0) {
        axl_http_write_json(client, build_system_snapshot());
    }
    axl_close(client);
    return 0;
}

/* SoftBMC top-level */
int main(int argc, char **argv) {
    axl_spawn(http_server_proc,   0, NULL, 0);
    axl_spawn(hwinfo_service,     0, NULL, 0);
    axl_spawn(terminal_proc,      0, NULL, 0);
    axl_spawn(kvm_proc,          0, NULL, 0);
    axl_spawn(watchdog_proc,     0, NULL, 0);
    return axl_wait(-1, NULL);    /* wait for all children */
}
```

**What changed:**

- Each feature is its own process. Its lifecycle is its process
  lifecycle — no Init/Poll/Cleanup trio.
- Per-request state is stack-local in `hwinfo_handle_one`.
- Per-connection isolation is free: each WebSocket client could
  be its own process if we want.
- No global cleanup ordering: processes exit in arbitrary order;
  parent waits.
- No `Persist()` hook: if a process wants to persist state on
  exit, it does so itself before returning (or via `axl_atexit`).
- Accidental `gBS->Stall(1s)` in `hwinfo_handle_one` freezes only
  that one request's process; the scheduler keeps running others.
  The stuck process is killable via `kill`.

Line count for a realistic port: I'd estimate SoftBMC's module
framework + manager (roughly 1500 LOC in
`Core/ModuleManager.c` + `SoftBmcModule.h`) collapses to ~300 LOC
of process-shaped service code, and the per-module files get
shorter too.

## 8. Alternatives considered

### 8.1 Stackless async/await via macros

Zero per-task stack, preserve some state across yields via
compiler tricks (`switch`/`case`, computed goto) or codegen from a
DSL. Locals die at yield points unless hoisted to a heap frame.
Rejected: kills the "feels like Linux" property. Same friction as
SoftBMC today, decorated differently. Still sometimes useful for
lightweight generators, but not the process primitive.

### 8.2 Pure actor model (Erlang-shape)

Each process is `(mailbox + handler)`. No shared mutable state;
communication only via messages. Gives iron-clad isolation even
without MMU. Rejected as the primary primitive because you end up
writing message-dispatch state machines, not sequential code —
same problem as SoftBMC. **But** an actor layer on top of the
process primitive makes perfect sense: `axl_spawn_actor(handler)`
is a process whose main loop is `for (;;) { msg = axl_recv();
handler(msg); }`.

### 8.3 RTOS-task model (FreeRTOS-shape)

Tasks + priorities + scheduler. Without hardware preemption, this
reduces to cooperative coroutines with a priority ordering on the
ready queue. Strict subset of what [§3](#3-core-primitive-process--stackful-coroutine) proposes. If we want
priorities later, add them to the ready queue; no architectural
change.

### 8.4 Hierarchical event loops (the shelved "parent drives children")

See [AXL-Runtime.md §5.4](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md#54-true-nested-loops-inner-loop-runs-while-outer-is-running) for the prior
rejection. The idea was: an `AxlLoop` has children `AxlLoop`s; the
parent iterates them each tick. Rejected because of source-
ownership ambiguity, break-event re-entry, poll-timer multiplication,
and undefined quit propagation. **Processes-as-coroutines
sidestep the problem entirely** — everything runs on one shared
loop; concurrency comes from multiple stacks, not multiple loops.

## 9. Phased implementation plan

Each phase ends with a demo that validates the plan before
committing to the next. No phase is more than 2 weeks of work.

**Current status (as of last work session):** K1, K2, K3, K5, K6
landed and pushed, plus two additional SoftBMC-shape ports
(BootConfig — UEFI NVRAM; ReqLog — RAM-resident ring buffer with
per-request mutation). The three ports together cover the full
shape spectrum the design anticipated: stateless, firmware-backed,
and live RAM state. `axl_waitpid(WNOHANG)` added to lift the
accept-then-spawn pattern's sequential-connection cap (ReqLog
demonstrates 24 connections against a 16-slot PCB with inline
zombie draining). K4, K7–K9 not started. See the [§9.x](#9-phased-implementation-plan) status
callouts below and
[experiments/axl-kernel/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/experiments/axl-kernel/README.md)
for resume context.

### Phase K1 — context switch prototype  **[LANDED]**

- Scope: x86-64 only, ~50 lines asm. `axl_ctx_switch(from, to)`.
- Demo: two coroutines in a tight `ping-pong` yielding to each
  other, driven by a raw `AxlLoop` timer.
- Exit criteria: switch works under `-O0` and `-O2`, debugger
  follows both stacks.

### Phase K2 — scheduler + spawn/exit/wait  **[LANDED]**

- Scope: PCB struct, ready/wait queues, `axl_spawn`, `axl_exit`,
  `axl_wait`, `axl_getpid`. All in-memory, no fds yet.
- Demo: parent spawns 4 children that each `axl_sleep_ms(500)`
  and return; parent `axl_wait(-1)`s until all are reaped.
- Exit criteria: 64-process stress test in a loop; no leaks
  (tier-1 registry clean at exit).

### Phase K3 — fd abstraction + one fd kind  **[LANDED]**

- Scope: fd table in PCB, `axl_read`/`write`/`close` syscalls.
  First fd kind: `AXL_FD_TCP`. `axl_listen`/`accept`/`connect` as
  fd-returning wrappers.
- Demo: `axl-kernel` echo server. Identical shape to `echo-server.c`
  but as sequential code inside a process. Integration test via
  existing `tcp-echo-server.py` / `tcp-probe.py`.
- Exit criteria: test passes on X64 and AARCH64.

### Phase K4 — signals, pipes, sleep, timers  **[NOT STARTED]**

- Scope: `axl_kill`, `axl_signal`, `axl_pipe`, `axl_sleep_ms` (real
  version with interruptibility), `AXL_FD_TIMER`, `AXL_FD_SIGNAL`.
- Demo: SIGINT → all processes get killed → scheduler exits
  cleanly.
- Exit criteria: Ctrl-C at any point leaves zero leaked tier-1
  resources.

### Phase K5 — libc-shape API + aarch64 context switch  **[PARTIAL: aa64 LANDED, libc-shape deferred]**

- Scope: `open`, `read`, `write`, `close` stdio-style over files;
  aarch64 asm context switch.
- Demo: a `cat`-like tool that reads a file and pipes it to a TCP
  client, all sequential code.
- Exit criteria: parity with X64 on AARCH64.

### Phase K6 — SoftBMC HwInfo module port  **[LANDED — go/no-go PASSED]**

- Scope: port exactly one SoftBMC feature (HwInfo) to axl-kernel.
  Leave the rest of SoftBMC on the current module framework.
- Demo: same dashboard, same HTTP endpoints, served by an
  axl-kernel process from within SoftBMC.
- Exit criteria: side-by-side LOC comparison. If the port isn't
  meaningfully shorter / simpler, **stop and reassess** — the
  abstraction isn't paying for itself.

### Phase K7 — introspection + control-plane API  **[NOT STARTED]**

- Scope: `axl_ps` returning the process table; a `/proc`-style
  in-memory view; a minimal HTTP/WS control-plane API on top of
  axl-sdk's http server (routes: `GET /api/proc`,
  `POST /api/kill/:pid`, `GET /api/fds/:pid`, `POST /api/spawn`,
  plus a WS channel for live log streams). Token-based auth from
  day 1 — no open endpoint.
- Demo: `curl -H "Auth: …" http://target:8080/api/proc` returns
  the live process table. Same data mirrored in SoftBMC's debug
  dashboard.
- Exit criteria: API surface is documented and stable enough to
  hand off to phase K8's host tool.

### Phase K8 — `axl` host CLI  **[NOT STARTED]**

- Scope: a cross-platform CLI that consumes the K7 API. Written
  in Go or Python (pick one at phase start — either gives a
  standalone binary with readline and TLS out of the box).
  Subcommands: `axl ps`, `axl kill <pid>`, `axl fds <pid>`,
  `axl spawn <applet>`, `axl logs [-f]`, `axl inspect <pid>`.
- Demo: `axl ps` from an ops laptop against a live axl-kernel
  target over the same network SoftBMC uses.
- Exit criteria: the interactive story is solved without any
  target-side parser, line editor, or shell.

### Phase K9 (optional) — `axl-debug` bringup applet  **[NOT STARTED]**

- Scope: a tiny target-side UEFI app (~200 LOC) that links
  against axl-kernel and runs one subcommand from its argv:
  `axl-debug ps`, `axl-debug kill 5`, `axl-debug fds 3`. No
  parser, no REPL. Invoked from the UEFI shell when the network
  is down or the HTTP server is broken.
- Demo: serial console only, no network, operator can still get
  `ps` output.
- Exit criteria: only worth building if bringup forces it. Skip
  otherwise.

### Phase K10 — `axlk_offload`: AP compute pool  **[NOT STARTED]**

UEFI is single-threaded **on the BSP**, but the BSP is not the
only CPU. `EFI_MP_SERVICES_PROTOCOL` lets the BSP dispatch
procedures to Application Processors that then run *truly
concurrently* with BSP code. AXL already exploits this in
[src/task/axl-task-pool.c](../../src/task/axl-task-pool.c) —
persistent AP workers with lock-free volatile-slot dispatch and
per-worker arenas, no shared allocator pressure. Phase K10 lifts
that into a kernel syscall that composes with the fd-readiness
model.

**Why a phase, not a one-line wrapper:** the value isn't dispatch,
it's *cooperative blocking on completion*. A coroutine submits
work, yields, and is rescheduled when the AP finishes. That
requires a new fd kind whose readiness is driven by an AP-written
flag, not by `gBS->WaitForEvent`. Once that exists,
`axlk_offload` falls out as a thin wrapper over the existing task
pool.

- **Scope.**
  - New fd kind `AXLK_FD_OFFLOAD` whose readiness predicate watches
    `volatile uint32_t done` written by the AP.
  - New syscall `axlk_offload(fn, arg, arena_kb)` returns an fd.
    `axlk_read(fd, &result, sizeof(result))` cooperatively blocks
    until completion, copies the result struct, closes the fd.
  - `AxlLoop` already supports callback-driven event sources;
    `AXLK_FD_OFFLOAD` registers a poll predicate, no new scheduler
    code path.
  - Single-core fallback: if `axl_backend_mp_init` returns NULL,
    the syscall runs `fn(arg, arena)` synchronously on the BSP and
    the fd starts already-ready. Same call shape, transparent.
- **Demo.** A SoftBMC-shape applet whose `/sha256` endpoint
  hashes a multi-MB payload on an AP while a second concurrent
  HTTP connection to `/cpu` (HwInfo-style, BSP-only) returns
  instantly. Run on QEMU `-smp 4`.
- **Exit criteria.**
  1. Works on X64 and AARCH64.
  2. End-to-end latency on `/sha256` at least 1.5× faster than
     the pure-BSP baseline; concurrent `/cpu` shows no
     degradation in p99 latency.
  3. Single-core fallback path identical-API verified by setting
     QEMU `-smp 1`.
  4. Zero leaked tier-1 resources after 1000 offload cycles
     (existing tier-1 registry catches this).
- **Hard constraints (firmware-level, not AXL choices).**
  - AP procedures **cannot** call boot-services protocols. No
    AxlNet, AxlIO, AxlLog (touches console/file), no
    `LocateProtocol`, no `gBS->Allocate*`. Offloaded work must
    be pure compute over arena-allocated inputs.
  - Buffer ownership: caller transfers ownership to the AP for
    the duration of the offload and reclaims it on completion.
    No shared mutable state between BSP and AP without explicit
    `__sync_synchronize` barriers.
  - After `ExitBootServices`, MP services are gone. Not a kernel
    concern (we live in DXE/app context); flag it in the porting
    guide for OS-loader-shaped consumers.
- **Risks worth flagging at design time.**
  - Most firmwares disable APs when the BSP is at `TPL_CALLBACK`
    or higher. The kernel never raises TPL today, but
    `axlk_offload` should debug-assert `gBS->RaiseTPL(TPL_APPLICATION)`
    returns `TPL_APPLICATION`.
  - Compile-time enforcement of the "no boot services from AP"
    rule is impossible. Code review + a runtime assertion-loop
    in debug builds (e.g., AP procedure runs inside a guarded
    region that traps on suspicious calls) is the best we get.
  - The existing AxlTaskPool busy-spins APs on `cpu_pause`. Fine
    for compute pool; document that idle APs consume real power
    and operators must size workloads accordingly.
- **What this does NOT enable.**
  - Two TCP handlers running in parallel. The handler needs
    sockets/files/log — all BSP-only.
  - Migrating a coroutine mid-flight to an AP. Stacks contain
    pointers to BSP-only resources.
  - Preemptive scheduling. Cooperative-on-BSP remains the model.

Phase K10 is independent of K4 and K7 — could ship first if a
SoftBMC-shape consumer needs CPU-heavy endpoints.

Phases K1–K5 are ~2k LOC of kernel plus ~500 LOC of tests. K6 is
the decision gate: port HwInfo, measure, decide. K7–K8 deliver
the interactive story; K9 is optional; K10 is the AP-compute
extension and is independent of the interactive story.

## 10. Open questions / unknowns

### 10.1 How do you kill a process stuck in firmware code?

Async-path calls (`axl_recv`, `axl_sleep`) are interruptible via
the scheduler. A process that does `gBS->Stall(10s)` directly is
not — nothing can preempt Boot Services.

**Candidate answer:** discourage direct Boot Services calls in
kernel apps (document as "syscall around it"). Runaway detection:
if a process hasn't yielded in N seconds, the scheduler marks it
pending-kill and drops it at its next yield. If it never yields,
the UEFI watchdog fires and resets the machine. Same floor as
SoftBMC today.

### 10.2 Stack size — one-size or per-spawn?

- One-size: simpler, fits a fixed pool.
- Per-spawn: flexible, requires two pools (common + large) or
  direct allocation.

**Lean:** fixed-16 KiB default, `axl_spawn(..., stack_kib=64)`
falls back to `axl_malloc` for oversize. Measure in K6.

### 10.3 How much Linux API fidelity?

POSIX-compatible signatures (`read(int, void*, size_t)`) vs
axl-shaped (`axl_read(int, void*, size_t)` with different errno
style)?

**Lean:** axl-shaped with POSIX-compatible *semantics*. Return
`-1` on error and stash error in thread-local equivalent; provide
`axl_strerror`. Later, a `posix-compat.h` shim could add real
POSIX names.

### 10.4 TLS (thread-local storage) across yields

Stackful coroutines preserve locals on the stack. Does anything
need true TLS? Candidates: errno-like globals, strtok state, libc
reentrancy bits.

**Lean:** one per-process block on the PCB, accessed via
`axl_this_proc()->tls`. Functions that need "TLS" read from there.

### 10.5 Debugger story

Custom asm context switch breaks GDB's default stack unwinder on
the non-running processes. Can be fixed with a GDB Python helper
that walks the PCB table and synthesizes threads.

**Lean:** ship a GDB helper from day 1 (~50 lines Python). Same
pattern Linux kernel has for task_struct iteration.

### 10.6 Porting existing UEFI drivers

Drivers that expect EFI_BOOT_SERVICES semantics and synchronous
execution live "below" the kernel — they run in the shared address
space with no syscall wall. They're trusted code.

**Lean:** same model as Linux kernel modules. Drivers are
privileged; only user-level services go through the syscall layer.
Drivers stay the same; services get the new shape.

### 10.7 Interaction with AP offload

`AxlTask` already dispatches work to Application Processors via
`EFI_MP_SERVICES_PROTOCOL`. Kernel-wrapped version: `axlk_offload`
is a syscall that hands a pure-compute job to an AP and
cooperatively blocks the calling process until completion.

See [Phase K10](#phase-k10--axlk_offload-ap-compute-pool-not-started)
for the full design — it's been promoted from "lean wrapper" to
its own phase because the fd-based wake-up surface is what makes
it compose with the rest of the kernel.

## 11. What success looks like

- A SoftBMC-like application is 30-50% smaller in LOC and
  structurally clearer.
- Developers coming from Linux can write a UEFI service in an
  afternoon without learning callback patterns.
- Stack overflow / runaway process bugs are caught by the
  scheduler, not by "reboot the BMC."
- The existing axl-sdk callback APIs remain for low-level code;
  the kernel layer is an opt-in upgrade.
- An ops laptop running `axl ps` / `axl kill` / `axl inspect`
  against a live target behaves the way `kubectl` does against
  a cluster — full readline, autocomplete, scripting — without
  the target carrying a shell parser ([§12](#12-interactive-access--axl-cli--bringup-applet)).

---

## 12. Interactive access — `axl` CLI + bringup applet

The runtime needs an interactive face that understands
axl-kernel processes, fds, and signals. The interesting design
question isn't "do we want a shell" — it's **where does the
parser live**, because any interactive face we build has to
understand *something* axl-kernel-aware (UEFI Shell doesn't
know what a PID is, and never will).

### 12.1 Primary face: host-side `axl` CLI

A cross-platform CLI on the operator's laptop speaks to a
control-plane HTTP/WS API on the target:

```
$ axl ps
  PID  PARENT  STATE   STACK    NAME
    1       0  RUN     16K/16K  init
    2       1  WAIT    2K/16K   http-server
    3       1  WAIT    1K/16K   hwinfo-service
    4       1  WAIT    4K/16K   rfb-kvm-server
  ...

$ axl kill 4
killed pid 4

$ axl inspect 3
pid 3 (hwinfo-service), parent 1
  state:  waiting on fd 5 (tcp listener :8080)
  stack:  1344 / 16384 bytes (8%)
  fds:    3:stdin  4:stdout  5:tcp-listen(:8080)
```

Why this is the primary face:

- **Smaller target-side footprint.** ~500 LOC of control-plane
  route handlers on top of axl-sdk's existing HTTP server. No
  in-target parser, no line editor, no history, no applet
  registry. The parser lives on the laptop where parsers belong.
- **Richer tooling.** Host CLI gets full readline, colors,
  autocomplete, TLS, scripting via shell loops. The target
  doesn't have to reinvent any of this.
- **Matches BMC ops reality.** BMCs assume a management network;
  operators already talk to them from laptops. `axl ps` over
  that same network is the obvious shape — it's `kubectl` for a
  UEFI kernel.
- **Scripting is trivial.** Anything `axl` does is also a
  `curl` call. CI and monitoring tools can hit the API directly
  without parsing CLI output.

Language: Go or Python, decided at start of Phase K8. Both
cross-compile to a single binary; both have readline, TLS, JSON
out of the box. Repo: `aximcode/axl-ctl` (peer to
`aximcode/axl-kernel`).

### 12.2 Control-plane API (what the target exposes)

Strawman, to be firmed up in Phase K7:

| Route | Purpose |
|---|---|
| `GET  /api/proc` | process table (JSON) |
| `GET  /api/proc/:pid` | single process detail |
| `GET  /api/fds/:pid` | fd table for a process |
| `POST /api/kill/:pid` | send signal; body = `{"signo": 15}` |
| `POST /api/spawn` | spawn an applet by name; body = argv |
| `GET  /api/log?follow=1` | log stream (WebSocket upgrade) |
| `GET  /api/health` | liveness + version |

Auth: bearer token from day 1. No open endpoints. The token
lives in a file on the ESP and is surfaced via a local
`axl-debug token` applet (see [§12.4](#124-bringup-fallback-axl-debug-applet)) for first-time bootstrap.

TLS: optional at first (most BMC management networks are
already isolated), mandatory later. mbedTLS is already in
axl-sdk.

### 12.3 Why we're not building an in-target REPL

Earlier iterations of this doc proposed a BusyBox-style
target-side shell with a POSIX-lite parser and applet registry.
That idea was rejected once the comparison became clear:

| | Target LOC | Host tooling | Scripting | No-network bringup |
|---|---|---|---|---|
| In-target shell | ~2 000 | none | brittle | works |
| Host CLI + API | ~500 | full readline / colors | curl-native | broken |

Host CLI has the *smaller* target-side footprint — the
counter-intuitive result — plus vastly better tooling for zero
extra target-side cost. The only thing it gives up is
serial-only bringup, and [§12.4](#124-bringup-fallback-axl-debug-applet) handles that separately for a
fraction of the cost.

The in-target shell is a real option if a concrete use case
demands it (very constrained network, hard-to-reach device,
offline maintenance workflows). Revisit only if that use case
actually materializes.

### 12.4 Bringup fallback: `axl-debug` applet

For serial-only bringup — no DHCP, no HTTP server, just a UART
cable and EDK2 Shell — a tiny target-side applet covers the gap:

```
Shell> axl-debug ps
  PID  PARENT  STATE   STACK    NAME
    1       0  RUN     16K/16K  init
    ...

Shell> axl-debug kill 3
killed pid 3

Shell> axl-debug token
bootstrap token: ****************************************
```

Shape:

- A `.efi` application that links against `libaxl-kernel.a` and
  invokes kernel introspection APIs directly — no network, no
  parser, no REPL.
- One subcommand per invocation: `axl-debug <verb> [args]`.
  Argv parsing is standard `argc`/`argv`, not shell-style.
- Only works against a **running** axl-kernel (i.e., the kernel
  must be loaded as a co-resident image or via a debug
  attach). Alternate shape worth considering: a static-dump
  variant that reads a coredump-ish snapshot from a file if the
  kernel has crashed.
- Expected size: ~200 LOC including argv dispatch. Subcommands
  are thin shims over `axl_ps()` / `axl_kill()` / etc.

When it's worth building: only if Phase K6's dogfooding hits a
case where `axl` over the network isn't available. Skip it until
then.

### 12.5 Scope summary

| Component | Where it lives | When |
|---|---|---|
| Control-plane API | Target (axl-kernel) | K7 |
| `axl` host CLI | Operator laptop (axl-ctl repo) | K8 |
| `axl-debug` applet | Target (optional) | K9, only if needed |
| In-target REPL | *not built* | — |

The interactive story is solved by K7 + K8. K9 is on-demand.

---

## 13. POC specification — what gets built first

This section exists so that someone (me, likely) can start
writing code without re-deriving decisions. It pins the minimum
technical spec for phases K1 + K2 only. Everything else in this
document is longer-term map; this section is an immediate to-do
list.

### 13.1 Scope

**POC = K1 (context switch) + K2 (spawn/exit/wait).** Nothing
more.

**In scope:**
- `axl_ctx_switch(from, to)` asm primitive (x86-64 SysV only).
- `AxlProc` PCB struct.
- `axl_spawn`, `axl_exit`, `axl_wait`, `axl_yield`, `axl_sleep_ms`,
  `axl_getpid`, `axl_getppid`, `axl_this_proc`.
- A scheduler coroutine (pid 0) with ready queue + zombie reap.
- A stack pool (fixed-N slots, fixed-size stacks).
- Stack canary at the base of every coroutine stack.
- A test binary (`AxlKernelPoc.efi`) that demonstrates the primitives.
- A QEMU integration test (`test-kernel-poc.sh`).

**Explicitly not in POC:**
- fd table, `axl_read`/`write`/`open`/`close` (K3).
- Pipes, signals, timerfd, signalfd (K4).
- AARCH64 context switch (K5).
- HTTP control-plane API, `axl` host CLI, `axl-debug` applet (K7+).
- libc-shape compatibility shim.
- Integration with axl-sdk's net / HTTP / task primitives.
- SoftBMC port.

**Target size:** ~800 LOC of kernel + ~300 LOC of asm + ~400 LOC
of test. If we're pushing 2 000 LOC, we've over-scoped.

### 13.2 Repository layout

```
axl-sdk/experiments/axl-kernel/
  include/axl-kernel.h         public POC API (~80 LOC header)
  src/
    kernel.c                   scheduler, spawn, exit, wait
    ctx-switch-x86_64.S        the asm primitive
    stack-pool.c               fixed-slot allocator
  test/
    kernel-poc.c               AxlKernelPoc.efi source
  Makefile.mk                  included by top-level Makefile
  README.md                    pointer back to this section
```

Top-level `Makefile` gains one include and one new target
(`AxlKernelPoc.efi`). Integration test
`test/integration/test-kernel-poc.sh` mirrors the existing
`test-*` harness.

### 13.3 PCB struct

Concrete C, not prose:

```c
typedef enum {
    AXL_PROC_READY,   ///< on ready queue, waiting for CPU
    AXL_PROC_RUNNING, ///< currently executing
    AXL_PROC_WAITING, ///< blocked on something (sleep, child, event)
    AXL_PROC_ZOMBIE,  ///< exited, awaiting reap by scheduler
} AxlProcState;

typedef struct AxlCtx {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp;     ///< must be last — asm indexes by offset
} AxlCtx;  /* 56 bytes on x86-64 */

typedef struct AxlProc {
    AxlPid           pid;
    AxlPid           ppid;
    AxlProcState     state;
    AxlCtx           ctx;             ///< register save area

    uint8_t         *stack_base;      ///< lowest address (canary here)
    size_t           stack_size;
    int              stack_slot;      ///< index into stack_pool; -1 if heap

    AxlProcMain      entry;           ///< for trampoline to call
    int              argc;
    char           **argv;
    int              exit_status;     ///< valid only in ZOMBIE state

    /* Wait linkage */
    uint64_t         wake_at_ms;      ///< 0 = not sleep-waiting
    AxlPid           wait_for_pid;    ///< 0 = not child-waiting

    struct AxlProc  *next;            ///< intrusive list (ready/zombie)
    struct AxlProc  *children;        ///< head of child list (sibling-linked)
    struct AxlProc  *sibling;
} AxlProc;
```

`sizeof(AxlProc)` under 128 bytes. 16 PCBs = 2 KiB. Static-allocated
in the POC (no dynamic PCB allocation to start).

### 13.4 Scheduler model

**pid 0 is a real coroutine** — it has its own stack (allocated
at kernel init, separate from the user-process stack pool). Every
`axl_yield` / syscall context-switches back into pid 0. Pid 0's
body is the scheduler inner loop ([§13.8](#138-scheduler-inner-loop)). Every switch goes
through pid 0; there are no direct proc-to-proc switches. This
makes the reap-exited-stack problem trivial: the scheduler is
always running on its own stack when it decides what to free.

Two data structures live on the scheduler:

```c
static AxlProc *ready_head, *ready_tail;   /* FIFO ready queue */
static AxlProc *zombie_head;                /* LIFO zombie reap list */
```

`ready_head/tail` is doubly-trivial: pop from head, push to tail.

### 13.5 Context switch — ABI + asm sketch

**Target ABI:** x86-64 SysV (gcc default on freestanding). UEFI
Boot Services use MS x64, but those are called through
`axl_efi_call` which already adapts. Context switches between
coroutines are pure SysV.

**Callee-saved registers to preserve:** `rbx`, `rbp`, `r12`–`r15`,
`rsp`. That's it — caller-saved regs are the caller's problem.

**Ignored:** FPU/SSE state. Freestanding builds don't use SSE by
default (`-mno-sse` is implied by `-mno-red-zone` + `-ffreestanding`
conventions in axl-sdk's build). If that changes, we add `fxsave`
/ `fxrstor` to the switch. Out of scope for POC.

**No red zone.** UEFI already disables the 128-byte red zone
(`-mno-red-zone`). Good — we don't have to worry about
signal-handler-like scribbling.

**Stack alignment:** SysV requires 16-byte alignment at `call`
sites. We guarantee this at spawn by aligning the initial `rsp`
down to a 16-byte boundary *minus 8* (so the `ret` instruction in
the trampoline restores 16-byte alignment).

**Asm sketch** (`ctx-switch-x86_64.S`):

```asm
    .text
    .global axl_ctx_switch
    .type   axl_ctx_switch, @function

/* void axl_ctx_switch(AxlCtx *from, AxlCtx *to)
 *   rdi = from, rsi = to
 * Saves callee-saved regs into *from, restores from *to, returns
 * on the target coroutine's stack.
 */
axl_ctx_switch:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    movq    %rsp, 48(%rdi)

    movq     0(%rsi), %rbx
    movq     8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15
    movq    48(%rsi), %rsp
    retq
    .size axl_ctx_switch, .-axl_ctx_switch
```

~15 lines of asm. The `ret` pops the return address from the
target stack — which is how a freshly-spawned coroutine enters
its trampoline ([§13.6](#136-spawn--trampoline)).

### 13.6 Spawn + trampoline

```c
/* Stack layout for a freshly-spawned coroutine (grows downward):
 *
 *   stack_base + stack_size:  [end]
 *                             ...              (runtime stack)
 *   initial rsp:              &axl_proc_trampoline   <-- ret target
 *                             ... 16-byte alignment padding if needed
 *   stack_base + 8:           (canary MSB — checked on yield)
 *   stack_base:               (canary LSB)
 */

#define AXL_STACK_CANARY  UINT64_C(0xDEADBEEFCAFEBABE)

static void axl_proc_trampoline(void) {
    AxlProc *self = axl_this_proc();
    int rc = self->entry(self->argc, self->argv);
    axl_exit(rc);  /* noreturn */
    __builtin_unreachable();
}

AxlPid axl_spawn(AxlProcMain entry, int argc, char **argv,
                 size_t stack_kib)
{
    int slot;
    AxlProc *p = pcb_alloc(&slot);     /* static table entry */
    if (p == NULL) return -1;

    uint8_t *base = stack_pool_alloc(stack_kib ? stack_kib : 16);
    if (base == NULL) { pcb_free(p); return -1; }

    *(uint64_t *)base = AXL_STACK_CANARY;

    /* Set up initial frame so ctx_switch -> ret lands in trampoline. */
    uintptr_t top = (uintptr_t)base + p->stack_size;
    top &= ~((uintptr_t)15);          /* 16-byte align */
    top -= 8;                          /* ret pushes 8 off the top */
    *(uintptr_t *)top = (uintptr_t)axl_proc_trampoline;

    p->pid         = next_pid();
    p->ppid        = current_proc->pid;
    p->state       = AXL_PROC_READY;
    p->stack_base  = base;
    p->stack_slot  = slot;
    p->entry       = entry;
    p->argc        = argc;
    p->argv        = argv;
    p->ctx.rsp     = top;
    /* Other regs don't matter — trampoline doesn't rely on any. */

    ready_queue_push(p);
    child_list_link(current_proc, p);
    return p->pid;
}
```

Argument passing: stashed in PCB (`argc`, `argv`). Trampoline
pulls them from `axl_this_proc()`. Simplest working design — no
register-juggling across the switch.

### 13.7 Exit + zombie reap

```c
void axl_exit(int status) {
    AxlProc *self = current_proc;
    self->state       = AXL_PROC_ZOMBIE;
    self->exit_status = status;

    /* Push onto scheduler's zombie list — it will reap us. */
    self->next = zombie_head;
    zombie_head = self;

    /* Wake any axl_wait() parent. */
    AxlProc *parent = pcb_by_pid(self->ppid);
    if (parent != NULL && parent->state == AXL_PROC_WAITING
        && (parent->wait_for_pid == 0 || parent->wait_for_pid == self->pid))
    {
        parent->state = AXL_PROC_READY;
        parent->wait_for_pid = 0;
        ready_queue_push(parent);
    }

    /* Never returns — switch to scheduler (pid 0). */
    switch_to_scheduler();
    __builtin_unreachable();
}

/* Runs on pid 0's stack — safe to free exited stacks here. */
static void reap_zombies(void) {
    while (zombie_head != NULL) {
        AxlProc *z = zombie_head;
        zombie_head = z->next;
        stack_pool_free(z->stack_slot);
        pcb_free(z);
    }
}
```

**Invariant:** `stack_pool_free()` is only called from pid 0,
never from the exiting coroutine itself. The zombie list is the
handoff.

### 13.8 Scheduler inner loop

Pid 0's body, in pseudocode:

```c
static void scheduler_main(void) {
    for (;;) {
        reap_zombies();

        /* Wake any sleepers whose deadline elapsed. */
        uint64_t now = axl_backend_time_ms();
        for (AxlProc *p = sleep_list; p; p = p->next) {
            if (now >= p->wake_at_ms) {
                p->state = AXL_PROC_READY;
                p->wake_at_ms = 0;
                ready_queue_push(p);
            }
        }

        AxlProc *next = ready_queue_pop();
        if (next == NULL) {
            /* Nothing ready — block on the soonest wake event.
               For POC, a crude sleep is fine: */
            axl_backend_stall(1000);   /* 1 ms */
            continue;
        }

        current_proc = next;
        next->state  = AXL_PROC_RUNNING;
        axl_ctx_switch(&sched_ctx, &next->ctx);
        /* Control returns here when next yields or exits. */
    }
}
```

Cooperative tick rate is whatever the hottest syscall decides.
For POC we don't need to integrate with `AxlLoop` yet — a naked
scheduler with its own sleep queue is simpler and proves the
mechanics. Integration with `AxlLoop` (so that timer/event
sources drive wakeups instead of polled sleep) is a K3/K4
concern.

### 13.9 Stack canary — overflow detection

```c
static inline void check_canary(AxlProc *p) {
    if (*(uint64_t *)p->stack_base != AXL_STACK_CANARY) {
        axl_log_fatal("stack overflow: pid %u (%s)", p->pid, p->name);
        axl_backend_boot_exit(EFI_ABORTED);
    }
}
```

Called from `axl_yield` (and every syscall that yields) for the
*outgoing* proc, and from the scheduler for the *incoming* proc
just before the switch-in. Two checks per switch; O(1) cost per
yield; catches any write that trampled the bottom 8 bytes of the
stack.

Not a perfect guard — a write that skips past the canary will
evade detection. But it catches the overwhelming majority of
overflow bugs in practice, and it's cheap enough to always be on.

### 13.10 Public POC API

This is the *entire* surface the POC exposes. Keep it this small.

```c
#ifndef AXL_KERNEL_H
#define AXL_KERNEL_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t AxlPid;
#define AXL_PID_ANY  ((AxlPid)-1)

typedef int (*AxlProcMain)(int argc, char **argv);

/* Kernel bring-up (called once from main) */
int  axl_kernel_init(void);
int  axl_kernel_run(AxlProcMain pid1_entry, int argc, char **argv);

/* Process control */
AxlPid axl_spawn(AxlProcMain, int argc, char **argv, size_t stack_kib);
int    axl_wait(AxlPid pid, int *status);       /* AXL_PID_ANY = any child */
int    axl_waitpid(AxlPid pid, int *status, int flags);  /* AXL_WNOHANG */
void   axl_exit(int status) __attribute__((noreturn));
AxlPid axl_getpid(void);
AxlPid axl_getppid(void);

/* Time / yield */
void   axl_yield(void);
void   axl_sleep_ms(uint32_t ms);

/* Introspection (for tests) */
struct AxlProc;
const struct AxlProc *axl_this_proc(void);
size_t axl_proc_count(void);

#endif
```

13 public functions. No fds, no syscalls beyond sleep/yield/wait,
no networking. Everything a real kernel needs comes later.

### 13.11 POC success criteria

All three must pass before we declare the approach viable and
graduate to a standalone repo:

1. **Ping-pong demo** — two coroutines yielding to each other a
   million times. No crashes, no corruption. Wall-clock time gives
   us a ballpark per-switch cost (target: <2 µs on QEMU, <1 µs on
   real hardware).

2. **64-process stress** — spawn 16 children (pool limit), each
   sleeps a random 1–100 ms then exits with a known status code;
   parent waits on each; verify every status code matches. Repeat
   10 rounds. Any leak (PCB, stack slot) after the 10th round
   fails the test.

3. **Stack overflow detection** — a test proc that deliberately
   recurses past its stack. The canary check must trigger and the
   kernel must log + exit cleanly, not silently corrupt the next
   process's stack.

4. **Ctrl-C handling (bonus)** — SIGINT during the stress test
   terminates all processes cleanly and the kernel exits to UEFI
   shell with status `EFI_ABORTED`. Tier-1 registry sweep finds
   zero leaks.

The QEMU integration test `test-kernel-poc.sh` encodes criteria
1–3 as pass/fail checks on serial-log grep. Criterion 4 is
validated manually during POC demo.

### 13.12 Open questions deferred past POC

These don't block K1/K2 and should not be designed speculatively:

- **When to integrate with `AxlLoop`.** Probably K3 when fds
  arrive — the scheduler's "wait for next wake" becomes
  `axl_loop_next_event` at that point. For POC, the crude 1 ms
  sleep in the scheduler loop is fine.
- **Dynamic vs static PCB allocation.** Static (fixed table of 16)
  for POC. Revisit when the SoftBMC port exposes the real
  concurrency ceiling.
- **Stack size per process.** POC uses fixed 16 KiB. Variable
  sizes arrive when we port real code and measure.
- **Debugger helper.** GDB Python script to walk the PCB table
  and synthesize threads — nice-to-have, not POC-blocking.

---

## Appendix A — decision log

Captured here so they don't keep getting re-litigated:

- **Core primitive is stackful coroutine, not stackless.** The
  cost (one stack per process) buys the property that matters
  most: sequential code model. See [§3.2](#32-why-stackful-not-stackless).
- **No fork() with address-space duplication.** UEFI has no MMU;
  copying address space is meaningless. `axl_spawn(entry, argv, ...)`
  is the primitive; users who want fork+exec write a single spawn.
- **New layer, not in-tree.** axl-sdk stays a primitive library.
  axl-kernel is a peer repo that consumes it. See [§5](#5-relationship-to-axl-sdk).
- **One loop, many stacks — not many loops.** Sidesteps the
  hierarchical-loop problems rejected in [AXL-Runtime.md §5.4](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md#54-true-nested-loops-inner-loop-runs-while-outer-is-running).
- **Cooperative only.** No attempt at preemption. Runaway detection
  is a best-effort mitigation, not a correctness property.
- **Libc-shape API.** Function names are `axl_read` etc., but
  semantics track POSIX closely enough that a compat shim is easy.
- **SoftBMC HwInfo module port is the go/no-go gate.** If that
  port isn't clearly simpler than the current version, the layer
  isn't paying for itself.
- **Interactive face is host-side, not target-side.** An `axl`
  CLI on the operator's laptop talks to a control-plane HTTP/WS
  API. No target-side REPL. The reasoning that led here: UEFI
  Shell doesn't know about axl-kernel processes, so *any*
  axl-kernel-aware interaction needs new target-side code — and
  a control-plane router is ~500 LOC where an in-target shell
  is ~2 000 LOC, for strictly better tooling. See [§12](#12-interactive-access--axl-cli--bringup-applet).
- **Tool name is `axl`, not `axlcmd` / `axlctl`.** Matches the
  `git` / `cargo` / `kubectl` single-word-plus-subcommand
  convention; ties directly to the project identity.

## Appendix B — rejected earlier, preserved for context

Previous design conversations concluded against stackful coroutines
(see [AXL-Concurrency.md](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md)) on memory-cost and
debugger-cost grounds. This design reopens that conclusion because:

1. The memory-cost argument was tuned for PEI/DXE-early constraints
   that don't apply at BDS/Shell/App stage where this runtime lives.
2. The debugger-cost argument is real but addressable with ~50
   lines of GDB Python.
3. The primary benefit (sequential code, per-process isolation)
   materially addresses a class of pain that the callback model
   keeps imposing on real consumers (SoftBMC).

Neither argument against was wrong. The target footprint changed.
