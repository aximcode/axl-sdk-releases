# axl-kernel POC

**Status:** K1 + K2 + K3 + K5 + K6 landed (April 2026), plus two
additional SoftBMC-shape ports (BootConfig — UEFI NVRAM; ReqLog —
RAM-resident ring buffer). All tests pass on both **X64 and AARCH64**.
Experimental. Lives in-tree until the approach is validated, then
graduates to its own repo `aximcode/axl-kernel`
(see [docs/AXL-Kernel-Design.md §5](../../docs/AXL-Kernel-Design.md#5-relationship-to-axl-sdk)).
The K6 SoftBMC-HwInfo port is the design doc's go/no-go gate and
it has passed — see the comparison section below.

**Paused — picking this up again?** See the bottom of this file
("Resume context") for where things stand, what's next, and
recommended next steps. The design doc's §9 phase plan has
status callouts against each phase.

Cooperative coroutine scheduler for UEFI: each process is a stackful
coroutine with its own 16 KiB stack. The scheduler is a plain loop
that context-switches between ready processes via ~15 lines of
x86-64 SysV assembly, with an `AxlLoop` underneath servicing
timers, fd completions, and the shell-break event via
`gBS->WaitForEvent` — no busy-spin.

## What ships in this POC

| File | Purpose |
|---|---|
| [include/axl-kernel.h](include/axl-kernel.h) | Public API (18 functions) |
| [src/ctx-switch-x86_64.S](src/ctx-switch-x86_64.S) | x86-64 SysV callee-saved swap + rsp |
| [src/ctx-switch-aarch64.S](src/ctx-switch-aarch64.S) | AAPCS64 callee-saved swap + sp + LR |
| [src/kernel.c](src/kernel.c) | PCB table, stack pool, scheduler, fd table, TCP syscalls |
| [test/kernel-poc.c](test/kernel-poc.c) | K1+K2 driver — ping-pong + stress + stack-canary |
| [test/axlk-echo-server.c](test/axlk-echo-server.c) | K3 driver — TCP echo server, fork-per-connection |
| [test/axlk-hwinfo-server.c](test/axlk-hwinfo-server.c) | K6 driver — HTTP HwInfo server, SoftBMC-shape |
| [test/axlk-bootconfig-server.c](test/axlk-bootconfig-server.c) | 2nd port — BootConfig (reads UEFI NVRAM, parses EFI_LOAD_OPTION) |
| [test/axlk-reqlog-server.c](test/axlk-reqlog-server.c) | 3rd port — ReqLog (RAM-resident ring buffer, cross-request state) |

Public API (entire surface):

```c
/* K1 + K2 — process control */
int     axlk_init(void);
int     axlk_run(AxlkProcMain entry, int argc, char **argv);
AxlkPid axlk_spawn(AxlkProcMain, int argc, char **argv, size_t stack_kib);
AxlkPid axlk_wait(AxlkPid pid, int *status);
AxlkPid axlk_waitpid(AxlkPid pid, int *status, int flags); /* AXLK_WNOHANG */
void    axlk_exit(int status) __attribute__((noreturn));
void    axlk_yield(void);
void    axlk_sleep_ms(uint32_t ms);
AxlkPid axlk_getpid(void);
AxlkPid axlk_getppid(void);
size_t  axlk_proc_count(void);

/* K3 — fd table + TCP syscalls */
int     axlk_listen(uint16_t port);
int     axlk_accept(int listen_fd);
int     axlk_read(int fd, void *buf, size_t n);
int     axlk_write(int fd, const void *buf, size_t n);
void    axlk_close(int fd);
```

## Build + run

```
# X64 (native)
make ARCH=x64 kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server
./test/integration/test-kernel-poc.sh      --arch X64     # K1+K2
./test/integration/test-axlk-echo.sh       --arch X64     # K3
./test/integration/test-axlk-hwinfo.sh     --arch X64     # K6 (HwInfo port)
./test/integration/test-axlk-bootconfig.sh --arch X64     # BootConfig port
./test/integration/test-axlk-reqlog.sh     --arch X64     # ReqLog port (RAM ring)

# AARCH64 (cross)
make ARCH=aa64 kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server
./test/integration/test-kernel-poc.sh      --arch AARCH64 # K1+K2
./test/integration/test-axlk-echo.sh       --arch AARCH64 # K3
./test/integration/test-axlk-hwinfo.sh     --arch AARCH64 # K6
./test/integration/test-axlk-bootconfig.sh --arch AARCH64 # BootConfig port
./test/integration/test-axlk-reqlog.sh     --arch AARCH64 # ReqLog port
```

## POC measurements (QEMU + KVM on the dev machine)

**K1 + K2:**
- **400 000 context switches in <11 ms total.** Per-switch cost
  below `axl_time_get_ms` resolution (<28 ns / switch). Design
  target was <2 µs. ~70x headroom.
- **40 spawn/exit/wait cycles across 4 rounds** with no leaks.
  Tier-1 registry reports 0 leaked resources on exit.
- **Stack canary trips cleanly** when a deliberately-recursive
  process overflows its 16 KiB stack.

**K3:**
- **3 sequential TCP clients**, each handled by its own spawned
  handler process, each doing straight-line `axlk_read /
  axlk_write / axlk_close` until peer disconnect. Parent reaps
  handlers via `axlk_wait(AXLK_PID_ANY)`. Clean exit, zero leaks.
- Handler processes overlap (pid 3 and pid 4 both running before
  pid 2 is reaped) — genuine concurrent I/O, not
  one-client-at-a-time.
- Scheduler idle path goes through `axl_loop_next_event` +
  `gBS->WaitForEvent`. QEMU CPU does not pin.

**K6 (go/no-go gate — HwInfo HTTP server):**
- **3 HTTP endpoints** (`/`, `/system`, `/cpu`) served over TCP at
  port 8080, each request handled in its own spawned process.
- Real SMBIOS data: QEMU returns its BIOS vendor ("EDK II"),
  system manufacturer ("QEMU"), product name ("Standard PC (Q35
  + ICH9, 2009)"), memory size (530 MB), CPU info.
- 404 for unknown paths, Content-Length headers, clean close.
- 5 clients × 4 endpoint invocations + 1 × 404 across test run.
  Clean kernel exit, `mem: no leaks detected`.

**2nd port — BootConfig (state-heavier pressure test):**
- **3 HTTP endpoints** reading real UEFI NVRAM variables
  (`BootOrder`, `BootNext`, `Timeout`, `SecureBoot`, `Boot####`)
  via `axl_nvstore_get`. Parses binary `EFI_LOAD_OPTION` records
  including UCS-2 → ASCII conversion of the Description field.
- Real boot entries from QEMU:
  - X64: `"UEFI QEMU HARDDISK QM00001"`, `"EFI Internal Shell"`, PXE/HTTP entries.
  - AARCH64: `"BootManagerMenuApp"`, `"EFI Firmware Setup"`, `"UEFI Non-Block Boot Device"`, etc.
- Same sequential-process shape as HwInfo — validates the model
  holds when the workload moves from SMBIOS scrape to binary
  NVRAM parse.

**3rd port — ReqLog (cross-request RAM state):**
- The one shape the prior two don't cover: a module-level ring
  buffer that *every* request mutates, with bounded memory and
  observable cross-request side effects. SoftBMC's log ring,
  telemetry counters, and session table all share this shape.
- **3 endpoints**: `/` (overview: capacity, received, dropped,
  head), `/log` (oldest→newest entries as JSON, up to capacity),
  `/healthz` (varies the recorded path mix).
- Test drives the ring past capacity to validate wrap-around and
  that `dropped` increments. Receives 11 requests against an
  8-entry ring and confirms `dropped=3`, `received=11`,
  buffer holds the 8 most recent.
- **No locks required.** Child handlers only yield on
  `axlk_read` / `axlk_write`; the append between
  "compute slot" and "store entry" has no syscall, so the
  scheduler can't preempt mid-mutation. Counters increment
  atomically from the scheduler's perspective. This is the
  cooperative-coroutine analogue of "the kernel preemption
  point can't fall here."
- **Confirms the shared-address-space assumption.** All
  "processes" are coroutines in one UEFI image, so a global
  struct is shared by construction — handlers don't need IPC
  to see each other's mutations. Tested directly: handlers
  see the cumulative counter on every request.
- Handles 24 sequential connections against a 16-slot PCB
  by draining handler zombies inline via `axlk_waitpid(AXLK_PID_ANY,
  NULL, AXLK_WNOHANG)` at the top of the accept loop. This is the
  pattern services want: slots recycle immediately instead of
  accumulating until the post-loop reap. Earlier revisions without
  this primitive capped out at ~14 connections total.

## K6 go/no-go assessment — honest read

Design doc §9 K6 exit criterion: *"If the port isn't meaningfully
shorter / simpler, stop and reassess."* Here's the honest
comparison.

### LOC at the example level

| Component | LOC |
|---|---|
| `axlk-hwinfo-server.c` (3 endpoints, standalone, embedded HTTP) | 295 |
| SoftBMC `HwInfoModule.c` (9 endpoints, framework-embedded, no HTTP parser) | 534 |
| SoftBMC `HwInfoModule.c` per endpoint | ~59 |
| axlk-hwinfo-server per endpoint | ~25 (excluding 80 LOC of in-file HTTP plumbing) |

The raw standalone-vs-standalone comparison isn't clean because
SoftBMC has HTTP in a shared 1 805-LOC server under it while mine
inlines a 60-LOC HTTP/1.0 shim. If the axl-kernel side grew a
shared `axlk_http.c` library, per-endpoint cost converges to ~25
LOC vs SoftBMC's ~59 — a ~60% reduction — and the reduction
scales with endpoint count.

### The architectural delta (the real reason this ships)

Raw LOC is a weak signal. The shape difference is the strong one:

| Concern | SoftBMC module | axl-kernel process |
|---|---|---|
| **Per-request state** | must be stateless or use void\* sidecar; static locals don't persist correctly | stack locals in the handler proc |
| **Lifecycle hooks** | Init + Cleanup + Poll + Persist callbacks (even if empty) | stack unwind from `main` return |
| **Registration** | `SOFTBMC_MODULE` struct + static route table + added to `gModules[]` | none — a standalone `main()` |
| **Long-running work** | hand-split across `Poll()` ticks (see `RfbPoll()` encoding one tile per call) | just a loop in the handler proc |
| **Blocking by accident** | any module's `gBS->Stall(1s)` freezes the entire loop (all clients, all modules) | blocks only that one handler proc; other procs keep running |
| **Cleanup ordering** | hand-coded reverse order in ModuleManager | process tree unwinds naturally; parent waits for children |
| **Cross-module data** | `QueryHandler` string-keyed lookup | regular function calls |
| **Per-client state** | `SoftBmcSetClientData()` sidecar + manual disconnect hook | local vars on the handler proc's stack |

### Verdict

**Go.** The port is shorter per endpoint once HTTP is amortized,
and *structurally* it eliminates every item on the right column
of that table. The abstraction is paying for itself.

What K6 didn't prove yet — and the design doc never asked it to:

- Concurrent HTTP (e.g. 100 in-flight requests). 16-proc cap is
  plenty for HwInfo but not for a real BMC. Raising the cap or
  moving to dynamic PCB alloc is a post-K6 concern.
- WebSocket. SoftBMC uses WS for the browser xterm; porting that
  needs an HTTP upgrade handler + streaming. Not trivial, not
  blocked by architecture.
- State persistence across OS handoff (SoftBMC's `Persist()`
  hook). Would live as a per-process `axlk_atexit` shim in the
  kernel layer.

Nothing in those blockers says the shape is wrong. They say we
have more features to build.

## POC scope — what this does NOT include

- No pipes, signals, signalfd, timerfd (K4).
- fd table is kernel-global — any process can use any fd number it
  knows. Per-process fd tables are post-K3.
- Only TCP fds; no files, no UDP, no console fds yet.
- No HTTP control-plane API, no `axl` host CLI (K7 / K8).
- No libc-shape compatibility shim.
- Public symbols use `axlk_*` / `Axlk*` prefix; design doc's
  `axl_*` names are aspirational for the eventual standalone repo.

## Known warts

- PCB table and stack pool fixed at 16 slots; fd table at 32
  slots. All return `-1` when full.
- Only one stack size supported (16 KiB). `stack_kib` parameter
  exists on `axlk_spawn` but rejects non-default values.
- `axlk_sleep_ms` still uses the pre-K3 `sleep_head` polling
  scheme (not loop timeouts). Works; not exercised by K3 demos.
- Kernel-global fd table means no isolation between processes —
  any process can `axlk_close` any fd. Fine for POC; proper
  per-process tables arrive post-K3.
- `axl_time_get_ms()` in the scheduler's wake-sleepers path goes
  through UEFI `RuntimeServices->GetTime` (~10 µs/call). Guarded
  behind `sleep_head != NULL`.

## Lessons worth capturing

**x86-64 context-switch alignment.** Initial PCB setup must
place the entry-point address such that, after the first `ret`
pops it, rsp is 16-aligned + 8 — matching SysV's callee-entry
invariant `(rsp + 8) % 16 == 0`. Getting this wrong manifests as
`#GP` from any `movaps` gcc emits for SSE-aligned stack locals
(e.g., zeroing a local array). The fix: `ctx.rsp` on first
switch must be 16-aligned, with the trampoline address stored at
that 16-aligned slot. See the comment block in
[src/kernel.c](src/kernel.c) above `axlk_spawn`'s stack setup.

**AArch64 context switch is simpler.** AAPCS64's `ret` branches
to LR (x30) rather than popping the stack, so the trampoline
address goes directly into `ctx.x30` — no stack push, no
alignment dance. `ctx.sp` is 16-aligned and the ABI's entry
invariant is already satisfied (AAPCS64 requires sp 16-aligned
at entry, not 16-aligned + 8 like SysV x86-64). Callee-saved
set: x19–x28 + x29 (FP) + x30 (LR) + sp = 13 regs, vs x86-64's
7. Porting the POC to aa64 was ~40 lines of asm plus an
`#ifdef`-ed `AxlkCtx` — zero logic changes in `kernel.c`
beyond the initial-rsp/entry setup.

---

## Resume context (for picking this up later)

### Where it stands

Eight commits on `origin/main`, unpaused, tree clean:

| Commit | Scope |
|---|---|
| `613bb7e` | `docs: add AXL-Kernel-Design.md` |
| `01cfce5` | K1+K2 — coroutines working on UEFI |
| `6823352` | Axlk\* types, document PCB, fix idle busy-spin |
| `4eae073` | K3 — fd table, TCP syscalls, AxlLoop |
| `4cf981d` | K6 — HwInfo port passes the go/no-go gate |
| `3d31814` | K5 — AARCH64 support, all tests green on aa64 |
| `f5c233d` | 2nd SoftBMC port — BootConfig reads UEFI NVRAM |
| `(this)`  | 3rd SoftBMC port — ReqLog (RAM-resident ring buffer) |

All integration tests pass on both X64 and AARCH64:
- `test-kernel-poc.sh` — K1+K2 ping-pong + spawn/wait stress + canary (5/5)
- `test-axlk-echo.sh` — K3 fork-per-connection echo (7/7)
- `test-axlk-hwinfo.sh` — K6 HwInfo port (all endpoints)
- `test-axlk-bootconfig.sh` — BootConfig port (all endpoints)
- `test-axlk-reqlog.sh` — ReqLog port (ring wrap, drop counter, post-wrap log)

### What's been proved

- Cooperative stackful coroutines work on UEFI, both archs.
- fd table + TCP syscalls integrate cleanly with AxlLoop.
- Linux-shape HTTP services port cleanly to the sequential-process model.
- Three distinct SoftBMC workload shapes (stateless SMBIOS scrape,
  NVRAM parse, RAM-resident ring buffer with cross-request mutation)
  all produce near-identical port code — the abstraction carries
  weight across the shape spectrum the design doc anticipated.
- Cooperative scheduling makes per-request state mutation lock-free
  by construction (no syscall between read-modify-write = no preempt
  point), as confirmed by ReqLog's unlocked counter increments.
- ~30 ns/switch on KVM; zero memory leaks across all scenarios.

### What's not yet proved (remaining risks)

- **K4 deferred** — signals, pipes, signalfd, timerfd. None of
  the ports needed them, but inter-process communication /
  graceful shutdown may.
- **POST / request-body parsing.** All three ports are GET-only.
  Mutation endpoints with request bodies are untouched. (ReqLog
  mutates state on every request, but via the URL-only side effect
  of being recorded — no body parsing.)
- **WebSocket / streaming.** SoftBMC RemoteKvm and RemoteShell
  use WS; not tried.
- **True concurrency > 16 procs at once.** PCB table is fixed. The
  new `axlk_waitpid(AXLK_WNOHANG)` primitive lets sequential
  workloads run unbounded on a 16-slot table (as ReqLog's 24-conn
  test shows), but genuinely concurrent fan-out is still capped.
  Grow the table, or go dynamic.
- **POST-abort cleanup.** Runaway / killed processes.

### Options when resuming, ranked

The 3rd-port pressure test is now done; the design's last
unvalidated shape (RAM-resident cross-request state) holds.
Remaining options are all "extend a validated design":

1. **Declare victory, spin off `aximcode/axl-kernel` repo.** The
   POC has now passed every shape the design doc anticipated
   (K6 HwInfo, NVRAM, RAM ring) plus all named exit criteria.
   This is the recommendation — every remaining phase adds
   *features* to a validated design.
2. **K4 — signals + pipes.** Next natural primitive. Unblocks
   SIGINT-to-group and shell-style pipelines.
3. **POST + request-body parsing.** Small, unblocks mutation
   endpoints (set BootNext, etc.).
4. **Concurrent-client stress, crash scenarios, 100-client flood.**
   The sequential connection cap is now gone (WNOHANG), but
   truly-concurrent fan-out beyond ~15 active children is still
   bounded by the fixed PCB.

### To resume: reading order

1. This README (you're here).
2. [docs/AXL-Kernel-Design.md](../../docs/AXL-Kernel-Design.md) — especially §9
   phase plan (status callouts on each phase) and §13 POC spec.
3. [experiments/axl-kernel/src/kernel.c](src/kernel.c) — the
   scheduler + fd layer + syscall pattern.
4. [experiments/axl-kernel/test/axlk-hwinfo-server.c](test/axlk-hwinfo-server.c),
   [axlk-bootconfig-server.c](test/axlk-bootconfig-server.c),
   and [axlk-reqlog-server.c](test/axlk-reqlog-server.c) —
   reference port shapes (stateless / NVRAM / RAM-ring).

### Gotchas not to re-discover

- **Idle-path busy-spin is a landmine.** `axl_backend_stall`
  pins host CPU in QEMU; the scheduler uses
  `axl_loop_next_event(sched_loop, true)` + `gBS->WaitForEvent`.
  Don't revert.
- **x86-64 initial rsp alignment is off by 8 from the obvious.**
  SysV ABI needs `(rsp + 8) % 16 == 0` at entry. Manifests as
  `#GP` on `movaps`. Spelled out in comments around
  [axlk_spawn](src/kernel.c).
- **Kernel's own types use `axlk_` prefix**, not `axl_`, to avoid
  colliding with axl-sdk's `axl_exit` / `axl_yield`. That's
  intentional.
- **Hot paths use intrusive linked lists**, not AxlQueue /
  AxlList — AXL's container APIs allocate per-enqueue via
  `axl_malloc`. Same reason Linux uses `list_head`. Don't swap.
- **fd table is kernel-global** in the POC (any process can use
  any fd). Per-process tables are a post-K3 improvement; design
  doc covers the intent.
