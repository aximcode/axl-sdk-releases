# AXL `netload` — interactive NIC-driver loader + link/DHCP probe

Design doc. Status: **implemented** (2026-07-15); dependency-driver
co-load added 2026-07-15 (see "Driver dependencies"); end-of-sweep
findings report added 2026-07-15 (see "Findings report"); `--save`/
`--apply` working-config persistence added 2026-07-16 (see "NVRAM
state" and "Modes"); `--json` machine-readable result object +
`--retries` surfacing added 2026-07-16 (see "Modes"); the firmware-first
probe's result put into the `-a` summary as an always-present row 0
added 2026-07-16 (see "Findings report").

## Purpose

Bring-up diagnostics for network drivers on real hardware (Dell PowerEdge / iDRAC
in particular). A field engineer needs to, over a **flaky KVM/virtual console**:

1. See the NIC drivers staged in the devkit image and **select one with a single
   keypress** (typing full paths on a bouncing virtual console is unreliable).
2. Load that driver, bind it, and see **whether a NIC came up and its link is up**.
3. Get an **address** — DHCP (default, 15 s wait, tunable via `--dhcp-timeout`) or
   a **static IP** (`--ip`/`--mask`/`--gw`/`--dns`) when the segment has no DHCP
   server (a real, common case: a NIC that links but never leases).
4. **Verify it's really up** — optionally `--ping` / `--ping-gw` / `--resolve`, so
   "networking is up" means *reachable*, not merely *configured*.
5. Do this **one driver after another**, or let an **auto mode** try them all.

The goal is an **active, verified IPv4 connection**; the flaky console is why
every option also has a **single-char short form** and why the pick step is one
keypress. The sharp requirement: **one of the staged drivers hangs / faults the box on
load.** The tool must record *which driver it is about to load* in a place that
**survives a crash**, so that after the reboot the culprit is known and skipped.
The devkit FAT volume is **read-only**, so the only crash-surviving store is
**UEFI NVRAM**; verbose output goes to the **screen**.

`netload` is a diagnostic sibling of `netinfo` and `rndisfix`.

## Non-goals

- Not a general replacement for `net-init.nsh`'s bulk `load` + `connect -r`
  (that stays as the "bring the whole stack up at once" path). netload *does*
  now handle the dependency-driver case for a probed NIC (see "Driver
  dependencies") so USB-RNDIS/CDC adapters come up under the single-key
  probe — but it still brings up **one NIC at a time**, not the entire staged
  set together.
- No writing to the (read-only) filesystem — **no log file**.
- Not a general traffic/throughput tester. netload *does* verify **reachability**
  (`--ping` / `--ping-gw` / `--resolve`) to confirm a link is *actually* up —
  because the whole point is an **active, verified IPv4 connection**, not merely a
  configured one — but for HTTP fetches or sustained traffic use `fetch` / a full
  `netinfo` afterward.
- Not a driver *fixer* — when it detects the known RNDIS data-plane stall it
  *points at* `rndisfix`, it does not apply the fix itself.

## Home & packaging

- New axl-sdk tool: **`tools/netload.c`**, built like every other tool
  (`AXL_TOOL_MAIN`, so it gets the version stamp and the `-b`/`-h`/`--version`
  framework flags for free).
- Staged into the uefi-devkit image next to the other tools; run from the shell.
- **Always launched manually** from the shell (`netload` / `netload -a`) — it is
  deliberately **not** wired into `startup.nsh`. After a crash-reboot the operator
  re-types the command (once), and the NVRAM quarantine makes that re-run resume
  past the crasher. (The single-key menu keeps the *only* painful typing to that
  one launch command.)

## Driver discovery

- Resolve the **volume the tool was launched from** (via `EFI_LOADED_IMAGE`
  `DeviceHandle`; axl helpers: `axl_app_image_path()` / the path/volume utilities).
- Scan **`<that-volume>:\drivers\<arch>\*.efi`** — the exact directory
  `net-init.nsh` iterates (`drivers\%arch%\*.efi`), where `<arch>` ∈ {`x64`,
  `aa64`} is fixed at build time (the tool binary is arch-specific).
- If `drivers\<arch>\` is absent, fall back to `<vol>:\drivers\`.
- Result: a **sorted list of `.efi` basenames**. That IS the menu. We cannot tell
  a NIC driver from a filename, so we list them all; "did it actually produce a
  NIC" is answered *after* load by the interface diff (below). The one exception:
  drivers the optional dependency sidecar marks as pure **dependency drivers** are
  filtered out of the pick menu / sweep (see "Driver dependencies").
- The sort is alphabetical **except** any candidate `axl_net_driver_is_ipxe()`
  recognizes (a filename heuristic — "ipxe" as a case-insensitive substring)
  always sorts last, regardless of its name. iPXE's `LoadImage` hook breaks
  every later `.efi` load in the same session, so a plain alphabetical sweep
  that happens to hand it out early poisons every driver after it — it must
  be the last thing the sweep ever tries. netload also disarms iPXE's
  5-minute boot-services watchdog immediately after starting a recognized
  driver (one choke point, `driver_load_start()`, shared by the sweep, the
  dependency co-loader, and `--apply`) — otherwise the box resets minutes
  into an otherwise-healthy run. See `axl_net_driver_is_ipxe()` in
  `axl/axl-net.h` for the full rationale; it's the same recognition
  `axl_net_try_driver()` uses internally, exposed for callers like this one
  that run their own load loop instead.

## NVRAM state (the only crash-surviving record)

All under one vendor GUID, via `axl-nvstore` (`SetVariable`/`GetVariable`,
**non-volatile + bootservice**, synchronous → committed to SPI flash before the
call returns).

On flash these are the keys `Trying` / `Quarantine` / `Log` / `Config` under the
netload vendor GUID (a `dmpstore` on real hardware shows them without a `Netload`
prefix); the `Netload`-prefixed names below are the doc's readable labels.

| Variable | Contents | Lifetime |
|---|---|---|
| `NetloadTrying` (`Trying`) | the single driver basename about to be loaded (the **breadcrumb**) | set immediately *before* each load, deleted immediately *after* a survived load |
| `NetloadQuarantine` (`Quarantine`) | `\n`-separated basenames known to have crashed the box | grows across boots; cleared by `--clear` |
| `NetloadLog` (`Log`) | a **bounded ring** of one-line results: `RESULT driver` (e.g. `OK Aaa.efi`, `NONIC Bbb.efi`, `CRASH Ccc.efi`) | appended after each attempt; cleared by `--clear`; dumped by `--dump` |
| `NetloadConfig` (`Config`) | one bounded line: `driver\|method\|ip\|mask\|gw\|dns\|mac` (`method` is `dhcp`/`static`; static fields empty on the DHCP path; `dns` is empty, one, or two comma-joined addresses; `mac` is the winning NIC's) — the **last known-working config** | written by `--save` on a real win; read by `--apply`; cleared by `--clear`; shown by `--dump` |

Size discipline: `NetloadTrying` and `NetloadConfig` are one short bounded line
each; `NetloadQuarantine` and `NetloadLog` are bounded (truncate oldest) so the
NV store budget is never stressed. If any `SetVariable` fails (read-only / full
NV store), `netload` warns on-screen and **continues** — the breadcrumb (and the
saved config) are best-effort-durable, never a hard dependency.

## Crash recovery (the point of the tool)

On **every startup**, before showing the menu:

1. Read `NetloadTrying`. If **present**, the previous run **died loading it**:
   - Print, loudly: `⚠ last run CRASHED while loading <X>.efi — quarantining it`.
   - Append `<X>` to `NetloadQuarantine`, append a `CRASH` line to `NetloadLog`,
     delete `NetloadTrying`.
2. Print the current quarantine list so the operator sees the running suspect set.

Because a quarantined driver is skipped by auto mode and flagged in the menu,
**re-running `netload` (or `netload -a`) after a crash-reboot resumes past the
crasher** — an automatic bisection of the bad driver, one manual re-launch per
crash.

## Modes

- **Interactive (default)** — draw the menu; wait for one keystroke
  (`axl_console_read_key`, no line input, no path typing):
  - `1`–`9`, `a`–`z` → load that driver and run the probe sequence.
  - `A` → auto mode (load-all sweep).
  - `F` → firmware-first probe (try the firmware's own NIC drivers, no staging —
    see "Firmware-first probe"). Uppercase so lowercase `f` stays a driver pick.
  - `r` → redraw the menu (recover from a bounce-garbled screen).
  - `q` → quit.
  Quarantined drivers render dimmed / tagged `[crashed]` and require a confirm
  keypress to retry.
- **`-a` / `--auto`** — iterate every non-quarantined driver in order, running the
  full probe sequence on each, logging throughout. **Stops as soon as any
  interface comes up and passes verification** (a DHCP lease, or a static address
  that answers a requested `--ping`/`--resolve`) — that is the goal, so it reports
  the winning driver + interface + IP and exits, leaving that driver loaded. A
  driver that links but never leases, or leases but is unreachable, is logged and
  the sweep **continues** to the next driver. Hands-off once launched. After the
  firmware-first probe comes up dry and *before* the staged sweep, `-a` tries a
  saved `NetloadConfig` (if one exists) first, best-effort — see `--apply` below.
- **`-l` / `--list`** — print the discovered drivers (with `[crashed]` tags) and
  exit; no loading. Just shows what the volume staged. **User-facing.**
- **`-p` / `--probe <driver>`** — run the full probe sequence on exactly one named
  driver and exit (skip the menu) — the one-shot form of a menu pick. Exits
  non-zero if the driver does not bring a **reachable** interface up.
  **User-facing.**
- **`-u` / `--dump`** — print `NetloadQuarantine` + a **findings table
  reconstructed from `NetloadLog`** (each `RESULT driver` line rendered as a
  colored row, the `SWEEP` line as a summary) + the raw log + the saved
  `NetloadConfig` line (if any), then exit. The log reconstruction is the point: a
  sweep that **crashed or hung** before its on-screen summary printed still
  yields a legible findings table after the reboot, because the per-driver log
  lines were committed to NVRAM as each probe finished.
- **`-c` / `--clear`** — delete all netload NVRAM variables (including
  `NetloadConfig`) and exit.
- **`-f` / `--connect`** — the **firmware-first probe** standalone: `connect -r`
  the firmware's own NIC drivers and try DHCP, with **no staging** (see
  "Firmware-first probe"). Exit 0 on a lease.
- **`-s` / `--save`** — on a real win in interactive mode, `-a`, or `--probe`,
  persist the winning driver + its working method/static params/MAC to
  `NetloadConfig` (best-effort; a write failure warns and does not fail the
  run that just won). A firmware-first win (no staged driver involved) is
  never saved — there is no driver file for `--apply` to reload.
- **`-y` / `--apply`** — re-apply the saved `NetloadConfig`: load its driver
  (breadcrumbed, like a normal probe) + declared dependencies, resolve the
  saved MAC to a live interface, bring it up with the saved method/static
  params, and verify — **skipping discovery and the sweep entirely**. An
  explicit `--apply` runs **first**, before any firmware probe or staged
  sweep. Exits non-zero (with a clear message) if nothing was ever saved, the
  saved driver fails to load, the saved NIC isn't present, or verification
  fails.
- **`-N` / `--no-deps`** — disable dependency co-load (see "Driver dependencies"):
  probe every driver strictly standalone, the pure
  one-driver-at-a-time isolation behavior. Useful when narrowing down which
  driver crashes the box.
- **`-D` / `--dir <path>`** — override the driver directory. **`-v` / `--debug`** —
  verbose (DEBUG) logging.
- **`-o` / `--out <FILE>`** — **tee all of netload's output to `FILE`** (e.g.
  `netload -u -o FS0:\netload.txt`) via `axl_stream_set_stdout_tee`, in addition
  to the screen. The whole reason: on a bouncing KVM/iDRAC console the scrollback
  is unreadable, so the operator reads the file instead. Works with every mode
  (`-a`, `-u`, `-d`, …). Best-effort — a read-only volume / bad path warns and
  continues screen-only. Closed on every exit path (`axl_atexit`); `--diag`/
  `--dump` release it early so the shell can append their dumps to the same file.
- **`-d` / `--diag`** — a **diagnostic report**. It **composes** with an action:
  `netload -a -d` runs the DHCP sweep *and then* dumps diagnostics (so the
  `drivers` list reflects what the sweep just bound); `-d` alone runs the report
  standalone. It reports netload's own passive *network landscape* (every
  interface's MAC / link / binding layer / bound-driver name, read-only — no
  connect, no DHCP), then the UEFI **shell's own `drivers`** list
  run via `EFI_SHELL_PROTOCOL.Execute` (the exact, battle-tested output, not a
  reimplementation). Pair with `-o` to capture it to a file. Not launched from a
  UEFI shell → the shell dump prints a clear "unavailable" notice and the
  landscape still prints. `--dump` likewise appends the shell `drivers` list
  after its NVRAM findings, so a post-reboot `--dump` shows both what netload did
  *and* what actually bound.
- **`--dh`** (with `--diag`) — also run the shell's **`dh -v`** (verbose handle
  dump: every handle's protocols + device paths), but to the **screen only** —
  it is *not* tee'd to `--out`. **Why the asymmetry:** driving a full `dh -v`
  through the shell's file-redirect (`>>a FILE`) faulted a real Dell PowerEdge's
  `Shell.efi` (an RSOD) — while the same call runs clean under OVMF and typing
  `dh -v` manually on that box works. Since the crash is inside firmware we
  can't fix and can't reproduce in QEMU, netload never redirects `dh -v`;
  `--dh` prints it to the console (the confirmed-safe path) with a note, and an
  operator who needs it in a file can run `dh -v >FILE` manually. (`drivers` —
  small, and proven safe on that box — *is* redirected into `--out`.)
- **`-j` / `--json`** — for automation: once the outcome of a `--probe`, an
  `-a` sweep, or an interactive pick is known, append **one** machine-readable
  result line to stdout (in addition to, not instead of, the live progress
  logging above — an operator watching a flaky serial console still needs
  those). Only the *decorative* end-of-sweep findings tables
  (`print_summary`'s per-driver table + recommendations) are suppressed; the
  `SWEEP` NVRAM log line is still written, same as without `--json`. Nothing
  is printed if the mode never produced a report (e.g. quitting the menu
  without probing anything). Shape:
  ```
  {"driver":"Rtk.efi","method":"dhcp","ip":"10.0.0.5","mask":"","gw":"","link":true,"retries":1,"result":"up"}
  ```
  `method` is `"dhcp"`/`"static"`; `ip` is the address actually assigned
  (empty if none ever was — never fabricated); `mask`/`gw` are populated only
  for the static path (`DriverReport` does not capture the DHCP lease's
  subnet/gateway, so DHCP emits empty strings there rather than a value the
  object can't vouch for); `retries` is the *configured* `--retries` budget
  (retries allowed, not consumed); `result` is `up`/`noreach`/`no-lease`/
  `none` (not `leased` — a static win never leases anything). A
  `"ping":{"target":...,"rtt_ms":...,"ok":...}` object is appended only when
  an explicit `--ping TARGET` ran (`--ping-gw`/`--resolve` are not folded
  in). On a multi-driver `-a` sweep the object describes the winner (or,
  absent a win, the last *attempted* driver — trailing quarantined/skipped
  candidates are stepped over so their zero-initialized row never emits a
  false `"result":"up"`; since `reports[0]` is always the firmware-first
  probe's row and is never marked skipped, this walk-back always lands on a
  real row — even a sweep with no staged drivers at all still has the
  firmware attempt to report). A win via the firmware-first probe launched
  from the `-a` sweep now emits a `--json` object too (`reports[0]`,
  `"driver"` = the resolved firmware driver name or `"(firmware)"`) — but
  three other win paths never build a `DriverReport` and so emit no
  `--json` object at all: the saved-config retry (`--apply` / `cmd_apply`,
  which precedes the staged sweep), interactive `F` (`probe_firmware_stack
  (cfg, NULL)`), and standalone `--connect` (`probe_firmware_stack(&cfg,
  NULL)`). Script against `--probe`/interactive numeric picks/`-a` (staged
  sweep or its firmware-first row) for guaranteed JSON output; the
  saved-config retry, interactive `F`, and standalone `--connect` are the
  JSON-silent win paths.
- **`-R` / `--retries N`** — retry a NIC that links up but never gets an
  address (`PR_LINK_NO_DHCP`) N times before giving up on it (default 1; an
  explicit `0` is rejected). Applies per NIC inside `bring_up_and_verify`, so
  it stacks with `-a`'s per-driver iteration. Reflected in the `--json`
  object's `retries` field.

**Every** flag has a single-char short form (the bouncing-console mitigation for
typing, mirroring the single-key pick): see "Static config & verification" below
for the config/selection/verification flags and their shorts.

The probe modes (interactive, `-a`, `--probe`) auto-load a probed NIC's
dependency driver(s) on demand when the dependency sidecar declares them;
`--list` / `--dump` / `--clear` never load anything.

Three hidden seams exist only for the headless QEMU test and are not part of the
user-facing surface: `--_mark <name>` (seed the crash breadcrumb), `--_key <char>`
(resolve one menu key), and `--_log <TOKEN> [--_logname <name>]` (seed one NVRAM
result-log line, to exercise the `--dump` label rendering). `axl-args`'s
`AxlArgDesc` has no
`.hidden` field to omit a flag from `--help` outright, so these seams do
appear there — each tagged `TEST SEAM:` in its help text, and all grouped
last in the flags array (after every user-facing flag) so the generated
`--help` still reads coherently top-to-bottom as "real flags, then the
test-only knobs."

## Static config & verification

The tool's goal is an **active, verified IPv4 connection**, so on top of the
default DHCP bring-up it accepts a static configuration, a NIC selector, and
reachability checks. All flow through the single per-interface bring-up call
(`axl_net_bring_up` inside `bring_up_and_verify`), so they apply no matter how
the NIC came up (staged driver *or* firmware-first). **Every flag has a
single-char short** (the bouncing-console mitigation):

- **IP configuration** — `-i` / `--ip A.B.C.D[/N]` (static IPv4; accepts a `/N`
  CIDR prefix), `-m` / `--mask M` (netmask when no `/N`; default
  `255.255.255.0`), `-g` / `--gw G` (default gateway), `-e` / `--dns S[,S2]`
  (one or two resolvers, programmed via IP4Config2), `-H` / `--dhcp` (force DHCP,
  the default), `-t` / `--dhcp-timeout N` (DHCP wait seconds, default 15; an
  explicit `0` is rejected).
- **NIC selection** — `-M` / `--mac XX:XX:XX:XX:XX:XX` and `-n` / `--nic N` pick
  one NIC (mutually exclusive). A **static IP requires a selector** (`--mac` /
  `--nic`, or a single `--probe` / menu pick): `--ip` under a blind `-a` sweep is
  a hard error, since applying one address to every link-up NIC is wrong on a
  multi-NIC server. Under `-a`, a selector also skips any produced NIC that does
  not match it.
- **Verification (gates success)** — `-P` / `--ping TARGET`, `-G` / `--ping-gw`
  (ping the learned/`--gw` gateway), `-r` / `--resolve NAME`. When any is
  requested and fails, the interface is reported **`up, no reach`** (the
  `PR_NO_REACH` outcome, NVRAM token `NOREACH`), *not* a win — `--probe` exits
  non-zero and an `-a` sweep continues to the next driver. This is what makes
  "networking is up" mean *reachable*, and it directly catches the RNDIS
  "link up, lease OK, no data plane" class.

Config-bearing flags are validated up front (`netload_cfg_parse`): a malformed
address/mask/gateway/DNS/timeout/retries, `--mac` together with `--nic`, or a
static IP with no selector under `-a`, each fails fast with a specific message
before any driver is touched.

## Firmware-first probe (try the firmware's own drivers)

netload's staged-driver sweep only DHCPs interfaces its *own* drivers *newly*
produce (the before/after diff), and treats anything already present as
"pre-existing" — so a NIC the **firmware** already brought up (or that just needs
a `connect -r` to bind a loaded-but-unconnected firmware driver) is **invisible
to the sweep**. On real hardware the `drivers` shell command often lists resident
network drivers; one of those may be all networking needs.

So `-a` (and the standalone `--connect`, and the interactive `F` key) begins with
a **firmware-first probe** — using only public axl-net/axl-driver APIs:

1. `axl_driver_connect(NULL)` (== `connect -r`) — bind any firmware NIC driver
   that is loaded but not yet connected.
2. `axl_net_list_interfaces()` — enumerate **every** present interface (not a
   diff), and for each report MAC / link / binding layer / bound-driver name
   (`axl_net_get_driver_info`, the same NII→SNP walk `netinfo` uses). This is the
   network **landscape**: what the firmware already offers.
3. DHCP each link-up interface (`axl_net_bring_up`, 15 s). A lease here =
   **networking is up via a firmware driver, with zero staging** — reported as
   such and the sweep stops before touching any staged driver.

Only if the firmware path comes up dry does `-a` fall through to the staged
sweep. This closes the gap where a working firmware NIC was never even
DHCP-probed. (A real firmware-NIC lease is hardware-only; QEMU with no `--net`
has no NIC, so the probe reports "no firmware NIC driver bound" — which is itself
the useful signal that a driver must be staged.)

Under `-a`, the probe's outcome always becomes `reports[0]` in the end-of-sweep
findings report — including on a win, where `cmd_auto` used to return 0
silently without printing anything (see "Findings report").

## Driver dependencies (co-load)

Some NIC drivers produce a working interface only when a **dependency driver** is
co-resident. The verified case: `UsbRndis.efi`, `UsbCdcEcm.efi`, and
`UsbCdcNcm.efi` each bind a USB function and publish the EDK2
`gEdkiiUsbEthernetProtocolGuid`; `NetworkCommon.efi` is the sole driver that
consumes it and publishes NII, on top of which the firmware's SnpDxe makes the
SNP. Probed alone under netload's load-one-then-unload loop, those three produce
no NIC (`NO_NIC`) — yet they are exactly the adapters (USB-RNDIS / CDC BMC
pass-through NICs) a field engineer most often needs on an iDRAC/KVM console.
(The self-contained drivers — `RtkUndiDxe`, `RtkUsbUndiDxe`, `AsixUsbUndiDxe`,
`ipxe-*` — publish NII directly and need no dependency.)

netload learns these edges from an **optional JSON5 sidecar** staged next to the
drivers, `<driver-dir>\netload-drivers.json5`:

```json5
{
    schema: 1,
    // Each entry: a NIC driver and the dependency driver(s) it needs
    // co-resident to produce a NIC. Drivers not listed here are
    // self-contained. Arch is implicit — this file lives in drivers/<arch>/.
    drivers: [
        { name: 'UsbRndis.efi',  requires: [ 'NetworkCommon.efi' ] },
        { name: 'UsbCdcEcm.efi', requires: [ 'NetworkCommon.efi' ] },
        { name: 'UsbCdcNcm.efi', requires: [ 'NetworkCommon.efi' ] },
    ],
}
```

The map is a **dependency tree**: a `requires` entry may name a driver that is
itself a `{ name, requires }` entry, so dependencies can nest. netload resolves
the tree **transitively and deepest-first** — probing a NIC brings up its
dependencies' dependencies before the dependency, before the NIC. Resolution is
cycle-safe: a dependency is marked "attempted" *before* its subtree is walked, so
a `A → B → A` cycle terminates. (For the current nine drivers the tree is only
one level — `UsbRndis → NetworkCommon → [firmware SnpDxe]` bottoms out in firmware
netload cannot stage — but the resolver handles arbitrary depth for driver sets
that need it, up to `NETLOAD_MAX_DEPS` distinct nodes.) The sidecar is an **array
of `{ name, requires }` objects** (not an object keyed by driver name) to match
the JSON reader (which iterates arrays, not dynamic object keys) and every other
axl-sdk sidecar. The image owner authors and ships it (uefi-devkit stages it into
`drivers/<arch>/`); the axl-sdk tool stays driver-agnostic — **no NIC names are
baked into netload**. If the file is **absent**, netload behaves exactly as
before (no dependencies, every driver probed standalone); a parse error is warned
and treated as absent.

**Classification.** A driver that appears as a `requires` value of *any* entry is
a **dependency driver** — even a **mid-tree node** that also appears as a `name`
(a dependency with its own dependencies): being required by something means it is
auto-loaded, not a menu pick. Dependency drivers are excluded from the pick menu
and the `-a` sweep (picking `NetworkCommon` alone is pointless — it makes no NIC of its
own) and shown instead as an informational line. Every other scanned `.efi` is a
NIC candidate.

**On-demand co-load (keeps the crash blast-radius minimal).** Dependencies load
only when a NIC that needs them is actually probed — not unconditionally at
startup. Immediately before probing NIC `X` in any probe mode:

1. Resolve the dependencies `X` requires. For each one **not already resident** and
   not quarantined: set the breadcrumb to the *dependency's* name, `load` +
   `start` it, clear the breadcrumb, and keep it **resident for the rest of the
   session** (reused by later probes, never unloaded). Each dependency is thus its
   own individually breadcrumbed step — a crash still pins exactly one driver.
2. Then run the normal probe on `X` (snapshot-before → `load`/`start`/`connect` →
   snapshot-after diff). A dependency driver produces no NIC of its own, so it adds
   no MAC between the snapshots and the diff still attributes the NIC to `X`. The
   broad `connect` after `X` loads is what drives `NetworkCommon`'s binding onto
   the freshly-published UsbEthernet protocol.
3. A non-winning `X` unloads **only `X`**; resident dependencies stay (the next NIC
   probe reuses them; a winning `X` needs its dependency to stay bound).
4. If a required dependency is **quarantined** (crashed a prior run) or missing
   from the directory, netload skips it and warns **precisely** — naming the
   NIC(s) that may not come up — then probes `X` anyway (it may still be
   self-contained on this box).

`--no-deps` disables dependency loading entirely, restoring pure
one-driver-at-a-time isolation.

**A `no NIC` outcome only blames a dependency under `--no-deps`.** In the
default (auto-load) mode every declared dependency was *already* co-loaded
before the probe, so a driver that still produces no interface means **no
matching hardware** — netload says exactly that and does *not* imply a missing
dependency. Only under `--no-deps` (where dependencies are deliberately not
loaded) does the `no NIC` message point at an un-loaded dependency as the likely
cause.

**The RNDIS connect stall (why netload warns before connecting a
dependency-dependent NIC).** When a dependency is resident, the broad `connect`
after the NIC loads drives `NetworkCommon`'s bind, which queries the device's
MAC and max-bulk-size *at bind time*
(`NetworkCommon/DriverBinding.c` → `UsbEth->UsbEthMacAddress` →
`GetUsbEthMacAddress` → `RNDIS_QUERY_MSG`). On a real RNDIS device those queries
are sent **before** `RNDIS_INITIALIZE_MSG` (which the EDK2 driver only sends
later, at `SNP.Initialize` time), so the device — per the RNDIS spec — ignores
them, and each `RndisControlMsg` spins its full bounded `RNDIS_CONTROL_TIMEOUT`
(~10-23 s, no early-exit on error). Two queries → **~30-60 s of frozen console**
during connect. It is *bounded* (every loop terminates), not a true hang — but
30-60 s of dead console reads as one, tempting a power-cycle. So before the
connect of a dependency-dependent NIC, netload prints a "this can take up to
~60 s; do NOT reset the box" notice. (The underlying query-before-initialize
ordering is an EDK2 UsbNetworkPkg defect, not netload's; netload only warns and,
if the box is genuinely wedged, quarantines via the breadcrumb.)

**Both crown jewels are preserved:** exactly one driver name is ever in
`NetloadTrying` across any single `load` (dependency loads are separate
breadcrumbed steps), so a hang still pins one culprit; and the operator still
selects with one keypress — dependencies load automatically, no extra typing. No
new NVRAM keys are introduced (the bounded `Quarantine` list already gates
dependency loads).

## Findings report (end-of-sweep summary)

An auto sweep (`-a`) scrolls a lot of per-driver output past a bouncing console;
the important facts — which driver linked up, which produced no NIC, what to do
next — are exactly what scrolls off. So the sweep ends with a **findings
report** that re-states everything in one screen:

- A **per-driver outcome table**: one row per driver with its outcome
  (`LEASED` / `LINK, no lease` / `no NIC` / `load failed` / `skipped`) and a
  detail column carrying the representative NIC's MAC, binding layer, and — for
  a winner — the leased IP. Co-loaded **dependency drivers** get their own rows
  ("co-loaded for X, Y"). Rows are **color-coded** (green win, yellow
  link-no-lease, red fail/skipped, dim no-NIC, cyan dep) via the public
  `axl_console_set_color()` / `axl_console_reset_color()` API — a silent no-op
  on a monochrome console, so the text always prints.
- **`reports[0]` is always the firmware-first probe's result** (see
  "Firmware-first probe") — never omitted, even on a firmware win. Its outcome
  label carries a `firmware:` prefix (`firmware:LEASED` /
  `firmware:LINK, no lease` / `firmware:no NIC`) so it reads distinctly from a
  staged-driver row, and its detail column uses wording that fits "nothing was
  staged": `no firmware NIC bound` (no interface at all, or none matching
  `--mac`/`--nic`) and `linked, no lease` (in place of a staged row's
  `loaded, no interface ...` / `up, no DHCP in 15s`, neither of which applies —
  nothing was loaded). This is what lets an operator tell, from the summary
  alone, whether the built-in path was even tried and what it found — a
  firmware win used to make `cmd_auto` return 0 with no summary at all. When
  more than one firmware NIC is present, the row describes ONE representative
  NIC consistently (best outcome wins: leased > unreachable > link-no-lease),
  with `mac`/`name`/`layer`/`ip`/`result` all drawn from that same NIC.
- A **count line**: `N drivers: A leased, B linked-no-lease, C unreachable, D
  no-NIC, E load-fail, F skipped (+G deps)`. **The firmware-first row is shown in the
  table above but is excluded from these staged-driver counts** (and from
  `N` — it is not a staged `.efi` driver) and from the NVRAM `SWEEP`
  breadcrumb's counts, so the numbers describe only the drivers actually
  swept. A firmware NIC that brought networking up is still reflected in the
  bottom-line outcome (it is a real win), just not tallied as a staged driver.
- The **bottom-line outcome**: `NETWORKING IS UP via <driver> (<ip>)` (the
  `<driver>` is the firmware NIC's resolved driver name on a firmware win), or
  `NO DHCP LEASE`.
- **Actionable next steps**, most useful first: a link-up-no-DHCP driver → the
  exact `rndisfix` + `netload --probe <driver>` to run; a count of no-NIC
  drivers (missing dependency / no matching HW); the quarantined drivers skipped
  this sweep + the `netload --clear` to retry them; or, if nothing linked, a
  cabling / staged-driver hint.

The report also prints on **quit from the interactive menu** (summarizing the
drivers the operator probed), and a one-line `SWEEP …` result is appended to the
bounded NVRAM `NetloadLog` ring so `--dump` after a reboot still shows the last
sweep's bottom line. A single `--probe <driver>` prints no report — its live
output already *is* the report for one driver. (The interactive menu's `F` key
and the standalone `--connect` also run the firmware-first probe, but neither
builds a `reports[0]`-style row today — each already prints its own direct
one-line outcome, so there is no summary table for a firmware row to join.)

## Per-driver probe sequence (the crash-safe core)

For a selected driver `X.efi` at `<path>`:

0. **(dependency co-load, if the sidecar declares any and `--no-deps` is off)**
   Load `X`'s not-yet-resident dependency driver(s) first — each its own
   breadcrumbed `load`/`start`, kept resident. See "Driver dependencies".
   Skipped silently when `X` has no declared dependencies.
1. `SetVariable(NetloadTrying = "X.efi")`  → console: `▶ loading X.efi …`
   *(breadcrumb is now durable in NVRAM)*
2. **`axl_driver_load(<path>) → axl_driver_start() → axl_driver_connect(NULL)`**
   — load, run the driver's entry (installs its DriverBinding), then a broad
   connect (the `connect -r` equivalent) so the firmware's SnpDxe/MnpDxe binds on
   top. **If the driver hangs / faults, the box dies here — and `NetloadTrying`
   already names it.**
3. Survived → `DeleteVariable(NetloadTrying)`; console: `✓ loaded`.
4. **Interface diff.** Snapshot `axl_net_list_interfaces()` (by MAC) *before* step
   2 and *after* step 2; the new MACs are the NIC(s) this driver produced.
   - No new interface → `loaded, but no NIC came up` (the driver may need a
     dependency, e.g. `UsbRndis` needs `NetworkCommon`, or no matching hardware).
     Record `NO_NIC`, unload, next.
5. **Per new interface**, log the full picture (reusing `netinfo`'s accessors):
   - `axl_net_get_driver_info(mac)` → binding layer (NII3.1/NII/SNP), driver name,
     bus location.
   - `axl_net_get_link_stats(nic)` → **link up/down + speed**.
   - MAC / MTU / any existing IPv4.
6. **DHCP** on each **link-up** interface: kick DHCP, **wait 15 s** (the
   per-NIC bring-up API with `dhcp_timeout_sec = 15`).
   - **Lease acquired → this is the win.** Log the driver + interface + leased
     IP / mask / gateway, record `OK`, and **stop**: return success immediately in
     `--auto` (leaving the driver bound and the interface up); in interactive mode
     announce success and return to the menu. No further drivers are tried.
   - **No lease in 15 s → log `LINK_NO_DHCP` and continue** to the next link-up
     interface, then the next driver.
7. **Diagnostic heuristic (the rndisfix lesson):** if an interface is **link-up
   but got no DHCP lease**, print:
   `link is up but no DHCP lease — the data plane may be stalled (e.g. the EDK2
   UsbRndis packet-filter bug). Try 'rndisfix', then re-probe.`
8. `axl_driver_unload()` (best-effort) — **only when moving on** (no lease). The
   **winning driver is left loaded and bound** so its link/lease survives. If a
   non-winning driver refuses Unload (known: some axl/EDK2 NIC drivers don't
   implement it), warn that a `reset` may be needed only before retrying a
   *conflicting* driver on the same NIC; distinct NICs coexist fine.
9. Append a one-line `NetloadLog` result (`OK`/`LINK_NO_DHCP`/`NO_NIC`/`LOAD_FAIL`).

## Logging

- **Screen is verbose and live** (the FAT is read-only): every step above prints,
  with the driver name always present so a scroll-back read tells the story. Uses
  `axl_printf` + the module logger. (Per-line wall-clock timestamps were considered
  and dropped — the breadcrumb already pins the crash culprit, and stamping the
  bounded NVRAM ring would roughly halve its depth for marginal forensic value.)
- **NVRAM is the crash-surviving summary**: breadcrumb + quarantine + the bounded
  result ring, retrievable with `--dump` after a reboot.
- **All on-screen strings are ASCII** (UEFI text console glyph set + the repo's
  `check-ascii` gate). The `▶`/`✓`/`⚠` in this doc are illustrative; the tool
  prints ASCII equivalents (`>`, `[ok]`, `!!`, `[crashed]`).

## Testing

Per axl-sdk test-first, split by what QEMU can actually exercise:

- **Unit / QEMU-smoke (real coverage):**
  - driver-directory scan → sorted menu list (fixture dir of `.efi` stubs).
  - menu index (`1..9`,`a..z`) → path mapping; redraw; quit.
  - NVRAM state machine: breadcrumb set/clear, quarantine append/dedupe/bound,
    log ring bound, `--dump` / `--clear`.
  - **crash-recovery**: pre-seed `NetloadTrying`, run startup → asserts the driver
    is quarantined and the breadcrumb cleared. (Simulates the crash without
    crashing.)
  - interface-diff logic against a stub `list_interfaces` (before/after sets).
  - **dependency co-load, structurally** (dummy `.efi` + a fixture
    `netload-drivers.json5`): a dependency driver is filtered out of `--list`/menu
    and tagged; probing a NIC with a declared dependency loads the dependency
    **first** (its breadcrumb, then the NIC's) and reuses it on a second probe;
    `--no-deps` suppresses the dependency load; a **quarantined** dependency is
    skipped with the precise "may not come up" warning; an absent/garbage
    sidecar falls back to standalone probing. These exercise the parse +
    classification + ordering logic — NOT that a real dependency produces a real
    NIC (dummies never bind).
  - **findings report** (dummy sweep): the summary tables every probed driver +
    the skipped/quarantined one, names the co-loaded dependency driver, prints the
    count line + `NO DHCP LEASE`, recommends `--clear` for the quarantined
    driver, and persists a `SWEEP` line to the NVRAM log for `--dump`. Real
    link/lease detail and the color rendering are HW/visual-verified, not
    asserted from serial.
  - **saved-config round-trip, structurally**: the headless `--_saveconf <line>`
    seam writes a synthetic `NetloadConfig` line (no real win needed) so
    `--dump` showing it and `--clear` removing it are asserted directly. This
    proves the NVRAM persistence/serialization plumbing, not a real load+
    apply (see below).
- **Real-hardware (validated by the user, documented as such — NOT claimed
  QEMU-tested):** actually loading a Dell/iDRAC NIC driver, link detection, the
  15 s DHCP, the crash-culprit path (a genuinely crashing driver), and the
  link-up-no-DHCP → `rndisfix` hint against a real RNDIS BMC-NIC. **The
  dependency co-load actually producing a NIC** (`UsbRndis` + `NetworkCommon`
  co-resident → SNP → link → DHCP) is here too: QEMU has no USB-RNDIS/CDC device
  to bind, so only real iDRAC/BMC hardware exercises the payoff. QEMU proves the
  plumbing (parse, classify, breadcrumb order); hardware proves the NIC.
  **`--save`/`--apply` actually reloading a real driver and re-binding a real
  NIC** is here too — the dummy `.efi` fixtures load but never bind, so a real
  `--save`-then-`--apply` cycle (load the saved driver, resolve the saved MAC
  to a live interface, DHCP/static + verify) is real-hardware-only.

## Open questions / risks

- **Run-from-volume resolution** on firmware where `LoadedImage->DeviceHandle`
  isn't a clean `fsN:` — fall back to the shell cwd / `axl_path_search`, or a
  `--dir <path>` override. (Add `--dir` as an escape hatch.)
- **DHCP-per-interface API surface** — confirm the exact axl-net entry that takes
  a NIC index + timeout during implementation (the `netinfo`/`ensure_drivers`
  bring-up path already does 15 s waits internally).
- **`SetVariable` on a locked/full NV store** — degrade to screen-only breadcrumb
  with a clear warning; never block the probe.
