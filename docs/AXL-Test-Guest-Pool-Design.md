# AXL-Test-Guest-Pool-Design — resident UEFI guests instead of a boot per test

**Status: SHIPPED, on `main` since 2026-11-09** (proposed 2026-11-03). The
`test/pool/axlpool` service runs as socket-activated systemd user units from
the main checkout (§6.7, `axlpool units`); the integration suite and verify's
unit selection lease from it, and a test that declares no `guest=` class, or
whose machine no class gives, still boots. Its successor is **qemu-pool**, a
Rust rebuild in a sibling repo (`aximcode/qemu-pool`, `docs/DESIGN.md`) that
replaces this service once it reaches parity. The sections below record the
design in the order it was measured and built. Goal stated by Mike
2026-11-03: cut the integration suite's **wall-clock time**; the number of QEMU
instances spun up per run is the pain. Sequenced with the CTest port
(`AXL-Build-System-Design.md` slice 5a), which is the layer that hands a test
its guest.

## 1. Where the time goes today

Two profiled, UNCACHED runs on 2026-11-03 (`AXL_TEST_PROFILE`,
`lib/profile-report.py`), 10 workers, the box otherwise idle:

| | X64 | AARCH64 |
|---|--:|--:|
| wall clock | **1,009 s** | **754 s** |
| test work (sum of test durations) | 9,262 s | 6,740 s |
| packing | 9x | 9x |
| guest boots recorded | 304 | 138 |
| tests recording no boot | 82 | (see below) |

Packing is 9x on 10 workers, so **wall clock is total work / workers** -- a
saving has to come out of the work. The boot count is a FLOOR: 82 X64 tests
record no boot, and while many are host-only, the `sshd` suites boot by a path
the instrumentation does not see (§12.11.1 of `AXL-CI-Release-Speed-Design.md`
has the same gap).

## 2. What a resident guest measures (spike, 2026-11-03)

`run-qemu.sh --background` with serial and QMP on UNIX sockets and a host
directory over virtio-fs, driven by `qemu.qmp` (QEMU's own Python QMP library)
and `pexpect`. Three runs per arch on a quiet box; the runs agree to a few
milliseconds. QEMU 10.0.0, virtiofsd 1.13.3.

| | X64 (KVM) | AARCH64 (TCG) |
|---|--:|--:|
| fresh launch to Shell prompt (`run-qemu.sh` included) | **4.5 s** | **8.9 s** |
| of which `run-qemu.sh` before QEMU starts (bash) | 1.1 s | ~1.1 s |
| QMP `system_reset` to prompt | 0.80 s | 5.6 s |
| QMP `snapshot-save` at the prompt | 0.20 s | 0.21 s |
| **QMP `snapshot-load`** | **0.065 s** | **0.089 s** |
| idle guest at the prompt, host CPU | 0.01 cores | 0.007 cores |
| typed command over serial (`echo hi` + status echo) | 336 ms | 336 ms |
| each further typed character | ~10 ms | ~10 ms |
| typed command over the guest's `sshd` | 604 ms | -- |
| **mailbox command over virtio-fs** (stand-in agent) | **23 ms** | **19 ms** |
| `ssh` connect / reconnect after `snapshot-load` | 0.20 / 0.18 s | -- |
| SFTP put, 5 images, 392 KB | 0.45 s | -- |

Facts the design rests on, each observed, not inferred:

- **`snapshot-load` reverts guest state** (a Shell variable set after the save
  is gone after the load) and the guest answers 20-35 ms later.
- **virtio-fs survives it**, and sees host changes made after the load. QEMU
  refuses internal snapshots with a raw writable disk, so the VM state goes to
  a dedicated qcow2 node (`-blockdev ...,node-name=vmstate`) and
  `snapshot-save`/`snapshot-load` name only that node; the raw FAT disk and the
  vars pflash are left out, which is safe because nothing writes them after
  the save.
- **virtio-fs is live**: a file changed on the host is read changed by the
  running guest, and a newly copied `.efi` runs, with no remount.
- **Typing is the slow channel, on both transports.** The cost is per
  character and identical on both arches -- the firmware's console input
  path, not the CPU. `sshd` is slower still: its input lands in the same
  console the Shell polls, plus a mirror repaint.
- **Idle guests are free.** A pool of parked guests does not hold cores.

### 2.1 Whole commands over TCP: a REST agent (spike, 2026-11-03)

A Shell application (`AxlHttpServer`, run in the foreground) taking `POST
/exec` with a whole command line, running it through
`EFI_SHELL_PROTOCOL.Execute`, and answering with the command's status and its
output (captured through an ASCII redirect). The guest has a virtio-net NIC
with a slirp hostfwd; one line is typed to start the agent and nothing after.
Two runs per arch, agreeing:

| | X64 | AARCH64 |
|---|--:|--:|
| prompt to agent serving (network bring-up) | 4.2 s, once per snapshot | 4.2 s |
| `GET /ping`, keep-alive / new connection | 10 / 30 ms | 0.3 / 1.0 ms |
| `echo hi`, output captured | 40 ms | 60 ms |
| the same with 60 more characters | 40 ms | 61 ms |
| an app run off the virtio-fs share | 47-54 ms | 99 ms |
| first command after `snapshot-load` | 73-79 ms, state reverted | 100-121 ms |

- **Exit status is real**: `ls fs99:` answers `0x800000000000000E`
  (EFI_NOT_FOUND), the next command `0x0`.
- **`Execute()` does not set `%lasterror%`** -- measured: a following command
  reads `0x0` after a failure. Typed at the prompt it is set (`0xE`). So the
  status has to come from `Execute`'s own `StatusCode` out-parameter, which
  **`axl_shell_execute()` passes as NULL**: the public API cannot report a
  command's status today. The spike called `Execute` directly
  (`--allow-uefi`); the real agent needs that gap closed in AXL, test-first.
- **X64's network path is quantized at 10 ms** (every ping is 0.2, 10 or
  20 ms); aa64 answers in 0.3 ms. Something on the KVM path waits for a
  10 ms tick -- worth finding on its own, since every AXL server on x64 pays
  it.

### 2.2 Where the REST agent's time went, and what removes it

**The 10 ms quantum is the guest's, and it is a pair of timers.** A capture
at the guest NIC (QEMU `filter-dump`) shows each request reach the guest and
the guest's ACK leave WITH the reply ~9.7-10 ms (sometimes 19.6 ms) later:
the packet sits unprocessed. UEFI has no NIC interrupts, so a halted CPU runs
nothing until the firmware's timer tick (10 ms by default), and MNP collects
received packets on its own 10 ms timer unless something calls `Poll()`.
e1000 behaves the same as virtio-net, so it is not the NIC driver.

Each lever alone does nothing visible; both together do (X64):

| agent setting | ping keep-alive / new conn. | idle CPU |
|---|--:|--:|
| default | 10 / 30 ms | 0.013 cores |
| `tcp4->Poll()` every 1 ms (a loop timer) | 10 / 10 ms | 0.015 |
| **+ firmware tick 1 ms** (`EFI_TIMER_ARCH_PROTOCOL.SetTimerPeriod`) | **1.0 / 1.1 ms** | 0.11 |

aa64 with both: 0.9 ms, 0.075 cores. **The fast tick costs ~0.1 core per
guest even idle**, so the shape is ADAPTIVE -- raise the tick and poll for a
short window after network activity, drop back to 10 ms when idle -- which is
Windows' `timeBeginPeriod`/`timeEndPeriod` pattern (raise the system timer
resolution only while someone needs it) applied per request, and Linux's
`busy_poll` for the polling half. The tick is firmware-global, so whoever
raises it must restore it on every exit path. Every AXL network server on x64
(sshd, webfs, SoftBMC) pays the 10 ms per round trip today, so this is an
AxlLoop/AxlNet feature, not a harness one.

**The rest of an exec is `EFI_SHELL_PROTOCOL.Execute()` itself**, timed in
the guest:

| per call, in the guest | X64 | AARCH64 |
|---|--:|--:|
| `Execute("echo b")` | 25 ms | 61 ms |
| `Execute("<app> -h")` | 57 ms | 102 ms |
| **`axl_image_run(<app>, "-h")`** | **4.0 ms** | **18 ms** |

`Execute` costs ~25 ms before the command does anything -- consistent with
EDK2 starting a new Shell instance per call. Loading the image directly skips
it. The nesting itself turned out to be optional, and the agent's answer is
a Shell started `-nonest` (§2.3), where `Execute` costs 0.14 ms (X64) before
the command runs.

**With both fixes the REST agent is as fast as the mailbox or faster** --
~5 ms per app on X64 (1 ms network + 4 ms image), ~19 ms on aa64 -- and it is
the one transport that also reaches **real hardware**: a lab machine with a
NIC and no virtio-fs runs the same agent and is driven the same way. It is
the pool's transport (§3); the mailbox stays as a fallback for a guest with
no NIC.

### 2.3 One verb: what the Shell does to a line, measured (2026-11-03)

The agent takes ONE command line and loads the image directly only when that
cannot change the result. What "cannot change" means was measured, not read:
`argv-dump.efi` records the argv it receives, and the same line ran through
`Execute()` and as `axl_image_run(path, <whole line>)`. **X64 and AARCH64
gave identical results.**

`LoadOptions` must be the WHOLE line, image path first: AXL's argv parser
(`_tokenize_load_options`, `src/posix/axl-app.c`) takes argv[0] from it, as
the Shell's own `LoadOptions` copy does. Given only the arguments, the app
lost its first one to argv[0].

| line (after the image path) | Shell argv | direct-load argv |
|---|---|---|
| `a b c`, `a   b`, `-f=x "y z"`, `"a b" c`, `café` | same | same |
| `FS1:\*.txt` | literal (EDK2 does not expand for apps) | literal |
| `%path%` | expanded | literal `%path%` |
| `a # comment` | `[a]` | `[a] [#] [comment]` |
| `a^ b` | `[a b]` | `[a^] [b]` |
| `^^x ^#y` | `[^x]` | `[^^x] [^#y]` |
| `a^"b "c^"d"` | `[ab cd]` (the spec says `a"b c"d`) | `[a^b c^d]` |
| `"" x` | **not run**, `EFI_OUT_OF_RESOURCES` (the spec says valid) | `[] [x]` |
| `dump.efi a` (no volume) | found on `%path%` | not found |

Through `Execute()` all of these worked: `>a`, `2>a`, `>v VAR`, `> NUL`,
`<a`, and pipes with both `|` and `|a`. Exit status is the app's return value
as an EFI_STATUS (`0x4` for a return of 4).

**`Execute()`'s status 0 does not mean the command ran.** It returned
`0x0`, and nothing ran, for: an unknown command, a missing `.efi`, a path
with the extension omitted, two stdout redirects, two stderr redirects. A
real failure inside a command that did run (`ls fs99:`) reports correctly.
And the agent's own capture -- appending `>a <file>` -- on a line that
already redirects stdout returned the PREVIOUS command's output: a stale
file read back as this command's result.

**Why, from the source: an EDK2 bug, and the way around it.** The spec says
Execute places "the status code returned by the specified command" in
`StatusCode`. In EDK2's default NESTED mode, `EfiShellExecute` runs
`Shell.efi -exit <cmd>` (`ShellProtocol.c`) and `StatusCode` is that Shell's
exit code: `DoStartupScript` starts `CalleeStatus = EFI_SUCCESS`, and every
cannot-run path in `RunShellCommand`/`RunCommandOrFile` calls
`SetLastError(SHELL_NOT_FOUND)` without writing the command status
(`Shell.c`). Present in EDK2 master (2025-11) and in the copy QEMU 10.0
bundles. The `nonesting` variable selects `InternalShellExecute` instead --
the command runs in the CURRENT Shell and the status pointer is written only
when it ran -- but `nonesting` can be changed only in a Shell started with
`-nonest`; elsewhere it is read-only `False`.

The same cases in a Shell started `-delay 0 -nonest` (a spike launcher via
`AXL_SHELL_LAUNCHER_BIN`), with `StatusCode` pre-set to a sentinel; X64 and
AARCH64 agree:

| | nested (default) | **`-nonest`** |
|---|---|---|
| unknown command, missing `.efi`, extension omitted | `0x0`, did not run | **sentinel untouched** |
| two stdout redirects | `0x0`, did not run | **Execute returns `EFI_NOT_FOUND`** |
| `"" x` | not run, `EFI_OUT_OF_RESOURCES` | **runs**, argv correct |
| an app returning 3 | `0x3` | `0x3` |
| redirection; `set` then `%var%` | works | works |
| a pipe | `0x0` | output correct, **sentinel untouched** |
| `Execute("echo b")`, X64 / aa64 | 29 / 41 ms | **0.14 / 0.36 ms** |
| `Execute("<app> -h")`, X64 / aa64 | 55 / 100 ms | **21 / 40 ms** |
| `axl_image_run`, X64 / aa64 | 4.4 / 18 ms | 4.4 / 18 ms |

**The rule, revised: no direct-load path.** In a `-nonest` Shell, Execute
reports a command that never ran (sentinel untouched, or an error return),
and costs ~15-22 ms more than a direct load per app -- noise against tests
that run for seconds. So the agent's one verb ALWAYS goes through the Shell:
exact Shell semantics, nothing imitated, no tokenizer to keep in step. Direct
loading comes back only if a measurement asks for it. What remains:

- **Pool guests boot their Shell `-delay 0 -nonest`** (the launcher's
  `LoadOptions`).
- **A pipe** leaves the status unwritten though it ran: the agent sees the
  `|` and reports status UNKNOWN, not not-run.
- **A nested Shell** (an `sshd` user's machine, not started `-nonest`) still
  has the bug: there the agent resolves the first word before running
  (alias, builtin, or a file on cwd/`%path%` -- `axl_path_search()`), and
  answers `not-found` itself.
- **`exit` and `reset`** run in the agent's own Shell under `-nonest`
  (untested): the agent refuses them; the pool resets over QMP.
  *Revised in §5.1: only `exit` is refused (it reports 0 while ending the
  Shell); `reset` drops the connection, a failure the client sees, and on a
  lab machine it is a command worth having.*
- **Capture** never appends a second redirect, and the capture file is
  deleted before each command so a stale one cannot be returned.

**What AXL needs, test-first:** a Shell execute that returns the command's
status, with the sentinel built in (`axl_shell_execute()` passes NULL
today); and, for nested Shells only, a first-word query -- alias
(`GetAlias`) and builtin (`GetHelpText`) are in `EFI_SHELL_PROTOCOL` but AXL
leaves both `void *`. The exported LoadOptions tokenizer is no longer needed
for the agent (its comment's promise of backslash-escaped quotes, which the
code lacks, is still worth fixing on its own).

### 2.4 The sentinel is not enough -- `%lasterror%` is (measured 2026-11-03)

**Correction to §2.3.** A contract-first review read EDK2's Shell against the
sentinel rule above and found it wrong in the unsafe direction, and a probe
(every case below, X64 and AARCH64, identical) confirmed it: in a `-nonest`
Shell a `.nsh` script, a drive change, an empty or comment line and a pipe ALL
leave `StatusCode` unwritten exactly as an unknown command does; and a
wrong-architecture `.efi` WRITES a status (its load error, `0x3`) while never
running. What does separate them is the Shell's own `%lasterror%`, readable
after the line because under `-nonest` the line ran in this Shell:

| line (`-nonest`; `%lasterror%` reset to 0 first) | Execute | StatusCode | `%lasterror%` | verdict |
|---|---|---|---|---|
| `echo`, an app returning 3 / -1 | 0 | 0 / 0x3 / 0xFF | same | ran |
| unknown command, missing `.efi`, a directory | 0 | unwritten | 0xE | not run |
| wrong-architecture `.efi` | 0x8000..03 | 0x3 | 0x3 | not run (load error, no error bit) |
| `goto` outside a script, `ls` on a missing path | 0x8000..15 / 0 | 0x8000..0E | 0xE | ran (error bit) |
| script `exit /b 3`; script ending in `echo` | 0 | unwritten | 0x3 / 0x0 | ran |
| script whose last command is not found | 0 | unwritten | 0xE | reads not run (safe direction) |
| pipe ending in an app returning 5 | 0 | unwritten | 0x5 | ran |
| drive change, empty line, `# x` | 0 | unwritten | 0x0 | ran |
| double redirect, unmapped drive | 0x8000..0E | unwritten | unchanged | not run |

- `%lasterror%` cannot be written (`SetEnv` refuses it); **only `cd .`** resets
  it to 0 -- `set -v` and `echo` leave it alone. So the reset is a quiet
  `cd .`, skipped when there is no current directory or `cd` is aliased.
- **Nested mode reports nothing usable**: 0 for everything that did not run,
  and 0x3 for the wrong-arch image (an app returning 3 looks the same). Every
  nested result is UNKNOWN. And an **empty line** in nested mode starts a
  whole interactive Shell, which ran `startup.nsh` again -- the probe recursed.
- `exit N` reports 0 and ends the hosting Shell (the review, from source), so
  the library refuses it. A Ctrl-C'd script reports the last completed status
  -- and the Shell consumes the break before `Execute` returns, so the call
  cannot see it (measured with a scripted break: the script stopped, the line
  read RAN 0). The contract states it rather than pretend to detect it.
- An application's status reaches the caller truncated to 32 bits
  (`Shell.c:2651` casts it to `SHELL_STATUS`): `EFI_ABORTED` from an app
  reads 0x15, without the error bit.

The contract this produced is `axl_shell_execute_status()` (phase 1). The
"pipe: agent reports UNKNOWN" and "nested: resolve the first word" bullets in
§2.3 are superseded: pipes are answered by `%lasterror%`, and a nested Shell
is answered UNKNOWN rather than guessed at.

Two harness rules the spike found the hard way:

1. **Drain the serial socket continuously.** A guest whose console output
   nobody reads blocks on its next write once the socket buffer fills; the
   aa64 mailbox agent froze exactly that way.
2. **Never reuse a request name.** The first mailbox protocol deleted the
   request after running it; the host had already queued the next one under
   the same name, and the agent deleted it unrun.

### 2.5 Fast round trips, opt-in -- and the USB keyboard hazard (2026-11-03)

`axl_net_set_latency` (WINDOW / ALWAYS, over `axl_timer_resolution_*`)
takes an event-loop server's p90 round trip from ~10-20 ms to ~1 ms on both
arches (test-net-latency-qemu.sh). It shipped **off by default** (Mike,
2026-11-03): a 1 ms firmware tick made OVMF's USB keyboard path drop
keystrokes under QEMU -- ten key presses 0.5 s apart: 10/10 at 10 ms and
5 ms, 8/10 at 2 ms, 1/10 at 1 ms, hub or root port alike. The test agent
opts in (ALWAYS, 1 ms): pool guests are driven over HTTP, and nobody types
on them. sshd opts in only once measured with a USB keyboard.

**Open, a separate investigation:** why a fast tick loses USB keys -- OVMF's
UsbKbDxe / XhciDxe async-interrupt polling, QEMU's usb-kbd or qemu-xhci, or
both -- and whether real hardware is affected. A fixable firmware or QEMU
defect would be worth reporting upstream; until then the hazard is stated
in axl-time.h and axl-net.h.

## 3. Control channels

| channel | role | why |
|---|---|---|
| **QMP socket** | lifecycle: snapshot save/load, `system_reset`, `quit`, hot-plug | Out of band -- works on a hung guest. "Reset" and "exit" belong here; a Shell `reset` over serial is the fallback, never the mechanism |
| **HTTP to a resident agent** | commands, in QEMU and on real hardware | ~5 ms (X64) / ~19 ms (aa64) per app with the adaptive tick and direct image runs (§2.2); real exit status; nothing typed |
| **mailbox on virtio-fs, same agent** | commands, for a guest with no NIC | 19-23 ms per command |
| **serial socket** | boot, the agent's launch, and the console transcript tests assert on | Up from firmware start and depends on none of our code |
| **guest `sshd`** | not the harness transport; the agent gives it `exec` (§5) | It is the code under test, it needs the network, and typing over it is the slowest channel measured |

## 4. Prior art

| implementation | what it offers | verdict | the detail taken |
|---|---|---|---|
| labgrid (Pengutronix, pytest) | a booted target kept across tests; strategies move it between states | right shape | the strategy idea: a test declares the state it needs, and only a different state costs a transition |
| QEMU snapshots, as kAFL/Nyx use them for UEFI fuzzing | per-case restore in milliseconds | adopted | snapshot at the prompt, restore per test |
| Avocado-VT (libvirt/QEMU test framework) | VM reuse unless a test sets `kill_vm`/`restart_vm` | assumes libvirt and a Linux guest; not used | the per-test declaration of how fresh a guest must be |
| syzkaller VM pool | many programs per VM | -- | reuse until damaged; a crashed or hung guest is killed and replaced, never restored |
| CTest resource groups | a spec file of N slots per class; CTest assigns a slot through the environment | adopted as-is | it is the pool's allocator |
| `qemu.qmp` | QEMU's own QMP client | adopted (spike; the harness may use socat) | -- |

## 5. The agent

A small application, `AxlAgent.efi`, staged beside `sshd.efi` as
`SftpServer.efi` is. It runs as a Shell command and stays in the foreground on
its own loop, at TPL_APPLICATION.

**Why it is an application and not a helper `sshd` calls.** `SftpServer.efi`
is passive: `sshd` loads it at start and calls into it from event callbacks.
Running a command means `LoadImage`/`StartImage`, which UEFI 2.11 Table 7.3
forbids at TPL_CALLBACK, and `EFI_SHELL_PROTOCOL.Execute()` expects the
Shell's foreground (the hazard is written up in `tools/sshd-sftp-relay.c`).
So the agent owns the TPL_APPLICATION context, and others queue work to it.

**Prerequisite in AXL:** a way to run a Shell command and get ITS status
(§2.1) -- `axl_shell_execute()` cannot, and `%lasterror%` is not set by
`Execute()`.

One core -- ONE verb, `exec`: a command line, always run by a `-nonest`
Shell (§2.3) --
behind three transports:

- **HTTP** (the pool, and real hardware): `POST /exec`,
  with the adaptive latency mode of §2.2.
- **Mailbox** (a guest with no NIC): requests are uniquely named files in a
  directory on the virtio-fs share; the agent writes the status and output
  back beside each, and deletes nothing it did not create.
- **`sshd` exec** (users): the agent publishes a protocol interface (the mirror
  image of `SftpIface`); `sshd`'s `chan_on_exec`, refused today, queues the
  request from its callback; output streams to the channel through
  `AxlConsoleTap`; the status goes back as `exit-status`, which
  `axl-ssh-chan-codec.c` already builds. With no agent running, `exec` is
  refused as now, with a message saying why.

The pool runs a PINNED agent build, so a regression in the agent under test
fails the agent's own tests instead of blinding the suite.

### 5.1 What shipped: `tools/agent.c` (phase 1b, 2026-11-03)

The agent is a shipped tool, `agent.efi` (`TOOL_EXTRA_APPS`, not in the
busybox: it carries an HTTP server). Its protocol is its header comment, and
`test/integration/lib/axl_agent.py` is the client the pool reuses. Three
routes: `GET /ping`, `GET /info` (protocol version, arch, `nonest` and the
firmware tick, read when asked), `POST /quit` (answer, then return to the
Shell -- which is also how the test sees the agent's OWN leak verdict) and
`POST /exec`, which answers JSON -- `outcome`, `status`, `execute`,
`reset_ok`, `ms`, and stdout and stderr SEPARATELY, each with a state
(`captured`, `truncated` at 1 MiB on a character boundary, `redirected` by
the line itself, `unreadable`, or `lost`: the line ran but the Shell never
created the capture file, so it never saw the redirect).

**Capture is by redirect, placed where the Shell will honour it.** An
independent contract review found the naive "append ` > file`" swallowed by
three spellings, and they were measured: EDK2 strips a `#` comment EVEN
INSIDE QUOTES (only `^#` is literal), so the captures go before the first
unescaped `#`; an unclosed quote or a final `^` would swallow them, so those
lines are refused (400); and in a pipe they bind to the LAST segment, so the
redirect scan covers only that segment (an earlier segment's stderr reaches
the console -- stated). The review's alternative, tapping ConOut, would merge
stdout and stderr and lose the line's own redirects' semantics; not taken.
Capture files are named per agent start (a random tag) and per line, so pool
guests sharing one virtio-fs directory can never read each other's.

Measured on the way (both arches identical):

- With no current directory -- an agent started as `fs0:\agent.efi` from a
  script -- the library's `%lasterror%` reset is skipped and every outcome
  is `unknown`. The agent changes to its capture directory at start when
  there is none. The capture directory is made ABSOLUTE at start (lines may
  `cd` anywhere) and proved writable with a probe file; it refuses to start
  otherwise.
- `>>a` onto a UCS-2 file is refused by the Shell (`not-run`); `type` ends
  with an extra line end; after `app | echo x` the status is the app's
  (`echo` leaves `%lasterror%` alone).
- A DEBUG AXL app's leak verdict is on stderr; the cases remove it as one
  exact whole line.

`axl_shell_execute_status` now refuses `exit` with `AXL_DENIED` instead of
`AXL_ERR`, so the agent answers 403 for a refused line and 500 only when
the Shell could not be asked.

**`exit` poisons the agent's Shell -- measured, then detected.** The
library refuses `exit` as a line's first word; the review's reason (it ends
the hosting Shell while reporting 0) came from EDK2's source, so it was run
(both arches identical; `exit` placed past the refusal):

| `exit` placed | that line | a later script | after the agent returns |
|---|---|---|---|
| none (control) | -- | runs whole, `exit /b 3` reads 0x3 | the Shell continues its script |
| inside a script the line runs | RAN 0 | stops after its first line, RAN 0 | the Shell exits to the setup menu |
| a later pipe segment (`echo x \| exit`) | RAN 0 | the same | the same |
| the first word via `%var%` | RAN 0 | the same | the same |

The agent keeps answering, so the harm is silent: every later script reports
a success it never finished. Nothing undoes the Shell's exit flag, so the
agent DETECTS it: after each line in a `-nonest` Shell it runs a canary
script that sets a variable to 1, then 2, read back through the
environment API -- 1 means only the first line ran, which sets
`shell_exiting` on that answer and makes every later `/exec` a 409 until a
restore or reset. The first canary read the canary's own `%lasterror%`, and
review found it would POISON a healthy agent forever whenever the status
reset could not run (an alias named `cd`, no current directory) or a line
deleted the canary script; the variable cannot be faked that way, a
deleted script is rewritten and retried, and an answer the canary cannot
give is `inconclusive` -- reported, never sticky (all measured; an alias
named `set` does not reach the canary's built-in). A line that turns
`nonesting` off is put back to `-nonest` for the canary, which must run
in the agent's own Shell.
`exit /b` ends only its script and poisons nothing. Also measured:
`reset` drops the connection (and under `run-qemu.sh --background`, which
passes `-no-reboot`, ends QEMU); the client used to RETRY the POST then --
`/exec` is never retried now, and a connection idle past the server's
keep-alive is reopened before a request instead. And a `%var%` holding `#`
is not a comment (comments are stripped before expansion), so the one
measured source of `lost` is a nesting Shell refusing a line.

Stated, not solved, in the contract: Shell state persists between lines
(`set nonesting False` makes later outcomes `unknown`); an `exit` past the
refusal still poisons the agent's Shell -- detected above, not prevented; a
line that waits for a key
hangs it -- the client owns the timeout and replaces the guest; POST is not
idempotent. Script-body delivery (§6, delivery 2) and a request-id cache are
v2.

## 6. The pool

- **Classes** keyed by the QEMU command line a test needs (arch, NIC model,
  extra devices, debug OVMF). Each class has a snapshot taken at the agent's
  ready line.
- **Per-test reset level**, declared in `test-meta`: `reuse` (nothing left
  behind), `snapshot` (the default), `reset` (`system_reset`), `fresh`
  (today's behaviour: firmware lifecycle, crash handling, `reset`/`exit`
  tests, debug-OVMF assertions). A guest that crashed, hung or missed its
  status deadline is replaced, never restored.
- **Allocation** is CTest's: a resource spec lists the slots per class, a test
  asks for one slot of its class, and CTest's environment names the slot.
  Tests that declare nothing keep booting as they do now.
- **Files** come over virtio-fs: the build tree is mounted, so a rebuilt image
  is visible on the next command with no staging.
- **One `<TestName>.nsh` per test, and NO per-test disk image** (decided with
  Mike, 2026-11-03). Today every test builds its own FAT image and writes a
  `startup.nsh` into it; a pooled test instead keeps a checked-in
  `<TestName>.nsh` beside its `.sh`, and runs it with ONE `exec`
  (`fs0:\tests\test-foo.nsh 8123 ...`). The values tests bake into their
  `startup.nsh` at run time today -- host ports, file names -- become the
  Shell's positional script arguments (`%1`..`%9`), so the script is a fixed
  file. Delivery, in order:
  1. the virtio-fs share every pool guest mounts (scripts and images both;
     measured live while running and across a snapshot restore);
  2. the script in the `exec` request body, which the agent writes into the
     guest and runs -- the real-hardware path, where nothing is mounted;
  3. one disk image holding every script, only if virtio-fs proves unusable
     somewhere: a FAT image cannot be changed under a guest that mounts it, so
     it must be rebuilt before each run and is stale at the first rebuild.

### 6.1 What keeps a test off a pooled guest -- and pool size (2026-11-03)

**State never does.** A snapshot restore reverts RAM and device state, so
whatever a test loaded or left resident is gone; teardown need not even be
perfect. What a pooled guest cannot change is its MACHINE: QEMU's command
line is fixed at launch, and the firmware enumerates PCI, builds its SMBIOS
and ACPI tables and binds drivers once, at boot -- OVMF does not see a
device hot-added afterwards, so hotplug is no way round it. A test that
needs different hardware needs a guest booted with it, which is a CLASS,
not a fresh launch.

The census tail (116 X64 launches outside the standard shape) by reason:

| reason | launches | pooled? |
|---|--:|---|
| QEMU's default NIC (now the standard NIC, phase 0b) | 14 | yes, now |
| debug OVMF + screenshots (another firmware image) | 16 | its own class |
| VGA / VGA + USB keyboard | 13 | its own class |
| virtio-fs share | 4 | every pool guest has it |
| virtio-net instead of e1000 | ~11 | a class, or one guest with both |
| network topologies (2-3 NICs, link-down, hubs, MACs, no ROM) | ~25 | largely: one multi-NIC class, links switched per test (`set_link` works live); tests reworked |
| boot-time fixtures (SMBIOS, ACPI tables, SPD EEPROM, TPM, NVMe) | ~10 | a class per fixture digest |
| CPU model / count / accelerator (Nehalem, Haswell, 4 cores, TCG crash guests) | ~13 | a class each |
| the test's subject IS the boot (launcher, boot path, reset) | a handful | never |

**Classes outlive runs.** A class used once per run saves nothing if the
pool dies with the run -- it still boots once. A pool that stays resident
across runs (a small pool manager; the inner loop of development is the
whole suite run again and again) boots each class once per session, and
every later run only restores. Then even a one-test class pays from its
second run, and only the boot-subject tests launch fresh. That needs:
classes created lazily on first use and evicted least-recently-used under
memory pressure; each snapshot keyed by everything that defines the machine
(firmware image, QEMU binary, full argv, fixture digests, the agent build),
so a firmware rebuild re-boots the class instead of testing a stale one; the
UEFI variable store (pflash vars on qcow2) and swtpm state inside the
snapshot, or variables a test wrote leak into the next; and a per-test
directory on the share, which a snapshot does not revert. A crash test fits
too: restoring the pre-crash snapshot is exactly the reset it needs.

**Pool size is bounded by memory, not cores.** An idle guest costs ~0.01
core; CPU is spent only while a test runs in it, and THAT concurrency stays
bounded by cores (CTest's `-j` and resource groups). Measured idle at the
prompt: 106 MB RSS (X64), 268 MB (AARCH64) of their 512 MB; nine idle
standard guests, 1,378 MB, of which KSM merged ~388 MB (28%) within 150 s and
still climbing (KSM enabled on the dev host 2026-11-03). A guest that has
run tests grows toward 512 MB. On a host with ~21 GB free, 30-40 resident
guests is comfortable.

### 6.2 Target shape: a resident pool service (proposed 2026-11-03)

Mike's direction: the pool may become a project of its own, run as a
systemd service that spawns guests per configuration on demand and shuts a
guest down after a configurable idle time. The shape agreed so far:

- **A systemd USER service, socket-activated.** It starts on the first
  lease request, runs guests as the user (KVM through the `kvm` group,
  sockets under `$XDG_RUNTIME_DIR`), and with lingering keeps running
  without a login. Nothing is resident until a test asks.
- **Idle shutdown is cheap because restarts load the snapshot.** Each
  class's snapshot lives on disk (qcow2 internal snapshots), so a class
  shut down for idleness comes back with `-loadvm` -- a restore, not a
  firmware boot. The idle timeout can then be aggressive (start at ~10
  min). The `-loadvm` start time is the first thing to measure.
- **Three ways a guest goes:** idle timeout per class, LRU eviction under
  memory pressure, and a global cap. Snapshots are keyed as in §6.1, so a
  rebuilt firmware invalidates its class rather than serving it stale.
- **A small lease API** on a Unix socket: lease a guest of a class (get
  the agent's address, the QMP socket and a per-test share directory),
  release it (the snapshot is restored). CTest's resource groups bound the
  tests in flight; the service bounds the guests that exist. CI runs on the
  same host, so it would lease warm guests too.
- **Generic service, AXL specifics in axl-sdk.** The service knows QEMU,
  snapshots, leases and lifetimes; a class is a QEMU argv, a readiness
  probe and an optional agent. The in-guest agent, the test conversion and
  `axl_shell_execute_status` stay here -- which also makes it useful to
  softbmc or anything else that boots UEFI guests.
- **Built in-tree first** (e.g. `test/pool/`, Python asyncio over the
  installed `qemu.qmp`, Pylance-strict), extracted once the lease API
  stops moving.

Prior art to read first: syzkaller's VM pool (`vm/qemu`: a manager keeping
a pool of QEMU VMs, replacing dead ones, with a snapshot mode that resets a
VM between programs -- the closest match); libvirt's snapshot handling (too
heavy to adopt for throwaway guests); Firecracker/Lambda warm pools
(restore-not-boot validated, but microVMs have no UEFI).

Decided (Mike, 2026-11-03):

- **Language:** Python in axl-sdk; ported to Rust when it becomes a
  standalone service other projects test with (SoftBMC, axl-webfs, AGT).
- **Idle timeout:** 10 minutes by default, tuned from measurement.
- **CI shares the service** with local runs -- one developer, one host, one
  CI job at a time, so a second service would only hold a second copy of
  every warm guest. Isolation does not depend on separation: snapshots are
  keyed by firmware, QEMU, argv, fixtures and agent build, so a CI run of
  another commit gets its own classes, and every lease is restored and has
  its own share directory. CTest bounds tests in flight per run; the
  service's cap and LRU bound memory; first come, first served.
  **Except release certification** (`release-suite.sh`, `--ci-gate`): it
  asks for FRESH classes -- booted and snapshotted anew, warm snapshots
  ignored -- so a release is proven against real boots of exactly what
  ships, as the suite already certifies uncached. And CI must still run
  without the service (the hosted-runner fallback has none): the harness
  keeps a direct-launch path for when no service answers, which the
  boot-subject tests always take.

### 6.3 Phase 2 plan (2026-11-03)

In-tree, `test/pool/` (a Python package, Pylance-strict, asyncio over
`qemu.qmp`), built in this order, each step test-first with its own gate:

1. **Spike the two unmeasured mechanics** (§6.2 said to measure first):
   - **Variables inside the snapshot.** The spike's guests kept the OVMF
     vars and the FAT disk RAW, so `snapshot-save` captured neither; a test
     that writes a UEFI variable would leak it into the next lease. Put the
     vars pflash on qcow2 (`run-qemu.sh` grows an option) and prove a
     `SetVariable` is reverted by a restore.
   - **`-loadvm` into a NEW QEMU process** -- what an idle-stopped class
     comes back with. The in-process restore is measured (65-121 ms); a
     cold one is not, and the guest's virtio-fs session belongs to the old
     virtiofsd. Measure the start time and whether the share, the agent's
     listening socket and the NIC survive. If virtio-fs does not, the
     snapshot is taken with the share unmapped and the lease maps it.
   **Measured 2026-11-03 (`run-qemu.sh --qcow2`, both arches agree):**

   | | X64 | AARCH64 |
   |---|--:|--:|
   | boot to the agent answering (the class's one cold boot) | 10.4 s | 22.1 s |
   | first `exec` after an in-process `snapshot-load` | 109-119 ms | 157 ms |
   | **a NEW QEMU process to the agent answering** | **0.30 s** | **0.41 s** |
   | a UEFI variable set after the snapshot, after either restore | gone | gone |
   | the share through a NEW virtiofsd, after a cold restore | reads, and sees a host-side change | same |

   `--qcow2` puts the vars pflash AND the boot disk on qcow2: `-loadvm` (and
   `snapshot-load` in a new process) refuses while any writable disk cannot
   hold a snapshot ("Device 'ide0-hd0' is writable but does not support
   snapshots"), and a file a test writes on the boot disk is then reverted
   too. Three things the pool must do, each learned by failing:
   - **Not `-loadvm`.** It takes the VM state from the first
     snapshot-capable disk, not the vmstate image ("This is a disk-only
     snapshot"). The new process starts PAUSED (`-S`), restores with QMP
     `snapshot-load` naming `vmstate` and the devices (node names are
     regenerated per process: find them by file), then `cont`.
   - **A new virtiofsd per new process**, started before QEMU, at the same
     socket path. The guest's virtio-fs session survives it.
   - **Never assume `fs0:`.** On AARCH64 the virtio-fs share enumerates
     before the boot disk (FS0 is the share), so the agent is started from
     whichever volume holds it (`if exist fsN:\agent.efi`).
   An aside: "Image type X64 can't be loaded on AARCH64" on an AARCH64 boot
   with a NIC is the NIC option ROM's X64-only driver -- harmless and
   pre-existing.
2. **Guest + class** (`guest.py`, `klass.py`): launch through
   `run-qemu.sh --background` (one launcher, phase 0b), the standard
   devices, `-nonest`, the agent on a forwarded port, QMP and serial
   sockets with the serial DRAINED continuously (§2.4 rule 1); ready at
   `AXL-AGENT-READY`; snapshot `clean`; a class key = hash of firmware,
   QEMU binary, full argv, fixture digests and agent build.
   **Step 2 DONE 2026-11-03** (`test/pool/axlpool/klass.py`, `guest.py`;
   `test-pool-guest-qemu.sh`, both arches): boot 10.9 / 22.2 s, restore
   ~190 / ~230 ms (20 cycles, the share read each time), cold start
   0.31 / 0.44 s. The class key is a dry run of the EXACT launch a boot
   uses (`launch_args`, shared), plus what `run-qemu.sh` now reports beyond
   the argv (`AXL_DRYRUN_TMPDIR`, `AXL_DRYRUN_STAGED` -- a digest of every
   file staged on the boot disk, so a rebuilt launcher or Shell is a new
   class -- and `AXL_DRYRUN_INPUT`, the vars template), firmware bytes and
   the QEMU binary. A dry run no longer starts virtiofsd (it did, once per
   key). Agents bind 127.0.0.1 (`--hostfwd-bind`): an agent runs whatever
   it is sent. A failed or hung boot leaves nothing running, checked by a
   process census by PATH -- the first version's pid-based check could not
   see a leaked guest, and one leaked under a concurrent run.
   **Owed to steps 3-4, the service, by design:** process OWNERSHIP --
   `PR_SET_PDEATHSIG` is per THREAD, so the service spawns guests from one
   long-lived thread; a booted guest lives under `run-qemu.sh`'s `timeout`
   (24 h) and a crashed service leaves its guests, so the service sweeps
   orphans by state directory when it starts -- and cold-start ports through
   the same flock scheme `run-qemu.sh`'s allocator uses (today an unlocked
   free-port probe).
   **The classes file (decided with Mike, 2026-11-03).** Machines are
   NAMED classes in one file, `test/pool/classes.toml` (TOML: Python reads
   it with the standard library's `tomllib`, and the Rust service -- meant
   for other UEFI/QEMU projects' pools too -- with the `toml` crate); a test
   names its class in its `test-meta` header (`guest=<class>`). Prior art:
   labgrid's named targets (one definition, tests name one), Avocado-VT's
   inheritance (`extends`), CTest's slot counts (`slots`). Every machine is
   a named class, never options written into a test, so all machines stay
   visible and countable in one place; `axlpool/classes.py` refuses a typo
   (unknown key, parent, arch, value; a cycle) with an exact message rather
   than letting it become a new machine. **One-off machines** -- the census
   tail: a CPU model, a fixture digest, a NIC topology -- are two-line
   classes (`extends = "standard"` plus their args) with `keep = "lease"`:
   booted when leased, stopped when the last lease ends, their snapshot
   left on disk so the next run cold-starts in ~0.4 s instead of booting --
   resident memory only while in use. Tests whose SUBJECT is the boot say
   `guest=none` and keep launching QEMU themselves. If one-offs pile up,
   the file shows it, and that is the moment to merge some into a shared
   class (a multi-NIC class with links switched per test, §6.1).
3. **Pool + lease** (`pool.py`): lease returns the agent address, the QMP
   socket and a fresh per-lease share directory; release restores `clean`
   (65-121 ms); a guest that crashed, hung or missed a deadline is
   replaced, never restored; idle timeout (10 min) and LRU under a memory
   cap.
   **Step 3, phase A DONE 2026-11-03** (`test/pool/axlpool/pool.py`;
   `test-pool-lease-qemu.sh`, 31 cases on X64 with the class settings, 27
   on AARCH64). The lease contract is pool.py's docstring, reviewed before
   it was built (twelve findings: the serial transcript, pool-caused
   losses read as MISS, release causes, a plain-data lease with a token,
   `booted_after` once per run, background reset, one deadline, FIFO,
   a cached boot failure, the orphan sweep in the contract) and again at
   the first green (fourteen: waiters behind the head never woken -- boots
   ran one at a time; slot leaks on unexpected exceptions; a timed-out
   cold start orphaning a QEMU; close() racing in-flight boots; QMP with
   no deadline -- a stuck QEMU hung the pool, now a session over
   qemu.qmp's async client with a deadline on connect and every command;
   revalidate() bringing retired guests back; concurrent releases acting
   twice). Measured on the way: a dead cold-started QEMU is a ZOMBIE until
   reaped, so liveness reads /proc/<pid>/stat; virtiofsd exits BECAUSE its
   QEMU did, so only a dead virtiofsd under a live QEMU is a pool-side
   loss; and `run-qemu.sh --background` passes -no-reboot, so a reset
   ended QEMU -- pool guests launch with `-action reboot=reset`.
   Its tests: `test-pool-lease-qemu.sh` (the contract, both arches) and
   `test-pool-classes-qemu.sh` (the class settings and revalidate, X64:
   arch-independent, and kept apart so neither nears CI's limit).
   **Phase B DONE 2026-11-04** (`test-pool-lifetimes-qemu.sh` and
   `test-pool-lifetimes-mem-qemu.sh`, X64): a
   reaper thread stops guests idle past idle_timeout_s (STOPPING, then
   STOPPED with their state; the next lease cold-starts) and takes back
   leases held past max_hold_s (the guest killed -- the holder may be
   mid-command -- and the release says REPLACED, expired-hold); under
   mem_cap_mb every guest counts at least its CLASS's guest RAM (the last
   `--mem` in its args, else run-qemu.sh's 512M; a class whose `--mem`
   run-qemu.sh would refuse does not load -- before 2026-11-07 every guest
   counted a flat 512, so a 768M `unit` guest was undercounted) and the
   least-recently-used IDLE guests are stopped first (leased ones never);
   a Pool() ends any QEMU or virtiofsd naming its root (a crashed pool's
   guests) and reloads the guests an earlier pool left (`slot.json` beside
   each state), so a later pool cold-starts them instead of booting.
4. **The service** (`service.py`): the lease API on a Unix socket under
   `$XDG_RUNTIME_DIR`, JSON lines; a systemd user unit, socket-activated,
   shipped beside it; a direct-launch fallback when nothing answers.
5. **First conversions**: the plain single-boot X64 tests become
   `<TestName>.nsh` run with one `exec` through a lease; wall clock
   measured against §1.
6. **CTest resource groups**, folded into CMake slice 5a (CTest replaces
   the runners).

The lease API is the contract the Rust service will keep, so it gets a
contract-first review before step 4 is built on it.

### 6.3.1 Hotplug, superset classes, and typing on a pooled guest (measured 2026-11-04)

Mike asked whether QEMU hotplug, or one "least common multiple" machine,
could keep more tests on fewer classes. Hotplug, measured on both arches
(two runs each, agreeing; a `--bridges` guest with spare root ports):

| hot-added after boot (QMP device_add) | X64 | AARCH64 |
|---|---|---|
| USB keyboard (on xHCI) | bound at once (ConIn splitter gains it) | the same |
| USB stick (usb-storage) | bound; a new fsN after `map -r`; read/write | the same |
| NIC in a spare PCIe root port | `pci` lists it, NO handle; usable only after `system_reset` (7 s) | not even listed; usable after a reset (19 s); `device_del` never completes |

`connect -r` changes none of it: OVMF enumerates PCI once, at boot; its USB
bus driver watches hub ports at runtime. So **USB devices are per-lease** --
a test that needs a keyboard or a stick gets it hot-plugged for its lease --
and **PCI devices are machine classes**: a NIC, NVMe or VGA must be present
at boot. That is where Mike's superset idea fits: a FEW superset classes,
each booted once with everything its tests need -- `standard`; `standard` +
every NIC model, links switched per test with QMP `set_link` (absorbs most
of the ~25 network-topology launches, §6.1); `standard` + VGA -- with
firmware, CPU model and boot fixtures as one-off classes (they cannot be
merged: one machine has one of each). The tests a superset touches are the
ones that enumerate exactly (lspci/lsusb output, BDFs, NIC counts, and
AXL_NET_NIC_AUTO's first NIC), so the grouping is chosen from the per-test
device census when tests convert (step 5), not guessed.

Found on the way and fixed: removing a hot-plugged stick renumbers fsN on
AARCH64, and the agent's capture paths held a volume NAME -- every later
`/exec` failed while `/ping` answered. The agent now finds its directory
again by a marker file (and restores a current directory) before each line
(commit 789cfcc9).

**Typing needs the firmware tick.** Ten `sendkey a` into a USB keyboard,
read by a program the agent ran: 0 of 10 arrived with the agent at its
default `always` latency (1 ms tick), 10 of 10 at `--latency firmware`
(twice each) -- the OVMF keyboard hazard of §2.5, not hotplug. A class whose
tests type says `agent_latency = "firmware"` (classes.toml); it is part of
the class key (the start script is staged on the boot disk).

### 6.4 Test standards -- for the Rust service, and what the minimal version takes (2026-11-03)

Mike's direction: the Rust service, meant for other UEFI/QEMU projects'
pools too, adheres to -- or improves on -- the standards for test results
and logging rather than inventing its own. Compared (sources checked
2026-11-03):

| standard | what it offers | verdict for the Rust service | the detail taken |
|---|---|---|---|
| JUnit XML | the de-facto results file every CI renders; no formal schema; CTest writes it (`--output-junit`, CMake 3.21+), so does cargo-nextest | EMIT it -- the lingua franca, too thin to be the internal model | `<failure>` (the test said no) vs `<error>` (it could not run) |
| TAP 14 / KTAP | a line protocol (`ok 1 - name`), indented subtests, YAML diagnostics; TAP 14 is a stalled draft, KTAP the kernel's stable dialect | ACCEPT it as the in-guest stream from a test binary to the runner | subtests, and a structured got/want block on failure |
| KCIDB (KernelCI) | a JSON schema: build, test and environment as separate objects; status `MISS` = the harness failed, nothing is known about the code; schema-validated submissions | MODEL the result store and status vocabulary on it | `MISS` vs `FAIL` -- what a pool needs most |
| LAVA (Linaro) | pools of real boards: per-device dictionaries extending device-type templates; scheduled health checks that take a failing device out of service | COPY the health-check idea -- the closest prior art for real hardware | a class's golden health probe, and quarantining a guest or board that fails it |
| cargo-nextest | a Rust runner: test groups with mutual exclusion, setup scripts that export env to tests, leaky-test detection, retries, JUnit | MATCH its runner behaviour | setup scripts ~ a lease handing the test its agent; leak detection ~ the census by path (§6.3 step 2) |
| OpenTelemetry logs and traces | a stable, vendor-neutral data model; Rust's `tracing` exports to it | USE its field names and its trace model | one trace per lease: the guest's serial log, agent calls and QMP operations under one id |
| systemd journal | where a user service's stdout already goes, with severity and structured fields | LOG there (the service is a user unit) | severity prefixes, one JSON object per line |

**Taken now, in the minimal (Python) version:**

1. **A harness-failure status, `MISS`, apart from `FAIL`.** A pool adds
   failure modes the per-test boot never had -- a guest that dies, a lease
   that times out, a restore that fails -- and they must never read as a
   test failure, or the pool makes the suite look flaky. CTest knows only
   pass / fail / not-run, so the lease API reports it as a distinct
   outcome, the runner turns it into a reserved exit code, and the log
   names the cause. Part of the lease API's contract (step 3).
2. **Structured logs from the pool service** (step 4): one JSON object per
   line on stdout, into the journal, with OpenTelemetry field names --
   `timestamp`, `severity_text`, `body`, and attributes such as
   `axl.class`, `axl.lease`, `axl.guest`, `duration_ms` -- so the Rust port
   emits the same records through `tracing` with nothing renamed.

**Left for the Rust service:** TAP output from the test scripts (no
consumer today; converting the bash suite's `PASS:`/`FAIL:` lines is churn
without a reader); JUnit (CTest provides it once CMake slice 5a lands); a
result database, LAVA-style quarantine, and real OTLP export.

### 6.5 What was built (2026-11-04)

Steps 3-5 of §6.3, as committed. The contracts are the module docstrings
in `test/pool/axlpool/`; this is the map.

- **The service** (`service.py`, `python3 -m axlpool serve`): JSON lines
  over a Unix socket, protocol 2 (`hello` reports the pinned agent of each
  arch). One service per socket and one pool per state directory (flocks); a
  refused start exits 3; under systemd socket activation a refused start
  first answers every queued client `unavailable`. Stop is ordered and
  bounded: connections end (pending leases answered `shutting-down`, held
  ones taken back), then `close()` (one deadline for boots in flight, guests
  stopped and stragglers killed in parallel).
- **The agent is pinned per arch**: the service's tree's build of each
  (`tree_agent`, through that tree's `build-prefix.sh`), or `--agent
  [ARCH=]PATH`; a lease for an arch with none misses `no-agent`.
  `"agent": "tree"` leases the asking tree's own build.
- **Every guest is the pool's child.** A boot through run-qemu.sh ends by
  handing its guest over to a cold start (~0.4 s), so it dies with the pool;
  only one caught mid-boot can outlive a crash, and the next pool's orphan
  sweep ends it. A failed hand-over keeps the slot STOPPED (its snapshot is
  good) and the retry cold-starts it.
- **The client** (`client.py`) and **`axlpool run --class C --arch A --
  test.sh`**: one connection per test; the test gets `Lease.env()` (never
  the token); exit = the test's, 77 for any pool failure (marked
  `AXLPOOL-MISS:` on stderr), 127 when the test cannot start, 128+N for a
  signal while waiting. Signals go to the test's process group.
- **`axlpool exec`** (one Shell line through the agent; `--console` for
  uncaptured output), **`axlpool status`**, **`axlpool qmp`** (§6.6),
  **`axlpool units`** (systemd user units: `KillMode=mixed`,
  `TimeoutStopSec` derived from the stop bound, `RestartPreventExitStatus=2
  3`; INSTALLED 2026-11-06 against the port branch's worktree, RE-PINNED to
  the main checkout 2026-11-09 when the pool merged, §6.7 -- generated by
  `python3 -m axlpool units --tree ~/projects/aximcode/axl-sdk --out DIR`.
  The service finds each arch's agent in its tree's `build/<arch>-debug`,
  so after `main` moves the main checkout needs `scripts/axl-build.sh
  ARCH=<x64|aa64> agent` (and `install.sh --arch all --cpp` for the staged
  SDK the suite checks) before its runs are current) and
  **`axlpool resource-spec`** (a
  CTest resource spec, `guest_<class>_<arch>`, CTest-verified).
- **Pooled tests** (step 5): a test declaring `guest=<class>` keeps its
  Shell commands in a checked-in `<TestName>.nsh` (`test_use_nsh`); the same
  test runs booted (a generated startup.nsh) or, under
  `run-integration.sh --pool`, on a leased guest: files staged into the
  lease's share, the script run through the agent with its output on the
  console, and TEST_LOG the guest's serial log from the lease's offset,
  streamed through `ts` exactly as a booted log is (complete up to a crash).
  What a pooled guest cannot give (its own QEMU arguments, a read-only or
  second disk, typed input, other networks) is refused by name. A pool miss
  reruns the test booted (`<name>.pool-miss.log` kept); pooled and booted
  runs are cached apart. `check-test-meta` holds `guest=` to a class, a
  `.nsh` and a `test_use_nsh` call. Eight tests converted; each app that
  hard-coded `fs0:\` now names its files from the Shell's current directory.
- **Measured.** A warm pooled test: 3 s against 6 s (X64) / 10-11 s
  (AARCH64) booted; lease + release overhead 0.12 s. Seven converted tests:
  test work -40% (X64), -60% (AARCH64). The remaining wall-clock limit is
  slots (4 per class for those seven tests).
- **Built once.** The runner records what it pre-built
  (`AXL_PREBUILT_MANIFEST`) and `scripts/axl-build.sh` skips a test's
  identical build: same arch, command-line variables and targets, and the
  same fingerprint (the environment variables the Makefile and cmake/ read,
  derived not listed; MAKEFLAGS; the backend; the tree; the call's own
  prefix's build state). Goals that leave no artifact (`NON_ARTIFACT_GOALS`:
  reports and lint gates; `print-*`) never count. Uncached full suite, no
  pool: wall 1101 -> 978 s, test work 10111 -> 9096 s.

### 6.6 QMP: control and events (2026-11-04)

Every guest is driven over **QMP on Unix sockets** -- filesystem permissions
instead of ports, nothing on the network; the 107-byte socket path limit is
checked at cold start and by run-qemu.sh for its own `--qmp-socket` -- through `qemu.qmp`, QEMU's own client, with a deadline on every
connect and command (`guest.Qmp`).

- **Two monitors per guest.** QMP serves one client per monitor, so
  `qmp.sock` is the one a test (or a person) may use, and `events.sock` is
  the pool's, held open by a listener thread per guest (`_EventLog`).
- **Events.** The guest's own RESET, SHUTDOWN, POWERDOWN and
  GUEST_PANICKED are recorded since its clean state (the record restarts on
  every restore and on the pool's own `system_reset`; the STOP/RESUME of
  the pool's snapshot loads are not the guest's). The health check reads
  them first: a guest that reset itself under a lease (or that a test reset
  over QMP, which the lease contract forbids) is released as REPLACED with
  cause `guest-reset` -- exact, and without waiting out an agent probe --
  and an idle guest that did is never handed out. Not recorded: a reset in
  the moments of a cold start before the listener connects (the probes
  cover it). A listener that cannot connect is logged ("QMP events
  unavailable") and the health check falls back to probes alone. Owed: a
  `guest-panicked` cause (GUEST_PANICKED is recorded; QEMU's default
  panic action exits, so it reads as qemu-exited today).
- **`axlpool qmp [--timeout S] SOCK COMMAND [ARGS-JSON]`** (exit 0 / 1 QEMU
  refused it / 3 no QEMU) is the shell's QMP, via `lib/qmp-lib.sh`'s
  `axl_qmp`. The hand-rolled `{ qmp_capabilities; ...; } | socat` pipelines
  are gone from the link-down injector (netload, sshd-start, sshd-sweep --
  verified working alone, with QEMU's x-link-down patch forced off),
  net-gate (which no longer needs socat) and input-modifiers. The three
  tests still on HMP move to QMP as they are next touched.

### 6.7 The pool by default, and the rest of the way (2026-11-06)

**Decided by Mike:** all QEMU verify and test work uses the pool -- reuse an
idle instance, boot only for a machine no class provides. That is what the
pool was built for, and until now it was opt-in (`--pool`), reached eight
converted tests, and no unit run: every `verify.sh` still booted three
guests (x64 units, aa64 units, qloop) and the suite booted for almost
everything. The service is now installed and socket-activated, so it is
always reachable on the development host.

**Step 1 -- the pool is the default (built).**
- `AXL_IT_POOL=auto`: pooled only when the user service ANSWERS
  (`axlpool status`) and serves the arch -- the runner's decision and the
  registration's are the same check, `lib/pool-check.sh ... auto`. A socket
  that merely accepts is not enough: the installed unit is socket-activated,
  so systemd keeps it listening while the service itself has failed. No
  socket: nothing pooled, nothing said. A socket but no service, no agent
  for the arch, or no resource spec: everything boots, and a check that every
  `guest=` test requires SAYS why. Pooled, the pool check stays in AUTO mode,
  so a service that dies mid-run lets the tests miss and boot instead of
  failing them all as an explicit `AXL_IT_POOL=1` / `--pool` rightly does.
- The DEFAULT is a second variable, `AXL_IT_POOL_DEFAULT=auto`, in the
  `base` test preset (inherited by the plain and suite presets): a preset's
  `environment` overrides the caller's, so putting `auto` in `AXL_IT_POOL`
  itself silently turned a caller's explicit `AXL_IT_POOL=1` or private
  socket into auto. The registration reads `AXL_IT_POOL` when it is set,
  else the default. `x64-ci` and `x64-integration` blanked both at first, so
  that CI and a release certification never depended on a developer's pool;
  **reversed by Mike 2026-11-07**: the self-hosted runners on this host use
  the pool like every other run (their own checkout's classes, run-qemu.sh
  and AGENT build -- `AXL_IT_POOL_AGENT=tree`, `axlpool run --agent tree` --
  so a CI run exercises the commit's agent, not the service checkout's), a
  miss still boots, and a runner with no service -- a hosted one -- boots and
  says so (a passing `integration.pool` check, when `CI` is set). To opt out
  under a preset -- whose default a caller cannot unset -- set
  `AXL_IT_POOL=0` (or `off`): booted, said by a passing check. The runner's
  opt-out is `--no-pool`.
- `run-integration.sh` with no flag is auto, decided AFTER every refusal (a
  refusal comes out alone) and handed to ctest as `AXL_IT_POOL=auto`;
  `--print-ctest` asks no service. `--pool` asks (refuses without it);
  `--no-pool` boots; `--ci` pools as any run does (it was booted and refused
  `--pool` until 2026-11-07). Every real run says which, and why.

**Steps 2-3 -- the unit suite on a pooled guest (built 2026-11-06).** The
whole unit suite runs on a leased guest of the `unit` class with the booted
run's exact assertion count (14909) and no failure: X64 81 s against
101-107 s booted, AARCH64 75 s against ~120 s. What it took:
- **The class** (`classes.toml`): the standard guest plus the TCP peers
  AxlTestNet connects to, as slirp `guestfwd ... cmd:` forwards (`cat` for
  the echo ports, `sh -c 'cat > /dev/null'` for the sink) -- fixed
  arguments, a program spawned per connection, no host port and no server
  for the pool to manage (`run-qemu.sh --guestfwd`); the firmware's 10 ms
  tick (`agent_latency = "firmware"`), which AxlTestNet's latency and
  AxlTestUtil's timer cases assert; and 768M of RAM (`run-qemu.sh --mem`).
- **A FAT volume at fs0, as booted.** The share is virtio-fs, and four
  binaries need FAT with them at its root: AxlTestIO and AxlTestSshdSftp the
  read-only attribute (a delete refused), AxlTestUtil its volume's root,
  AxlTestDriver FAT's names (8.3 aliases). So the generated script makes a
  FAT RAM disk in the guest (`mkrd`, the firmware's own FAT driver),
  copies the staged files onto it, sets the attribute (`attrib +r`, where
  the image build used `mattrib`), and renames the map so the disk is the
  guest's only fs0 (the pool's own disk keeps FS21; `map -d` wants the name
  WITH its colon). The disk is guest RAM -- the snapshot restore after the
  lease discards it -- which is why the class has 768M: at 512M AxlTestFw's
  LZMA compress failed on AARCH64 once a RAM disk existed, and the disk
  cannot shrink below 64 MB (AxlTestSshdSftp's grow cases need the room).
- **Each binary re-enters fs0:\ first**: AxlTestUtil leaves the Shell
  elsewhere, and the next binary's name would not resolve (the agent reads
  that as a script it could not run).
- **One test made topology-independent** (AxlTestUtil): "the first volume
  enumerated is fs0" asserted the firmware's handle order, which is no part
  of the contract -- now "one volume is fs0, and every volume the Shell
  maps is named by its alias"; and its driver_locate cases build their
  expectations from the image's own volume (`axl_app_boot_path`), compared
  exactly -- `axl_driver_locate` now names the volume by the Shell's alias
  verbatim, as the other path functions do, instead of the enumerator's
  lowercase `.name`.
- **The pool re-reads a tree's classes.toml when the file changes** (its
  signature, as for an agent image) -- adding the class needed a restart.

**Steps 3b-4 -- the units pooled per binary, and verify on them (built
2026-11-07).** `verify.sh --only=x64,aa64` with the units pooled: 71-92 s,
both arches green at 14909, no unit boot (against 168-194 s for the one-boot
run beside the gates).
- **One wrapper, two callers** (`lib/pool-or-boot.sh`): a run on a leased
  guest that, on a POOL miss -- the wrapper's, or one the command's own
  harness met, both marked `AXLPOOL-MISS:` -- says so, keeps the miss's log,
  and runs the command booted. The runner's integration tests and the pooled
  `unit.<App>` registration both use it; a command's own 77 stays its SKIP.
- **`unit.<App>` registered pooled** when the tree's pool decision pools:
  each binary under `pool-or-boot.sh --class unit`, one slot of
  `guest_unit_<arch>`, spread over idle guests by ctest.
- **The budget over pooled runs.** Each binary's count file carries its own
  run time (header to footer, `lib/unit-times.awk` -- not the lease, whose
  RAM disk and copy the one boot pays once), how it ran, and the host
  contention its run measured (the booted run's sampler, run off the
  pooled run's sentinel -- the first version never started it, so every
  pooled count said the idle 1.00). The ratchet judges the sum against the
  one-boot 110 s in whole seconds, each binary scaled by max(its
  contention, `AXL_UNIT_BUDGET_SCALE`), and folds the verdict into its exit
  after the baseline write -- a slow run is not a short one. NOT judged, and
  said (verify's table carries the line, and its verdict names an x64
  budget left unjudged; a failed ratchet's row quotes its FAIL line): a run
  no front end declared a
  scale for -- verify always declares one, while a bare `ctest --preset
  x64-suite` runs the units beside dozens of integration guests, a load a
  run-queue factor does not see all of; a booted binary (its time includes
  a boot); an unmeasured one (no timestamps -- never a 0 s pass); one whose
  contention was not sampled ("-" -- never the idle 1.00); and the aa64
  tree, whose one-boot run skips the budget too.
- **verify takes the registration's verdict.** It hands each full arch run
  `AXL_IT_POOL` (`AXL_VERIFY_POOL`, default auto; 0 opts out) and reads the
  listing back: units with a guest slot are pooled; otherwise it prints the
  registration's own `integration.pool` reason. Its first version decided
  from the socket's existence and would have said "pooled" while every binary
  booted behind a socket-activated unit whose service had failed. The RUN is
  then handed the socket the listing resolved, not `auto` again: ctest
  evaluates the registration afresh, and a service that failed in between
  would otherwise unpool the units silently; given the socket, the run's
  pool check fails instead.
- **A load-dependent bound fixed** (AxlTestLoop): "a defer queued from a
  defer runs on the next iteration" asserted >= 100 links in 50 ms of wall
  time, and failed on x64 once a pooled guest ran beside the gates -- a
  starved vCPU fits fewer links into the window. It now counts the poll ticks
  the window held and asserts >= 20 links per tick (measured ~1,400-1,700
  idle; a loop that waits for the tick manages ~1; sabotage-verified).

**Step 5 -- the quiet slot on a leased guest (built 2026-11-07).**
`scripts/qloop-verdict.sh` (verify's qloop job and CTest's `gate.qloop`, one
copy) runs AxlTestLoop on one leased `unit` guest when the pool serves, so a
full verify boots no guest at all. What qloop is and how it judges:
docs/AXL-Build-System-Design.md §10.14.
- **Registered as the units are:** pooled, `gate.qloop` takes one slot of
  `guest_unit_x64`, is told the socket and requires `integration.pool`;
  verify hands its qloop run the socket its x64 listing resolved (or its own
  `AXL_VERIFY_POOL` mode). A miss runs it booted, through the same
  `lib/pool-or-boot.sh` -- which grew `--timeout N`, bounding the pooled
  attempt (the lease wrapper with it) and the booted run alike, since the
  slot keeps its 300 s hang limit.
- **Settle first** (`axlpool settle`): a released guest's reset runs in the
  background, and the trees' last releases are still restoring when the
  slot starts -- exactly the load it exists to avoid. It waits until no
  guest of the pool, of any tree, is booting or resetting, bounded at 60 s;
  a pool still busy after that is NAMED in the verdict, not waited on
  forever.
- **The witness keeps working:** the lease names its guest's QEMU
  (`AXL_POOL_QEMU_PID`, in `Lease.env()`), and a pooled run samples that
  process's vCPU run delay exactly as a booted run samples its own --
  stopped explicitly, since the guest outlives the run. Read per thread,
  reading to reading: a long-lived QEMU retires idle I/O workers mid-run,
  and a summed reading lost their whole history (it read -59 ms). A run that asked
  for the witness and could not start it says "not measured", never 0 ms.
- **The verdict says where it ran**: "on a leased guest", "booted (no
  pool)", "booted after a pool miss (<reason>)", or "on a leased guest, the
  pool not settled (<what was busy>)".

**Step 7 -- background runs and host forwards (built 2026-11-07).** The
largest group the census could not pool -- 49 tests, ~2,500 s of work -- boot
a guest in the BACKGROUND, forward a host port to a server the guest runs,
and drive it from the host. They now pool too (`test-http.sh` first: 224 =
224 assertions, 99 s pooled against 138 s booted).
- **`test_add_port_forward` records, `test_run_background` adds**: on a
  pooled guest each pair becomes a QMP `hostfwd_add net0` on 127.0.0.1 (no
  test connects through another host address), the test's `.nsh` runs through
  the agent in the background (`axlpool exec --console`), and its serial log
  streams into TEST_LOG as a booted guest's does. A forward QEMU refuses -- a
  held host port -- answers a reason instead of `""`, and is the test's FAIL.
- **The agent moved to guest port 4747**: 22 tests serve on 8080 (and 8443,
  2222, 7000, 5640...), which the agent used to hold.
- **Forwards live OUTSIDE the snapshot** (measured: one survived a
  snapshot-load), so the pool removes every forward but the agent's before
  any reset (`Guest.drop_forwards`, checking QEMU's answer), or a lease's host
  port would stay bound into the next.
- **A snapshot class's release does not ask the agent**: a background test
  leaves its server running and the agent inside it, so its /ping timed out --
  the guest was killed, and the wait cost ~10 s of every such test's own time
  (measured: 16 ms idle, 10,023 ms busy). QEMU, a guest reset and the vCPUs
  are still asked over QMP; a snapshot restore replaces all guest state, the
  agent with it, and restore() waiting for the agent to answer is the real
  health check. A restore that fails retires the guest (the pool log says so).
- **A pooled test runs on the machine its booted twin gets.** Two gaps
  found by converting timing tests, both closed: (1) the agent's 1 ms tick
  (agent_latency "always") stayed in force while the test's program ran --
  ~0.15 cores idle against 0.02 booted, and back-off timing changed; the
  agent now drops to the firmware's tick while it runs a line and restores
  its own between lines (probe: timer-res-selftest's PERIOD reads 10000 in a
  line, the agent's /ping stays under 5 ms between). (2) run-qemu.sh's default
  NIC is virtio-net, a booted X64 test's an e1000 with QEMU's iPXE ROM: the
  `standard` class (and `unit`, inheriting) now takes `--nic-model e1000` on
  X64, so the e1000/iPXE paths stay tested wherever the pool serves.
- **No QEMU is the test's to kill**: TEST_QEMU_PID is EMPTY on a pooled
  background run -- `kill 0` would signal the test's own process group, and
  `kill -0 0` always succeeds. `test_guest_stop` ends a run either way;
  a test that polls the guest's liveness, types input, or starts a second
  background run stays booted (the second is refused by name).
- **An exec that ended is decided at once**: the wait loop sees the
  background exec gone, drains the stream, and turns a 77 into a pool miss
  (the test reruns booted) instead of "the server did not start" after the
  whole wait.

**Step 8 -- every boot onto the pool: the backlog (Mike, 2026-11-07: "make
sure we don't lose the work tasks necessary to migrate all of the QEMU boots
to the guest pool").** Measured by the TIMING lines (§6.8) over one full
uncached X64 suite: **425 booted guest runs against 54 pooled**; booted guest
time is 72.6% of the suite's 7,264 s of test work, pooled runs 10.1%, the
pool's lease + release ~0. Biggest first; tick each off here:
1. ~~**~70 tests call scripts/run-qemu.sh directly**~~ -- **mechanism DONE
   2026-11-08 (§6.9)**: run-qemu.sh's pooled mode, a mid-lease restore, the
   `plain` class. First batch, 16 tests converted by `guest=plain` alone,
   every pass count equal booted and pooled on each declared arch:
   cxx-hosted, exit-status, libc-stdio, cxx-exceptions, libc-alloc, netdiag,
   pk-verify, cut-tr, stack-guard, bss-probe, time, tool-help, cxx-iostreams,
   jose-cc, host-toolchain, lsproto (runner wall over the 16 on X64: 149 s
   booted -> 90 s pooled, two of them waiting for a fresh plain guest's
   boot; AARCH64, the 11 that run there: 232 s -> 75 s). test-time is
   declared arch=both but runs X64 only, so on AARCH64 it boots inside its
   lease.
   Stay booted, each for its reason: shell-status, ~~timer-resolution~~
   (pooled 2026-11-08 on `plain_firmware`, the plain machine with the
   firmware's own tick: its script names the tool relatively, `@echo -off`
   first), flushfail-tools, ramdisk-map (their --nsh lacks `@echo -off` or
   names fs0:); tool-redirect-pipe and jose (slower pooled: their file I/O on the
   virtio-fs share outweighs the boot); cpu-idle (measures the host CPU of
   run-qemu.sh's own QEMU); workload-gate (its 1 s budget assumes a boot);
   net-config (reconfigures the NIC the agent talks through); lsacpi,
   task-pool-mp, cpu-replay, mac-replay, no-gpu (machine options).
   Second batch (2026-11-08), `guest=plain` alone, pass counts equal booted
   and pooled: tool-version (5, both arches), kbtune-driver (14, both),
   nic (X64; three calls, each on a restored guest -- item 8's multi-boot
   for free). Every other unconverted caller boots by an option §6.9 does
   not pool, so it is in an item below or stays booted: `--gpu`
   (console-text-mode, console-reshape, gfx-mode, gfx-present, compositor,
   cursor, mkfixture-post -- item 10), `--mount` (json-corpus, tar,
   volume-map, tar-fat, shell-launcher-fallback), `--background` /
   `--serial-socket` (console-readline, tar-ctrlc, input-*, axbench-ctrlc,
   yield-ctrlc, old-shell), `--qemu-arg` (item 9's machines), a screenshot
   or held key (kbtune-bounce, sendkey-*); ~~**tools** names `fs0:\` in its
   case commands~~ -- **tools and flushfail-tools DONE 2026-11-09**
   (`guest=plain`): the cases name their files relatively, and the flushfail
   driver seeds its source in the Shell's current directory; tools' legacy-
   shell half (`--shell`) boots, as designed. ramdisk-map stays booted: the
   boot volume's map is its subject.
2. ~~**12 background tests whose startup ends in `reset -s`**~~ -- **9 DONE
   2026-11-07**: http-free-in-handler, http-rebind{,-storm,-churn,-load,
   -multi}, socket-rebind-load, ws-held-101, service-reload. The trailing
   reset only powered the booted guest off (test_use_nsh appends it); the
   `stall` before it stays, so the binary's last serial bytes (its leak
   verdict) drain into the log before the pooled exec ends and the stream
   stops. rebind-multi's own `-netdev` was exactly
   test_add_port_forward's two pairs; service-reload's launcher named its
   replacement at `fs0:\` and now names it beside itself
   (`axl_app_image_dir_path`). Left booted: **http-teardown-hooks** boots
   four times, a fresh guest per scenario (item 8); **tcp-close-pending**
   captures the wire with a `filter-dump` on net0, and **tcp-close-armed**
   that plus the X64 debugcon -- a machine the standard class does not give
   (a `filter-dump` could be added over QMP `object-add`; the debugcon
   cannot be added after boot). **tcp-close-pending DONE 2026-11-09**:
   `test_add_net_dump FILE` is the filter-dump on net0 -- booted a `-object`,
   pooled a QMP object-add before the run and object-del at the stop and in
   test_cleanup (a filter lives outside the snapshot; one a SIGKILLed test
   left behind is deleted before the next add, but keeps writing until then
   -- the pool's release should drop `axl_dump*` objects as it drops
   forwards: a service change, not made). tcp-send-cancel, booted for its
   X64 debugcon (item 5's decision), pools since every pooled X64 guest has
   one (see "A debugcon on every pooled X64 guest").
3. ~~**driver-http**: needs "keep the guest up after the script returns" on a
   pooled run (a harness gap).~~ **DONE 2026-11-09**: the gap was on the
   BOOTED side -- test_use_nsh always appended `reset -s`. `test_use_nsh
   --keep-up` ends a booted startup at the Shell prompt; a pooled guest stays
   up after the agent's run anyway. driver-http reads its bring-up lines from
   the clean log (a pooled background stream stamps each line, a booted
   background log does not).
4. ~~**9p-server**: its selftest writes `fs0:\9pexport`, not writable on a
   pooled AARCH64 guest -- make the export root configurable.~~ **Stays
   booted (2026-11-09, measured)**: with the export moved into the run's
   directory, its seed failed on both arches -- the long-name case's
   240 x U+20AC name is 720 bytes of UTF-8, past Linux's 255-byte NAME_MAX on
   the virtio-fs share (FAT takes it). The change was not kept.
5. ~~**test-sum**: on a pooled AARCH64 guest the firmware's VirtioFs lookup
   debug lines land unterminated in the serial log for its 8,158-character
   name.~~ **DONE 2026-11-09** (`guest=standard`): every AARCH64 guest has a
   debug UART now -- see "A debug UART on every AARCH64 guest" below.
6. **webdav-fs, webdav-inplace**: read and set the FAT image with mtools on
   `$TEST_DISK` -- need a share-based equivalent. (webdav-fs is the longest
   test in the suite, 165 s.)
7. **net-latency**: boots through run-qemu.sh per measurement.
8. **Harness DONE 2026-11-09**: a common-test.sh test that runs more than
   once in one lease gets a fresh machine and disk per run, as each boot is
   -- every run after the first restores the guest first (`axlpool restore`;
   a rebuilt image's test_build_image restores, then replaces the share's
   files), streams the console from there, and re-adds its host forwards
   (the restore drops them). A background run may follow a stopped one
   (test_guest_stop); one while another is live is refused. http-teardown-
   hooks (four background boots) converted this way. sd-sibling and
   service-teardown-fail stay booted (a volume-root `\drivers\` search, a
   reset), netload too (item 9).
   **~20 multi-boot tests** (netload, cxx-hosted, exit-status,
   console-device-errout, service-teardown-fail, cpu-idle, shell-status,
   sd-sibling, cxx-exceptions, ~~nic~~ (item 1), tools, workload-gate,
   libc-stdio, ramdisk-map, net-config, cpu-replay, mac-replay, libc-alloc,
   lsacpi): often one machine booted N times -- one .nsh, or one lease per
   phase.
9. **~21 custom machines** (lsusb, cpu-topology, cpu-simd, gfx-simd,
   nic-driver-name, shell-buffer-growth, stderr-stale, task-pool-mp, tpm*,
   mkfixture*, ata, nvme, scsi, smart, spd, i2c, ipmi*): one-off classes
   (`extends = "standard"`, their own args, `keep = "lease"`), per §6.2.
   **Mechanism DONE 2026-11-08** (§6.9, "The machine is compared"): a
   run-qemu.sh call or a common-test.sh test pools on a class whose machine
   is exactly its own. Converted, pass counts equal booted and pooled, no
   miss: task-pool-mp (`smp4`, both arches), ata and scsi (`ata`, `scsi`:
   their scratch disk images became QEMU `null-co` drives -- zeros of a fixed
   size, no file -- so the machine is the same every run), mac-replay
   (`mac_replay`; axl-emulate's run and the direct one each on a restored
   guest), ipmi and ipmi-ssif (`ipmi_kcs`, `ipmi_ssif`: the standard guest
   plus the BMC simulator; their inline startups moved to `.nsh`). Stay
   booted, each for a measured reason written beside it: **nvme, smart**
   (QEMU's nvme device is not migratable -- the class's snapshot-save
   refused it); **cpu-replay** (on its replayed qemu64 model the agent's
   network bring-up fails, so the class never came up); **tpm, tpm-seal**
   (a swtpm per run, whose state a snapshot does not hold); **cpu-topology,
   cpu-simd, gfx-simd, lsacpi, netload** (several machines in one test --
   a lease is one class; splitting them, or leasing several classes per
   test, is an owner decision); **nic-driver-name** (asserts the whole NIC
   set, which the agent's NIC would join). Left: **i2c, spd, mkfixture-spd**
   -- their SMBus EEPROM is `mem-path=$PROJECT_DIR/test/data/...`, an
   absolute path; classes.toml has no tree-relative form, and the class key
   does not cover a `mem-path` file (klass.py `_loaded_files`) -- both a
   change to the service's code.
10. **~23 GPU / screenshot / typed-input tests** (console-device-*, kbtune*,
    stderr-row, sendkey*, yield-ctrlc, input-*, gfx-*, compositor, cursor):
    a GPU class, and input over the lease's QMP socket. **The GPU half DONE
    2026-11-08**: the `gpu` class (`--gpu`) runs console-text-mode,
    console-reshape, gfx-mode, gfx-present, compositor and cursor on both
    arches, and `fixture_net` (`--bridges --gpu`, run-qemu.sh's own NIC)
    mkfixture-post -- pass counts equal booted and pooled. Left: the
    screenshot and typed-input ones (`--screenshot`/`--sendkey`/`--holdkey`
    still boot: run-qemu.sh's pooled mode drives neither over the lease's
    QMP yet).
11. ~~**~30 sshd/ssh background tests**~~ -- **the 20 behind `hostile_boot`
    DONE 2026-11-08**: lib/ssh-hostile-lib.sh generates its startup with
    `test_use_nsh_text` (staged files named relatively) and every test that
    reaches it declares `guest=standard`; pass counts equal booted and pooled
    on both arches. Two harness pieces came with it: `test_guest_running` and
    `test_guest_qemu_pid` (common-test.sh), because a pooled run's
    `TEST_QEMU_PID` is empty -- `kill -0 ""` read the guest as gone and the
    zero-window CPU sample read no process; and check-test-meta accepts a
    generated script when the test CALLS the lib function that generates it
    (sourcing the lib alone does not count). Left: the sshd-* tests (their own
    startups; the link-down and multi-NIC ones stay fresh or get classes).
12. **~16 tests whose subject IS the boot** (service-stage-*, old-shell,
    crashhandler, shell-launcher*, boot-path, volume-map, tar-fat,
    net-no-ipxe-autoload, run-qemu-reboot): stay booted by design --
    `guest=none`, each with its reason written beside it.
13. ~~**common-test.sh tests with an inline startup**~~ (`test_set_startup`,
    no machine of their own) -- **17 DONE 2026-11-08**: the startup moved to
    a checked-in `<test>.nsh` run by `test_use_nsh` (arguments as `%1`..`%4`,
    files named relatively), `guest=standard`; a hand-written
    `-device e1000 -netdev user` pair became `test_add_network` (the same
    pair booted on X64, the class's NIC pooled). Pass counts equal booted
    and pooled, no miss -- both arches: 9p 30, cxx-driver-ctors 17,
    hello-minimal 5, minimal-log 6, service-reload-fail 6,
    service-reload-stuck 4; X64 only: console-mirror 17, driver-leak 9,
    echo-client 3, tcp-multi-transmit 3, sntp 2, udp 4, tcp-connect-rtpl 3,
    net-tools 18, https-client 3, http-async 11, redfish 13 (redfish's
    startup had no `reset -s`, so its booted guest idled to the 60 s limit;
    test_use_nsh's reset ends it). driver-leak-test loaded
    `fs0:\driver.efi` (3 of 4 phases failed pooled) and now names it beside
    itself (`axl_app_image_dir_path`), as reload-svc does. Boot removed, as
    the booted runs measured it: 74.6 s on X64, 62.6 s on AARCH64.
    Left booted, each for its reason: **i2c, spd, mkfixture-spd, ipmi,
    ipmi-ssif** add a device (an SMBus EEPROM, the BMC simulator -- item 9);
    **stderr-stale** the debugcon; **tcp-send-cancel** a `filter-dump` on
    net0 (as tcp-close-pending); **9p-tool-serve** types into the guest
    (`test_send_stdin`, item 10); **sshd-stuck** drives sshd's fault hooks,
    which read fixed `fs0:\sshd-fault-*` files; **vmedia-reads** -- its
    driver publishes `AXLVM:` as a copy of `fs0:\vm\`; **efi-pack,
    efi-pack-drivers, image-path, service-pin-path** assert or pass paths
    rooted at the volume (`ARGV[0]=fs0:\sub\...`, `<vol>:\app\...`,
    `driver_path` pins at `fs0:\`) -- the paths are their subject;
    **console-mirror-edit** has the child Shell's `edit` write
    `fs0:\mtxt.txt`, which on a leased guest is the pool's own disk.

**Backlog pass of 2026-11-09 (items 1-3, 8-10), and what is left.** 26
backlog tests converted or given a measured permanent reason (above, per
item); 17 still say `not converted yet`: the screenshot/typed-input ones
(console-device-fbcon, -fbcon-life, -input, -takeover; kbtune, kbtune-bounce;
sendkey-load, sendkey-render -- run-qemu.sh's pooled mode does not drive
--screenshot/--sendkey/--holdkey over the lease's QMP yet), the four webdav
tests (item 6), net-latency (item 7), i2c/spd/mkfixture-spd (an absolute
mem-path) and test-sum (item 5). Full uncached suites at the end, pooled:
X64 325 passed, 5 failed, 1 skipped (json-corpus); AARCH64 148 passed, 4
failed, 1 skipped -- at a host load of 30-37 on 12 cores (other agents' runs
alongside). Two failures were this pass's own and are fixed
(test-timing-lines' fake lease ran twice without a restore stub;
test-pool-resource-spec pinned the old class list and hyphenated names); the
other seven (X64 net-latency, console-device-fbcon-life, http-throughput,
timing-lines' boot-time bound; AARCH64 ssh-admission, sshd-keys, ssh-rekey)
are timing/count bounds under that load and passed rerun alone, pooled.

**Every guest accounted for: `guest=none` and the lint (2026-11-08).** A
test that launches a guest now declares either the class it runs on or
`guest=none`, with a `# guest=none: <why>` header comment; `make
check-test-meta` (lib/discover.sh `--lint`) fails a test that launches a
guest with neither, a `guest=none` with no reason, and a `guest=none` on a
test that launches nothing (stale). "Launches" means a guest launcher, real,
stubbed or dry-run (so the harness tests that drive run-qemu.sh against stubs
carry `guest=none` too, saying so): `test_run_foreground` /
`test_run_background` / `test_timing_begin`, a `"$DIR/scripts/<name>"` word
for run-qemu.sh or a shipped script wrapping it (ssh-demo.sh, axl-emulate,
profile-qemu.sh, show-it.sh, console-visual.py) or a `RUN_QEMU` variable,
`qemu-system-*`, `"$TEST_QEMU_BIN"` / `"$QEMU_BIN"` or the test's QEMU
command array, and the pool's own `test/pool/*-cases.py` -- in the test or
any `lib/*.sh` it sources (through `$(dirname "$0")`, `$SCRIPT_DIR` or
`$TESTS_DIR`); a launcher in a comment, run-qemu.sh's path written, hashed or
listed, or lib/discover.sh's own pattern, is not one (each pinned, with
controls, in test-discover-selftest.sh). It is a superset of what
lib/timing-report.py sorts a log by. Every consumer that leases, labels or
registers by class asks `test_meta_pool_class`, for which `none` is no
class. 141 tests were tagged when the lint landed; a reason beginning "not
converted yet (... item N)" is a backlog entry above, the rest are the
decisions. `grep -l 'guest=none: not converted' test/integration/test-*.sh` is the
remaining backlog.

**A script whose last status is 0xE booted quietly -- fixed (2026-11-08).**
The agent reads a script whose last status-setting command returned 0xE (a
`type` of a missing file followed by an `echo`, which sets no status; an app
returning 14) as `not-run` -- the 0xE row of include/axl/axl-shell.h, the
safe direction for the API's own callers. For the harness it was the unsafe
one: `axlpool exec` exited 77, and both common-test.sh's pooled runs and
run-qemu.sh's pooled mode took that for a pool failure and reran the test
booted, so any converted test with such a script silently stopped pooling
(test-9p-tool, whose last failing command is a `type` after `umount`, was
found this way: pool miss on both arches). Now common-test.sh gives the agent
a wrapper, `axl-pool-run.nsh` (`if not exist <script>` -> a marker, the
test's line, `@cd .`), and run-qemu.sh ends its generated script with `@cd .`
-- the one silent command that sets `%lasterror%` to 0, as a boot's
startup.nsh discards the status. What the wrapper would hide, the test's own
script missing on the guest, it reports itself (`AXL-POOL-NO-SCRIPT`), and
common-test.sh turns that back into the pool miss it is. Pinned by
test-pool-script-status-qemu.sh (real lease), a `notfound` case in
test-run-qemu-pooled-qemu.sh, and the wrapper's bytes and the marker's miss
in test-run-qemu-pooled.sh; each confirmed failing before the fix and caught
by sabotage after.

Third batch (2026-11-08), pass counts equal booted and pooled, no miss, both
arches: 9p-tool 15 (its files named relatively, `9p: wrote 18 bytes to
got.txt`), heap-release 47, console-device-hold 12. Two of heap-release's
children ran their own child at `fs0:\` and now name it relative to the
Shell's directory, as the observer names them (axl_app_image_dir_path does
not resolve in an image the observer launched -- measured); childrun's entry
fails by design, so its heap check alone passed with a child that never ran
-- it now says so and the test counts 11 child runs (sabotaged: caught).
console-device-hold's driver reads `busy.nsh` beside its image. **test-sum
stays booted** (item 5): converted to a generated script, it passes pooled on
X64, but on a pooled AARCH64 guest the firmware's VirtioFsDxe prints a DEBUG
error for its 8,158-character name (ENAMETOOLONG from the host) on the
console UART, truncated at 256 bytes with no newline, and the tool's next
line lands after it. Measured cause: AARCH64 firmware DEBUG output shares the
console UART; with a SECOND `-serial` the bundled edk2-stable202408 firmware
sends DEBUG to it and the console stays clean (X64 already sends DEBUG to the
0x402 debugcon). The fix is a debug UART on pooled AARCH64 guests, with the
pooled ASSERT checks reading it -- **done 2026-11-09, and for booted runs
too** (next paragraph).

**A debug UART on every AARCH64 guest (2026-11-09).** Booted and pooled
alike, every AARCH64 guest gets a second serial port for the firmware's DEBUG
output -- `qemu_fw_debug_uart` in axl-common.sh, the one place both
run-qemu.sh (`finalize_qemu_cmd`, every launch mode) and common-test.sh
(`_test_fw_debug_uart`, every launch of TEST_QEMU_CMD) take it from -- so the
console UART carries no DEBUG output. X64 is unchanged: OVMF sends DEBUG
to the 0x402 debugcon, and no X64 class key moved. The order is the contract:
on `virt` the UARTs follow `-serial` order, so the debug one comes after the
console's, and a `-nographic` launch spells its console out first
(`mon:stdio`, or `stdio` when -monitor/-mon/-qmp took the monitor -- QEMU's
own rule, system/vl.c), since any explicit `-serial` drops the one -nographic
would add. Where it is written: a pooled guest's `$STATE_DIR/firmware-debug.log`
(run-qemu.sh `--state-dir`), beside its console; a booted run's
`<serial-log>-firmware-debug.log`, `--debugcon FILE` (which on AARCH64 had
made QEMU fail -- `virt` has no ISA bus), else the run's temp dir;
common-test.sh's `TEST_FW_DEBUG_LOG`, `$TEST_TMPDIR/firmware-debug.log` or
TEST_QEMU_DEBUGCON. The chardev appends (`append=on`): a pooled guest's cold
start relaunches the recorded argv and must not truncate what a lease reads
from an offset (measured: the boot's lines survive the hand-over), while each
booted launch truncates its own. No service change: guest.py records the
real argv. A pooled test streams the guest's log from the offset at its run's
start into TEST_FW_DEBUG_LOG; a lease whose guest has no such log is a pool
miss, never a blind pass -- in common-test.sh and in run-qemu.sh's pooled
mode alike (there the run boots, saying why). A pooled guest's log only
grows, across leases and cold starts; at ERROR level it is a few KB a boot.

The readers moved with it, each proven to SEE an assert: common-test.sh's
DXE-core guard reads TEST_LOG and TEST_FW_DEBUG_LOG (a debug log asked for
and not written fails); `test_check_firmware_asserts` checks the debug UART
on AARCH64 -- its CONTROL is that the boot wrote there (pooled: the guest's
whole log), because the bundled AARCH64 firmware logs at ERROR level and
prints no load lines (so it depends on the firmware logging something at
boot; test-fw-debug-uart-qemu.sh pins the routing on a line the test itself
causes); when a run hit an ASSERT or a CPU exception, run-qemu.sh prints the
debug UART's log after its output as `[firmware-debug] ...`, booted and
pooled, so a caller grepping its output (task-pool-mp) still sees one, and
common-test.sh appends it to TEST_LOG on an exception. That last one was the
review's catch: an AArch64 exception's header is the console's
(SerialPortWrite) but its registers, backtrace and stack dump are DEBUG, so
they had left every log a test reads; test-harness-exception-qemu.sh now
pins the `ELR` line in the log on AARCH64, booted and pooled. Real-firmware controls: re-adding the
TCP Cancel on an armed Transmit gave tcp-send-cancel on AARCH64 `PASS: no
firmware ASSERT on the serial log` and `FAIL: no firmware ASSERT on the debug
UART` (`ASSERT [TcpDxe] ...LinkedList.c(594)`) -- the console check alone
would have been blind; re-adding the driver start-failure double free gave
service-startfail on AARCH64 the DXE-core guard's `*** FAIL` on an `ASSERT
[DxeCore] ...Pool.c(721)` that only the debug UART carried; test-fw-debug-uart-qemu.sh pins the routing on a real
guest through both harness paths, booted and pooled; test-fw-debug-uart.sh
pins every launch mode's `-serial` order against a stub QEMU. efi-pack and
stderr-row dropped their filters for DxeCore's "Error: Image at ... start
failed" line, which no longer reaches the console. test-sum pools
(`guest=standard`). **tcp-send-cancel stayed booted**: its X64 half needed
a debugcon, which a pooled X64 guest did not have (it cannot be added after
boot), and `guest=` names one class for both arches -- until the next
paragraph gave every pooled X64 guest one.

**A pool's class keys are cached per tree until `revalidate`** (pool.py
KEYS): a run-qemu.sh change that alters a guest's argv is not seen by a
running service until the tree is revalidated (or the service restarts) --
measured here, the first pooled AARCH64 lease after the change was a guest
booted before it, which the harness turned into a pool miss. `axlpool`
has no CLI verb for it; `PoolClient().revalidate(tree)` retired exactly the
five AARCH64 classes and no X64 one.

Full uncached X64 suite after these two commits: 330 passed, 0 failed, 1
skipped (json-corpus: no corpora), no pool miss; and no test without a
`guest=` recorded a booted run -- the lint's claim, checked against what the
run did. One pooled attempt of console-device-hold timed out at its 40 s
limit (41.3 s, its HOLD-RELEASED never seen) under the suite's 12x packing
and passed on the retry in 7.9 s; six isolated runs, pooled and booted on
both arches, never did. Explained 2026-11-09 (next paragraph): the test is
booted again.

**A debugcon on every pooled X64 guest (2026-11-09).** OVMF sends DEBUG
(ASSERTs included) to the isa-debugcon at I/O port 0x402, and probes the
port first: with no such device it writes nothing, so a pooled X64 guest's
firmware ASSERT showed only as a hang and a timeout, and a test that asked
for the capture (TEST_QEMU_DEBUGCON) could not pool. Every pooled X64 guest
now has it: `qemu_fw_debug_uart` -- the AARCH64 helper, now both arches --
gives X64 `-chardev file,id=axl_fwdebug,path=<file>,append=on -device
isa-debugcon,iobase=0x402,chardev=axl_fwdebug` (`-debugcon file:` cannot
append, and a cold start replays the recorded argv). run-qemu.sh adds it to
an X64 launch with `--state-dir` -- the pool's launch, so the log is the
guest's `$STATE_DIR/firmware-debug.log`, where the AARCH64 one is -- and the
dry run lists it, so every X64 class key moved. Everything downstream is
the AARCH64 path with the arch gate removed: a lease with no such log is a
pool miss in common-test.sh and a boot (saying why) in run-qemu.sh's pooled
mode; the run's part is streamed or sliced from the offset at its start
into TEST_FW_DEBUG_LOG and `<serial-log>-firmware-debug.log`; the DXE-core
guard reads it; `test_check_firmware_asserts`' pooled X64 CONTROLs are its
MARKER (`TcpDxe`) in the guest's whole log, whose boot loaded it, and a
`Loading driver at` line in the run's own part (DxeCore logs every image
load, the run's app included) -- so a stream gone blind fails rather than
passing the ASSERT check over nothing, which AARCH64's ERROR-level firmware
cannot offer; run-qemu.sh
prints a pooled run's part as `[firmware-debug] ...` when it holds an ASSERT.
TEST_QEMU_DEBUGCON is no longer refused on a lease. A booted X64 guest keeps
its default (no port); one that asks (TEST_QEMU_DEBUGCON) gets the same
helper, so its debugcon is read by the same guard. run-qemu.sh's booted
`--debugcon FILE` is unchanged (plain `-debugcon`, nothing printed), and its
pooled mode still boots for `--debugcon`: what that option is for,
gdb-syms.py, needs every module's load line, which are the guest's boot's,
not a lease's.

Proven on real guests: test-fw-debug-uart-qemu.sh on X64 pooled finds
DxeCore's "Error: Image at ... start failed: Compromised Data" once in the
common-test.sh log and once beside run-qemu.sh's --serial-log, and booted,
once in a `--debugcon` file; dropping the `-device` from the pool's argv
(revalidated) failed both pooled checks, and so did blinding either reader's
X64 path. Re-adding the TCP Cancel on an armed Transmit gave
tcp-send-cancel, pooled on X64, `PASS: no firmware ASSERT on the serial log`
and `FAIL: no firmware ASSERT on the debugcon` (`ASSERT ...LinkedList.c(594):
!IsListEmpty (Entry)`). **tcp-send-cancel pools** (`guest=standard`, both
arches; its commands moved to test-tcp-send-cancel-qemu.nsh, its filter-dump
to `test_add_net_dump`). stderr-stale and tcp-close-armed were booted for
the same debugcon and could now be tried pooled -- not done here.

Class keys: `PoolClient().revalidate(tree)` on a service holding a booted
X64 and AARCH64 standard guest retired exactly `standard/X64` -- each time
the X64 argv changed -- and no AARCH64 class.

THE COST, measured (owner's decision whether a booted X64 run gets it by
default): a booted X64 run to app exit (run-qemu.sh, the standard machine,
exit-status-selftest), 10 boots each way interleaved, host load 2.6-3.2 on
12 CPUs: **2.08 s without the port, 3.66 s with it** (boot=, means; wall
2.23 / 3.83 s) -- +1.58 s, +76 %, for 148-150 KB of DEBUG output: ~10.5 us a
byte (a VM exit and a one-byte file write each). A pooled guest pays that
once at its boot; a lease pays only for what the firmware prints during its
run. Measured over the full pooled X64 suite (below):
147 X64 leases and 23 X64 guest boots wrote ~6.0 MB to the guests' logs, of
which ~3.5 MB is boots (~150 KB each); the rest, ~2.5 MB, is ~17 KB a lease
(a few hundred KB for the heaviest guests) -- ~0.18 s a lease and ~26 s of
the suite's 7,663 s of test work at 10.5 us a byte (an estimate from bytes
and the booted per-byte cost, not an A/B of the suite), plus ~1.6 s on each
pool boot, which a lease waiting for a fresh guest pays.

Full X64 suite on a private pool from this tree (cache ON, -j 16): 341
passed, 0 failed, 1 skipped (json-corpus: no corpora), no pool miss;
yield-ctrlc (guest=none, booted, no debugcon in its argv) passed on its
retry. AARCH64 (same pool): 152 passed, 1 failed, 1 skipped
-- the failure net-latency (guest=none, booted, an AARCH64 guest this change
does not alter) on a timing bound (first exchange 21 ms against 15) under
13x packing; alone it passed 26/26.

**The share cannot take a read from a timer callback (2026-11-09).**
console-device-hold failed pooled in CI (run 42578726165) the same way, and
locally 12 of 78 pooled X64 runs under load (16 busy loops beside 6
concurrent runs), 0 of 12 without the busy loops (host load ~25 from other
work). Each hang: the Shell, three
scripts deep in busy.nsh, inside `ShellFileExists(cwd)` -- the check
RunShellCommand makes after EVERY command, on the lease directory, so on the
share -- and from there in VirtioFsDxe's VirtioFlush, polling the used index
in 1 ms Stalls forever (QMP register samples: RIP in the AcpiTimerLib delay,
the frame chain through VirtioFsDxe into the Shell). VirtioFsDxe raises no
TPL around a request (FAT takes a TPL_CALLBACK lock; edk2-stable202505 is
the same as 202408), and VirtioFlush waits for the used index to EQUAL the one it
published. The driver's 10 ms tick, at TPL_CALLBACK, reads busy.nsh through
the Shell protocol; when it preempts one of the Shell's requests its own
eleven requests carry the ring past the Shell's index, which then never
matches: in three captured hangs the ring stood exactly 11 past the index
the Shell waited for. With the tick's read moved to the FAT boot disk and
nothing else changed, 0 of 30 under the same load; booted, 0 of 30 (15 X64,
15 AARCH64) under the same load. The hangs were captured on X64; AARCH64's
firmware runs the same VirtioFsDxe code. A preemption that lands before the
Shell publishes its chain does worse than hang: VirtioPrepare starts every
chain at descriptor 0, so the tick overwrites the Shell's descriptors. The read is legal UEFI
(EFI_FILE_PROTOCOL is callable at TPL_CALLBACK) and ordinary for a resident
driver, so the test went back to `guest=none` and refuses a lease
(`_test_unpooled`). Keeping it pooled would mean staging the file the
tick reads on a FAT volume of the leased guest, which the test cannot name
portably; fixing VirtioFsDxe (a TPL_CALLBACK raise around each request, as
FAT does) is an upstream edk2 change. **Before converting a test,
ask whether anything in it does file I/O from an event callback or a
driver's loop tick: on a leased guest that I/O is on virtio-fs and can
wedge the Shell or corrupt its request.** The two pooled tests whose driver fixtures do file I/O
(driver-identity, flushfail-tools) do it in the image's entry or unload,
in the Shell's foreground, which is serialized with the Shell's own.

**Re-measured 2026-11-08, and what the rest is worth.** One full uncached
X64 suite after item 11: **373 booted guest runs against 124 pooled**; boot
13.4% of 7,775 s of split test work, booted run 39.5%, pooled run 22.6%. Its
wall did not move -- 571 s against 572 s that morning -- because the suite is
CPU-bound (7,860 s of work at 14x packing on 12 cores), so wall tracks total
CPU work, and a pooled run saves only its boot (~7 s on X64; the run itself
costs the same). The boots left are ~1,040 s, of which ~250 s belong to tests
that must boot (the pool's own tests, service-stage-*, machine options):
converting every other one is worth at most ~50-60 s of wall (~9%) locally.
The remaining items are therefore taken in order of BOOT SECONDS removed, not
of tests converted. What pooling a long test can cost: a pooled test waits
for a class slot before it starts, so with 4 standard slots the 20 ssh tests
alone took 220 s against 100 s booted; the standard class has 8 since (127 s;
AARCH64 151 s), and the service's memory cap -- `axlpool serve --mem-cap-mb`,
default half of MemTotal, stated in its `started` record -- bounds the
resident guests the raise allows (it had been in `Pool` but never set by
`serve`). A cap below the service's own tree's largest class refuses the
start; a class from another tree that cannot fit misses at once as
`over-cap`, naming both sizes, instead of waiting out the lease deadline to a
reasonless `no-slot`.

### 6.8 Where a test's wall time went: the TIMING lines (2026-11-07)

§1 measured the suite with the opt-in profile (`AXL_TEST_PROFILE`), which saw
booted guests only and needed a special run. The question the pool keeps
raising -- *of this test's wall time, how much was guest boot, guest run,
pool lease/release, and the host?* -- is now answered by every run's logs,
CI included, with no flag:

| line | written by | when |
|---|---|---|
| `TIMING: guest kind= via= guest= boot= rc=` | `common-test.sh` | each guest run ends (booted fg/bg, pooled fg/bg, `test_timing_begin` direct launches) |
| `TIMING: test=<name> total= setup= guest= boot= between= post= booted= pooled= start= lease=` | `common-test.sh` | once, as `test_cleanup` begins |
| `TIMING: run-qemu mode= guest= boot= rc=` | `scripts/run-qemu.sh` | its guest ends (`--background`: from the watchdog subshell) |
| `TIMING: runner test= attempt= via= wall= cpu= rc=` | `lib/run-one.sh` | after each run (a `run-qemu.sh --background` guest outliving its test may add its line after it) |
| `axlpool run: leased <guest> <class>/<arch> (<start>, pool start <ms> ms) in <ms> ms` / `released <guest> (<outcome>) in <ms> ms` | `axlpool run` | lease granted / guest handed back |

The fields are defined once, in the "Wall-time accounting" block of
`scripts/axl-common.sh`. `boot` is launch to the Shell banner on the serial
console (`UEFI Interactive Shell`): from the `ts` stamp on a booted foreground
run, from a watcher on a background run and on `run-qemu.sh`'s log; `-` on a
pooled run, whose start kind and cost are the lease's (`AXL_POOL_START`,
`AXL_POOL_START_MS` from `Lease.env()`, and `AXL_POOL_LEASE_MS`, the wait as
the wrapper measured it).

**Decisions, and why.**

- **Into the test's own log, by path.** `run-one.sh` exports
  `AXL_TEST_TIMING_LOG=<logdir>/<test>.log` and opens that log O_APPEND (and
  `pool-or-boot.sh` the pooled attempt's), so a line appended by path lands
  between the test's output instead of under its file offset. Writing to
  stderr instead would have lost `run-qemu.sh`'s line in 46 tests, which
  capture its stdout+stderr in a variable and parse it -- and could have broken
  their exact matches. Unset (a test run by hand), every producer writes only
  to a terminal, never into a stderr that is not one -- a self-test capturing
  a harness snippet's `2>&1` read the summary as its last line when
  `common-test.sh` wrote to stderr; a test started from a terminal points the
  variable at it, so its children's lines show there too.
- **Cheap.** Clock reads are `$EPOCHREALTIME` (no fork); a foreground boot
  reading is one grep of the stamped log after the run; a background guest
  gets one disowned watcher subshell (two `tail --pid`s), which ends with the
  guest. A `grep -m1 -q` reading the banner was measured holding its match
  until its input ended (0.5 s reported as 1.5 s), so the watcher reads lines
  in bash.
- **Kill-safe.** The summary is taken as `test_cleanup` begins, before its
  teardown, so a hanging teardown cannot lose it; a run still open then is
  closed as `rc=unfinished` (foreground) or `-` (a background guest the
  cleanup ends). A SIGKILLed test prints nothing of its own, but its finished
  runs' lines are already written and the runner's line still follows.
  `run-qemu.sh` prints `rc=unfinished` from its guard trap.
- **A run belongs to the shell that started it**, and only the main shell
  prints the summary: a `( test_setup ... )` helper cannot double-count, and
  a test calling `test_cleanup` before its trap gets one summary.
- **Release is the wrapper's line, not the test's**: it happens after the
  test has exited.
- **The opt-in profile stays** (`lib/profile.sh`): it carries the launch
  census (argv, staged-file digests), which an always-on path should not pay
  for.

`lib/timing-report.py <logdir>` tabulates a run: per test TOTAL, SETUP, BOOT,
GUEST-B (booted), GUEST-P (pooled), POOL (lease + release), POST, HOST (the
rest), sorted by total, a SUM row and the shares of boot / booted run / pooled
run / pool overhead / host. It is a census: a test with no guest timing is
sorted by what its script (or a `lib/*.sh` it sources, comments ignored) can
launch -- QEMU directly: UNMEASURED; only an instrumented launcher (which
reports every guest it starts): STARTED NO GUEST (a dry run, a stub);
neither: HOST-ONLY. Retries and pool misses are counted below the table.

First full run (X64, uncached, 316 tests, 2026-11-07): 7,264 s of split
test work over 314 tests -- boot 12.2%, booted run 60.4%, pooled run 10.1%,
pool lease+release 0.0% (a lease of an idle guest is ~5 ms: the reset runs in
the background after the release), host 17.3%. That run predates the boot
readings for lib/stage-lib.sh and the crash guests, so its BOOT is a floor
(105 booted runs without a reading, 77 of them since closed). Unmeasured: 2, neither a guest
(test-run-background-pdeathsig.sh's `-M none` child, test-timing-lines.sh's
stub). Boot readings are a floor where a guest prints no `UEFI Interactive
Shell` banner (the EFI 1.x shell, a `--serial-socket` guest, stubs). Pinned by `test-timing-lines.sh` (the producers) and
`test-timing-report.sh` (the report, exact output over fixture logs).

### 6.9 `run-qemu.sh` on a leased guest (2026-11-08)

§6.7 step 8 item 1: ~70 tests launch guests with `scripts/run-qemu.sh`
directly (142 call lines), so common-test.sh's pooled path never sees them.
Porting each to common-test.sh means rewriting how 46 of them read their
output; instead **run-qemu.sh itself runs on the lease** when one is held and
the machine asked for is the lease's class. A test opts in exactly as any
other does -- `guest=<class>` in its test-meta, nothing else changes.

**The contract** (scripts/run-qemu.sh's header is the authority):

- **When.** `AXL_POOL_AGENT` is set (the runner leased the test's class and
  runs it under `axlpool run`), the run has an app, its `--arch` is the
  lease's, and its machine IS the lease's class's (below): `plain` is a bare
  run or `--net` alone (a booted run's `-net none` or virtio-net -- the class
  adds the agent's virtio-net NIC and the virtio-fs share); `standard` is
  `--bridges --net` with the class's NIC (`--nic-model e1000` on X64); a
  one-off class is the run naming exactly its options.

  What is not the machine but the way a run is driven boots exactly as
  before: the screenshot and input options, `--display`/`--vnc`, `-i`,
  `--mount`, `--background`, `--debugcon`, `--gdb`, the caller's sockets,
  `--reboot`, `--boot-target`, `--shell`, `--qcow2`/`--state-dir`/
  `--hostfwd-bind`, `--tap`, `--hostfwd`; no app, `QEMU_DRYRUN`,
  `AXL_RUN_QEMU_POOL=0`.
- **The machine is compared, fact for fact, with the class's (2026-11-08,
  items 9-10).** `axlpool run` exports the class's run-qemu.sh options --
  classes.toml's args then its arch's, `ClassSet.machine_args`, the list the
  pool boots it with -- as `AXL_POOL_CLASS_ARGS`, one per line (a class arg
  may not contain a newline; classes.py refuses one). run-qemu.sh parses them,
  after the `--net` every pooled guest has, with its OWN option loop (a run
  of itself under `_AXL_RQ_MACHINE_SIG=1` that prints the parsed machine and
  exits), and compares that with the run's: `--bridges`, the NIC (model,
  `--nic-no-rom`, `--mac`), `--gpu` (or what implies it), `--no-gpu`, `--cpu`,
  `--mem`, each `--guestfwd` and each `--qemu-arg` in order. The first fact
  that differs is said (`the machine is not the gpu class's: the run has
  gpu=false, the class gpu=true`) and the run boots; a lease with no
  `AXL_POOL_CLASS_ARGS` boots too. One parser for both sides means an option
  spelled two ways (`--nic-model e1000` with or without `--net`) is one
  machine; and a new option cannot pool by accident -- test-run-qemu-pooled.sh
  reads the option loop's case labels and fails unless each is in exactly
  one list: a machine fact, a way of running that boots, or neither. The plain/standard rules of the
  first cut fall out of it unchanged (every earlier pooled test still pools).
  **common-test.sh asks the same question** of a pooled test's own QEMU
  arguments (what it appends to `TEST_QEMU_CMD`, or a device adder's --
  `test_add_ipmi_bmc_sim_kcs` and the rest now append on a lease too):
  `_test_pool_machine_check` hands run-qemu.sh the standard guest plus those
  arguments under `_AXL_RQ_MACHINE_SIG=compare` (no standard NIC when they
  bring their own, as `_test_default_nic` boots it). The same machine runs
  pooled; any other is the test's misdeclared `guest=` and fails loudly.
  Class names are now `[a-z][a-z0-9_]*`: the CMake registration names a
  CTest resource type `guest_<class>_<arch>` verbatim, and a hyphen in one
  (`plain-firmware`, the first spelling) broke the whole pooled registration
  (`set_tests_properties unknown error`).
- **A fresh machine per call.** Before each pooled run the lease's guest is
  restored to its clean snapshot (`axlpool restore`, below), so a run never
  sees what an earlier run -- or the test's own pooled use of the lease --
  left in guest RAM, NVRAM, the Shell's variables or a loaded driver; and its
  files go to a directory of their own (`rq-<n>`) in the lease's share, so it
  never sees an earlier run's.
- **The same script.** The run executes, through the agent with its output on
  the guest's console (`axlpool exec --console`), the script a boot's
  startup.nsh would: `@echo -off`, the volume and directory, the `--setvar`
  lines, the sentinel echo, then the app (`load` for a driver) or the `--nsh`
  script's lines. A `--nsh` script pools only when it starts with `@echo -off`
  (a boot echoes every command before it); that line, a following `fs0:` and
  `cd \`, and a last `reset -s` are the boot's boilerplate and are dropped.
  What remains boots, said, when it has a `reset` or `exit` line (nothing
  would end the agent's line the way a boot's QEMU exit does), names `fs0:`
  (on a leased guest that is not the run's disk), or changes to a volume's
  root (`cd \...`).
- **The boot disk's Shell, imitated where it shows** (each found by a real
  run failing, then pinned): the run's directory is the Shell's `path` (a
  boot's is `FS0:\efi\tools\;FS0:\efi\boot\;FS0:\`, the leased Shell's names
  its own volumes, so a bare `tool` was "not recognized"); every staged
  `.efi` is also there as `.EFI` -- the Shell looks a bare name up with the
  extension UPPER case, which FAT matches and the share (virtio-fs over a
  case-sensitive Linux directory) does not; a copy, since a hard link made
  VirtioFsDxe fail to run either name; and only a run whose script had
  `fs0:`, or an app run, enters its directory -- a boot's startup.nsh has no
  current directory before `fs0:`, so a relative name finds nothing there.
- **The same output.** The console from the sentinel on, ANSI-stripped and
  filtered exactly as a boot's serial log is (`--raw`: from the run's start --
  there is no firmware boot to show; `--serial-log` the same slice). The 46
  tests that parse run-qemu.sh's output read the same bytes.
- **The same exit codes**: 0 (the app's own status is not run-qemu.sh's, as
  booted -- including a run cut off at `--timeout`), 1 (setup refused), 2
  (`--workload`), 8 (a CPU spike, sampled on the leased guest's QEMU,
  `AXL_POOL_QEMU_PID`, with no boot warm-up), 9 (`--max-duration`).
- **A pool failure is never the run's verdict**: a refused or failed restore
  (a service older than the op answers `unknown-op`), the agent unable to run
  the script (exec's 77), or no sentinel on the console -- the run boots
  instead, as before.
- **Said, but not to the caller.** `TIMING: run-qemu mode=pooled ...`, and a
  booted run inside a lease says why in one line, `run-qemu: booted inside a
  <class> lease: <reason>` -- both to the TIMING channel
  (`$AXL_TEST_TIMING_LOG`, else a terminal), never into the stdout/stderr
  callers capture.

- **Beside common-test.sh's pooled runs in one test.** A pooled run-qemu.sh
  call boots instead while the test's own pooled background run is live
  (`AXL_POOL_BG_LIVE`, exported by common-test.sh: a restore would end that
  run's server). And common-test.sh's next pooled run, finding a run-qemu.sh
  directory in the share, restores the guest first and streams the console
  from then -- the earlier run's state and output are not its own.
- **Fixed in review** (independent pass before commit): the pooled run is
  called as a plain command, so `set -e` holds inside it as in the booted
  path (`f || ...` had turned it off: a failed copy went unseen, and an app
  printing nothing exited 0 where a boot exits 1); the exec client is started
  as the process itself, so a signal's guard reaches it; the sampler's
  sentinel is in the EXIT trap; the pooled run's zero warm-up does not leak
  into the boot after a pool failure; the run records its skip-cache inputs;
  a failed restore marks its lease taken back before a waiting release can
  claim it.

**`axlpool restore`: a mid-lease restore (service op `restore`).** The
release already restores a snapshot class's guest (~0.4 s, in the
background); the new op does the same for a HELD lease, which stays held.
It takes the lease's restore token -- a capability the lease's environment
carries (`AXL_POOL_RESTORE_TOKEN`), separate from the release token the
wrapper alone holds: a test can give itself a fresh machine, never end the
lease its wrapper owns. A failed restore takes the lease back (its release
says replaced, `restore-failed`).

*Restore, not a lease per call -- measured* (X64, inside one plain lease,
idle host): `axlpool restore` costs 191-204 ms wall (110-116 ms of it the
snapshot load, the rest the CLI's start). A nested lease per call costs
129-133 ms wall once a SECOND guest is idle -- each waits ~40 ms for the
previous call's background reset -- but the first one booted that second
guest (7.8 s), and it holds a slot CTest's resource accounting never granted
the test: with every slot held by tests each waiting for a nested lease, they
all wait out the lease timeout. And the test's own leased guest sits idle
throughout. The restore reuses the slot the test holds, deterministically,
for ~70 ms more per call. A pooled call measures 0.5-0.6 s (restore
included) against 1.2-1.7 s booted on X64, and 1.0-1.2 s against 6.4-6.9 s
on AARCH64 (TCG).

**The `plain` class -- the evidence.** The standard class is `--bridges` plus
a NIC; a booted plain run-qemu.sh guest has neither (`-net none`). lsproto's
present view, booted plain against pooled: `EFI_PCI_IO_PROTOCOL` on 5 handles
booted, 7 on a plain guest (the agent's NIC, the share), 10 on a standard one
(the root port, the xHCI) -- which alone adds `EFI_USB2_HC_PROTOCOL` and three
`EFI_USB_IO_PROTOCOL` handles a plain run never has. `plain` is the bare
machine plus exactly what the agent needs. It cannot be identical: the agent
talks TCP, so the network stack is up (ARP, DHCP, IP4/6, TCP, UDP... -- 68
of 124 lsproto lines differ, plain booted against plain pooled). A test that
enumerates the machine in full stays booted; one that asserts what it
needs (lsproto's own test) is unaffected.

**The lint** (`gate.check-test-meta`, lib/discover.sh): a `guest=` test runs
on the lease through `test_use_nsh` with its `.nsh` beside it, OR through
`scripts/run-qemu.sh` calls -- one of the two, or the lint names it.

### 6.10 The code the service runs is in a pooled test's cache key (2026-11-09)

**The gap.** The skip cache (`test/integration/lib/test-cache.sh`) gives a
pooled run its own key (`AXL_TEST_CACHE_MODE=pool`), and every key hashes the
tree's `test/pool/` as harness. But the guests come from the long-lived user
service, which runs the code it was STARTED with -- an older commit, or the
main checkout's copy while the key hashes a worktree's. Edit `pool.py` in a
worktree and its pooled keys miss although the service never ran the edit;
restart the service on new code and they hit although every guest now comes
from code no green run saw.

**The identity.** The service reports a CODE IDENTITY in `hello`: `code`, a
sha256 over each pool-code file's path and digest (`code_identity` over
`service_code_files` in `service.py`) -- every module the process loaded from
its own tree (the `axlpool` package, the agent client
`test/integration/lib/axl_agent.py`) plus every file of the package, which a
later import would load. Taken ONCE, in `Service.__init__`, after the imports:
what the process loaded, which an edit to the checkout afterwards does not
change. It is also in the `started` log record (`axl.code`).

**No protocol bump.** The field is added inside version 2: the contract says
unknown fields are ignored, so an old client keeps working, and a new client
meeting an old service sees no `code` and treats it as UNIDENTIFIED. A bump
would have made every new client refuse the running service until it
restarted -- every pooled test a miss, booted. Unidentified is safe without
it: no pooled key, and a miss under `--expect-code`.

**Asked once per run.** `axlpool code --socket S` prints the identity alone
(exit 0; 1 no service; 3 a service that reports none). The CTest registration
(`cmake/AxlIntegrationTests.cmake.in`, read once per `ctest`) asks it when it
pools and puts `AXL_POOL_CODE_ID=<id>` -- or `AXL_POOL_CODE_ID=` when there is
no answer, set so a stray value in the caller's environment cannot stand in
-- in every pooled guest= test's environment. The key reads it: in pool mode
an empty identity is no key (the test runs); otherwise `pool_code=<id>` is in
the key. `cache_is_fresh` before the run and `cache_commit` after it read the
same variable, so they agree.

**A restart mid-run.** `lib/pool-or-boot.sh` passes `--expect-code
$AXL_POOL_CODE_ID` to `axlpool run`, which compares it with the `hello` on the
very connection it then leases on (one connection is one service process)
and, on a mismatch, misses before leasing -- `AXLPOOL-MISS: the pool service
runs code (<new>) other than this run's (<old>): it restarted, or is older`.
The runner then boots the test (§6.7) and commits a BOOTED key. So a pooled
key naming identity X is only ever committed by a test that ran on X. What it
does not do: re-ask before each SKIP -- a test skipped after the restart was
decided by the identity the run started with, a claim about that code; the
next run asks again and misses. The cost of the mid-run case is boots, not a
wrong answer.

**For the successor (qemu-pool).** REQUIRED: report a code identity in
`hello` that changes whenever the code serving leases changes (a binary's
digest, or a build id), and honour a client's expectation of it before
leasing -- or the axl-sdk cache has no pooled keys at all. The cases are
`test/pool/service-cases.py` (`hello` carries it; `code_identity` changes on
an edit, rename or added file and on nothing else) and
`test/pool/client-cases.py` (`axlpool code`; `--expect-code` against a
matching, a differing and an unidentified service).

## 7. What it buys -- the census (2026-11-03)

`lib/pool-census.py` over one profiled, uncached run per arch (every launch
recorded with its command line: `launch|` records, `lib/profile.sh`), the
classes keyed on what a pooled guest could not change after boot:

| | X64 | AARCH64 |
|---|--:|--:|
| guest launches | 341 | 173 |
| tests that boot | 221 | 122 |
| distinct guest shapes | 68 | 36 |
| top four shapes | **223 launches (65%)** | **123 launches (71%)** |
| their single-shape tests' work | ~5,520 s of 8,144 s | ~4,390 s of 5,620 s |
| tests booting more than one shape | 17 (1,042 s) | 7 |

**The top four shapes are one machine built four ways.** `run-qemu.sh`'s
plain guest (no devices, `-net none`) and `common-test.sh`'s (xHCI + USB
mouse/tablet/hub + RNG) with an e1000, with `-net none`, or with QEMU's
DEFAULT NIC because no `-net` was given. A pool serves them with ONE
standard guest per arch -- the superset (xHCI, USB, RNG, e1000 or virtio-net
on user networking) -- to the extent the tests tolerate the extra devices;
a test that enumerates PCI or USB is the kind that will not. Unifying the two
launchers on that machine is worth doing on its own. (`common-test.sh`'s
AARCH64 guest also carries `virtio-rng-pci` TWICE -- an accidental
duplicate.)

The tail is real variety, not noise: debug OVMF with screenshots (16 X64
launches), virtio-net guests, VGA/GOP guests, TCG crash guests, and the
network-topology tests (the three `test-sshd-start*-qemu.sh` boot six distinct
shapes: two NICs, a link-down NIC, hub-joined NICs, no option ROM). Those
stay `fresh`.

**The saving.** A restore instead of a boot saves ~4.4 s (X64) / ~8.8 s
(aa64) per launch on a quiet box; under the suite's own load boots are
slower, so read these as floors:

| | pooled launches | saved work | wall clock |
|---|--:|--:|--:|
| X64 | 223 | >= ~980 s | **>= ~100 s of 980 (10%), up to ~20% under load** |
| AARCH64 | 123 | >= ~1,080 s | **>= ~110 s of 711 (15%), up to ~30% under load** |

**The cost is per-test conversion, not the pool.** Each of the ~160 tests in
those shapes builds its own disk, writes a `startup.nsh` and reads the serial
log; a pooled test instead stages through the share and runs commands
through the agent. The conversion is mechanical but touches every one of
them, and is where most of the work sits.

Not counted: typing that tests do today over serial at ~10 ms a character,
and QEMU process churn falling from hundreds of launches per run to the pool
size. **The pool is not a 2x.** The rest of the work is test bodies -- fixed
stalls, network waits, timeouts; the profile's top entries
(`test-cpu-spike-qemu.sh` 180 s, the console-device suites, the `sshd` suites
at ~130 s) are that kind, and a pass on them is the other half of a
wall-clock cut.

## 8. Phases

0. **Census -- DONE 2026-11-03** (§7): every launch recorded, the `sshd`
   gap closed, 65% / 71% of launches in one standard guest per arch.
   Reset levels are assigned per test as tests convert.
0b. **One standard guest from both launchers -- DONE 2026-11-03.**
   `common-test.sh` and `run-qemu.sh --bridges` both ask
   `build_qemu_base_cmd ... standard` for `qemu_standard_devices` (root port
   at 00:02.0 with the ONE virtio-rng behind it, xHCI at 00:03.0 with mouse,
   hub and tablet) and take the boot-menu wait from `qemu_boot_wait_args`; a
   test that chose no network LAUNCHES the standard NIC, stated (all three
   launch paths, `stage-lib.sh` included). `test-standard-guest.sh` pins it
   with the census classifier itself and a stub QEMU that records what the
   launchers really start. Two things stayed as they were, deliberately:
   - **A plain `run-qemu.sh` guest is still the bare machine.** softbmc
     launches through it and its PCI goldens pin that layout (the first cut
     put the root port in every guest and would have shifted softbmc's
     devices a slot); uefi-devkit calls `build_qemu_base_cmd` directly.
   - **USB is opt-in there for cost too**: each USB device is ~0.2 s of boot
     (X64 2.25 s plain against 2.78 s with the set; AARCH64 6.7 s against
     7.3 s), and its headless screenshot baselines expect no pointer. The pool
     makes the cost moot for pooled launches, so it is revisited there.
   **Slots are pinned** because removing AARCH64's duplicate RNG moved its
   xHCI from 00:03.0 to 00:02.0 and failed an exact-string `lspci` check;
   pinned, both arches number the standard guest identically.
1. **AXL prerequisites, test-first**: a Shell execute that reports the
   command's status (§2.1), and a first-word resolution query for nested
   Shells (§2.3), and the adaptive network latency mode in
   AxlLoop/AxlNet (§2.2).
1b. **Agent, HTTP transport -- DONE 2026-11-03** (§5.1; one `exec` verb,
   test-first, both arches). The mailbox transport (no-NIC fallback) is
   DEFERRED, not dropped (Mike, 2026-11-03): no pool class here lacks a
   NIC. It is owed -- built in this project when a no-NIC class appears,
   and otherwise a feature of the standalone Rust pool service (§6.2), whose
   other consumers (SoftBMC, axl-webfs, AGT) may boot guests without one.
2. **Pool manager + CTest resource groups**, with the X64 plain class; move
   the single-boot plain tests over and measure the wall clock against §1.
3. Remaining classes, AARCH64, and `verify.sh`'s unit runs.
4. **`sshd` exec** over the agent (an AxlSsh feature: OpenSSH parity).

## 9. Decisions open

Saved for the Rust service's design (Mike, 2026-11-03; §6.4):

- **An all-Rust project, including the launcher** (Mike, 2026-11-04):
  `run-qemu.sh` stays, and stays valuable, in axl-sdk, but the Rust
  project builds its OWN QEMU launcher in Rust rather than calling it --
  the script has grown unwieldy for bash. The behaviour to carry over is
  what the pool depends on: the standard devices (`--bridges`), `--qcow2`
  and `--state-dir`, `--hostfwd-bind`, the dry run with its
  `AXL_DRYRUN_TMPDIR` / `_STAGED` / `_INPUT` facts (the class key), the
  `QEMU_PID`/`VIRTIOFSD_PID`/`HOSTFWD_*` report, virtio-fs, the -nonest
  launcher and the locked host-port allocator.

- **QMP in the Rust project** (Mike, 2026-11-04): QMP over Unix sockets
  is the only control channel, through
  [qapi-rs](https://github.com/flatkvm/qapi-rs) (types generated from
  QEMU's own QAPI schema; tokio) behind a thin trait of our own, since its
  release cadence is modest. The guest monitor is event-driven (§6.6), on a
  persistent connection per guest. Tests do NOT get the guest's QMP socket:
  the service holds the only connection and offers a narrow proxied subset
  (key and pointer input, `set_link`, screenshots) through its API, so a
  test cannot `quit` or reconfigure its guest, and nothing contends with the
  event connection. (axl-sdk's Python pool still hands `AXL_POOL_QMP` to a
  trusted in-repo suite.)

- **Result storage:** JUnit files per run (recommended first), or a
  KCIDB-like store across runs -- history and flaky-test detection are
  what would justify the store.
- **Telemetry export:** JSON lines only (recommended until a collector is
  worth running), or OTLP export to an OpenTelemetry collector as well.

Earlier:

- ~~Where the pinned agent comes from~~ -- decided: each arch's build in
  the service's own tree (§6.5).
- Whether `sshd`'s launcher starts the agent itself, so `ssh host cmd` works
  after one `sshd` command.
