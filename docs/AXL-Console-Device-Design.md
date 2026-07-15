# AXL Console Device — Design

> Part of the **AXL Console** family — see `AXL-Console-Design.md` for the
> umbrella (the `AxlConsoleOps` contract + how the producers and consumers
> relate, and the PTY/forkpty lens this take-over shape sits inside). This
> doc is the **take-over producer** leaf.

**Status:** PROVEN (spike, 2026-07-10) — output take-over is captured on
**both arches** (x64 confirmed under DEBUG OVMF, edk2-stable202511; aa64
render-confirmed). The **input relay** — the symmetric `ConIn` twin (input
ownership + Ctrl+letter correctness; its effect on the key bounce is *unproven*,
see §5) — is **DESIGNED, not built**. Not yet a
shipped `<axl/…>` surface: the mechanism lives in the AGT spike
`spike/conprov-cv2.c` (throwaway proof), and the axl-sdk producer
(working name `axl-console-device`) is the extraction of it. Evidence:
AGT `.superpowers/sdd/spike-cv2.md`, `consplitter-research.md`,
`edk2-mechanism.md`.

---

## 1. What the device is, and why

`axl-console-device` is the **take-over** producer in the console family:
a resident driver that becomes the **sole** console of a UEFI Shell it did
**not** launch — no child `Shell.efi`, no `StartImage`. It `load`s into the
running boot shell and interposes on it the way a monitor and keyboard do,
then delivers every console *output* call as a structured `AxlConsoleOps`
operation for a consumer to render, and (designed) injects the console's
*input* from an alternate source.

**Origin — the key bounce.** The whole console effort began from a concrete
defect: keys **repeat/bounce** at the raw firmware Shell prompt over a laggy
BMC/KVM link, while `axedit` on the *same* link does **not**. Per the `kbtune`
hardware research (axl-sdk `AXL-KbTune-Design.md` §7) axedit stays clean **with
the software debounce OFF**, so its immunity is **architectural** (read + render
path), **not** the debounce — and the **damaged-region redraw cadence is the
leading candidate**, with drain cadence ruled weak and input read-timing
unexplored. So the render take-over below is the *plausible* bounce cure (proven
here); the input relay (§5) is designed and its bounce effect is **unproven** —
the kbtune A/B settles it. Do not sell the input relay as "the key-bounce fix";
sell the render take-over as the leading-candidate cure, to be validated.

This is one of two insertion strategies axl-sdk knows for splicing a producer
into the live firmware console (the "insertion strategy" axis in
`AXL-Console-Design.md` §4). The other, `axl-console-tap`, **swaps the `gST`
pointers** and is the right tool when the installer is the foreground and then
`StartImage`s a child shell (axterm) or runs at BDS. This doc's device
strategy **never touches `gST->ConOut`'s value** — which is exactly why it can
interpose on a shell that is *already running at the prompt*, the case the tap
cannot serve.

---

## 2. Two launch models (take-over is primary)

A console provider is deployed one of two ways, and the launch context — not
taste — picks the shape:

| Launch context | Shape | Mechanism | POSIX kin |
|---|---|---|---|
| **Loaded onto the existing ESP boot shell** (resident driver) — **the primary, most-used** | **take-over** (`axl-console-device`, this doc) — become the sole ConOut/ConInEx via ConSplitter, no child shell | register / evict console devices | closest to Linux `register_console()` + unregistering `fbcon`; "steal a running shell's terminal" = **reptyr**. NOT a per-process PTY. |
| **ESP boot app** (`\EFI\BOOT\BOOTx64.EFI`) — we are the foreground | **child-shell** (axterm) — swap the console, then `StartImage` a managed child `Shell.efi` | tap swaps `gST` console, StartImage child | **`forkpty(3)`** |

Both route the shell's I/O through the same `AxlConsoleOps` (AGT's input path +
our render), so a consumer is unaffected by which is used. **Lead with
take-over:** most users do not want to overwrite the ESP boot shell with a
foreground app, so interposing on the *existing* one is the default. The
child-shell shape (`axl-console-tap` + `AGT-Axterm-Design.md`) is the answer
only when we are already the foreground boot app.

---

## 3. The take-over mechanism (proven, non-fragile, no NVRAM)

All in the resident driver's `DriverEntry`, at **TPL_APPLICATION**, with **no
defer timer**, **no `gST->ConOut` reassignment**, **no foreign vtable
mutation**, and **no NVRAM write**. Verified against
`MdeModulePkg/Universal/Console/{ConSplitterDxe,ConPlatformDxe}` @
edk2-stable202511; captured by `spike/conprov-cv2.c`.

1. **Publish + self-tag in one call.** Install our own
   `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` (feeding `AxlConsoleOps`, owning its
   `SIMPLE_TEXT_OUTPUT_MODE`) and a Vendor device path on a fresh handle —
   and, in the *same* `InstallMultipleProtocolInterfaces`, install
   **`gEfiConsoleOutDeviceGuid`** on that handle. ConSplitter's ConOut
   `Supported()` binds on the presence of that **tag**, not on the `ConOut`
   NVRAM variable (`ConSplitter.c:830/972`), so tagging ourselves is
   sufficient — **and nothing writes `ConOut`, so there is no SPI-flash
   write.**
2. **Connect.** `ConnectController(ourHandle)` → ConSplitter's Start sees the
   self-installed tag and adds our `SimpleTextOut` to the fan-out. No `ConOut`
   read is involved anywhere.
3. **Evict the others.** `LocateHandleBuffer(gEfiConsoleOutDeviceGuid)` and,
   for every handle but ours, `UninstallProtocolInterface(…, ConsoleOutDevice)`.
   Removing the tag makes the core disconnect ConSplitter from that handle →
   ConSplitter's Stop drops the firmware GraphicsConsole (and serial) from the
   fan-out. The Shell's output now fans **only to us**, so GraphicsConsole goes
   silent and the consumer owns the framebuffer.
4. **Re-sync the console logger to our geometry.** One
   `gST->ConOut->SetMode(gST->ConOut, 0)`. A UEFI Shell wraps `gST->ConOut` in a
   **ConsoleLogger** that caches its scrollback row count (`RowsPerScreen`) from
   `QueryMode` at shell start and refreshes it **only** when a `SetMode` is routed
   through `gST->ConOut` (`ConsoleLoggerSetMode → ConsoleLoggerResetBuffers`,
   `ConsoleLogger.c:1016/1254`). Steps 1–3 changed the console geometry via
   `ConnectController` + eviction *without* such a `SetMode`, so the logger keeps
   its stale row count. When our grid is **taller** than that stale count, the
   logger's history bound (`RowsPerScreen*ScreenCount-1`) is exceeded the moment
   the shell scrolls, and it `ASSERT`s / `CpuDeadLoop`s at `ConsoleLogger.c:489`
   (`CursorRow == RowsPerScreen*ScreenCount-1`). AGT hit this at 142×44 as a
   ~1-in-6 timing race (sometimes an incidental `SetMode` re-synced first); the
   round-trip closes the window deterministically. We advertise one mode (0), which
   ConSplitter exposes post-eviction, so `SetMode(0)` maps to our geometry; the
   logger's `ClearScreen` on `SetMode` routes a harmless clear down to the consumer
   at take-over. When nothing wraps `gST->ConOut` this is a no-op re-affirm.
   Regression: `test-console-device-qemu.sh` Scenario 3 (wide smoke built
   `-DPRECACHE_SMALL` forces the stale-small window → 4/4 assert without this step,
   8/8 clean with it).

**Why it survives `load`'s `ConnectAllEfi`** (the connect-all storm that
`StartImage` runs immediately after our `DriverEntry`, and that defeated the
earlier `DisconnectController` eviction): uninstalling the *tag* disconnects
**ConSplitter** — the tag's `BY_DRIVER` consumer — but **not ConPlatform**,
which holds the GOP handle's `SimpleTextOut` open `BY_DRIVER` (it opened the
protocol, not the tag; it only *installed* the tag at boot). So ConPlatform
stays `Started`; on `ConnectAllEfi` its `Supported()` re-opens `SimpleTextOut`
`BY_DRIVER` and gets `EFI_ALREADY_STARTED` → its `Start()` never re-runs → it
never re-evaluates `IsInConOutVariable` and **never re-tags GraphicsConsole**
(`ConPlatform.c:418` tags only when the path is in `ConOut`, and it does not
re-evaluate). GraphicsConsole stays out of the fan-out. This holds whether or
not the GOP path is in `ConOut` — which is precisely why the earlier `ConOut`
NVRAM rewrite was redundant and was dropped.

**Why it is deterministic (no deadloop, no timer).** `gST->ConOut`'s **pointer
value is never touched** — we sit *below* the ConSplitter aggregate, not in
front of it. So the Shell's per-command `RestoreStdInStdOutStdErr` never sees
`gST->ConOut != saved` (`ShellParametersProtocol.c:1420`), never runs
`CloseSimpleTextOutOnFile` on our struct, and the `Pool.c(721)` free-of-a-
static assert that kills a `gST`-swap can never fire. All surgery runs inside
the `load` command bracket and is safe there *because* the pointer is constant.
Evidence: `spike-cv2-x64-edit.debugcon.log` / `-prompt.debugcon.log` show 0
`ASSERT` / `Pool.c(721)` / `Bad Signature` / `CpuDeadLoop`, with `edit`
servicing a file at the tail; `spike-cv2-x64-edit-ui.png` shows the whole
`edit` UI in our grid with the region past our grid **pure black after both
connect storms** (GraphicsConsole confirmed silent, not co-painting).

**Uninstall / restore (the inverse surgery).** `axl_console_device_uninstall`
un-does the take-over in three steps, and the *order* matters:

1. **Restore the evicted consoles.** For each handle we evicted, re-install the
   `gEfiConsoleOutDeviceGuid` tag and `ConnectController` it, so ConSplitter's
   `Start` re-adds the firmware GraphicsConsole to the fan-out. The local display
   comes back.
2. **`DisconnectController(ourHandle)` — before uninstalling our protocols.**
   ConSplitter opened our `SimpleTextOut` `BY_DRIVER` when it bound us, and holds
   a raw pointer to it in its fan-out list. `UninstallMultipleProtocolInterfaces`
   alone does **not** reliably make ConSplitter drop that entry, so an explicit
   `DisconnectController` runs ConSplitter's `Stop`, which removes our
   `SimpleTextOut` from the list and closes its open. **Skipping this is a
   use-after-free:** our `SimpleTextOut` lives inside the device struct we
   `axl_free` at the end, so a stale fan-out entry means the *next* `OutputString`
   on `gST->ConOut` jumps through a freed vtable and **hangs**. This was caught
   under DEBUG OVMF by `test-console-device-qemu.sh` Scenario 2 (bisection: the
   `uninstall` call returns, the first post-restore `OutputString` wedges;
   skipping the free makes it succeed — confirming the dangling reference).
3. **Uninstall our protocols and free.** With ConSplitter no longer referencing
   us, `UninstallMultipleProtocolInterfaces(devpath, SimpleTextOut, tag)` and the
   subsequent `axl_free` are safe.

---

## 4. Why the device model won (the dead ends, one line each)

The take-over shape is settled; this is *why*, not a re-litigation. The full
spike journey (options A–F, Spikes D / NF / CV) is in AGT's
`spike/README.md` + the SDD notes.

- **Swap `gST->ConOut` from the resident driver — FATAL.** A pointer change
  inside a Shell command bracket is freed *and* unconditionally reverted by
  `RestoreStdInStdOutStdErr` at command end → `Pool.c(721)` deadloop
  (backtrace-confirmed, `edk2-mechanism.md`). This is `axl-console-tap`'s
  strategy and is only safe *before* the command loop (child-shell / BDS).
- **In-place wrap of ConSplitter's method pointers — FRAGILE.** Keeps the
  pointer value constant (so it is deterministic) and does give clean local
  ownership when it does not chain, but it overwrites a live foreign vtable and
  banks on ~6 EDK2 properties the UEFI spec never promises
  (`consplitter-research.md` §5) — a per-platform bet, not shippable as a
  product default.
- **`DisconnectController` eviction of GraphicsConsole — UNDONE.** It succeeds,
  but `load`'s `ConnectAllEfi` reconnects GraphicsConsole and a resident driver
  cannot re-evict (needs TPL_APPLICATION; the loop pump is TPL_CALLBACK).

The device model (§3) is the only shape that is **both** deterministic (constant
pointer value) **and** clean-local-owning (evict the other fan-out devices)
**and** non-fragile (own memory, no foreign vtable, no NVRAM) — it wins on every
axis the others split.

---

## 5. The input relay — input ownership + Ctrl+letter (BUILT, increment 2)

§3 takes over **output**; the symmetric **input** twin gives us ownership of the
shell's keystrokes. It is enabled by `cfg.take_input` (output-only leaves input in
**passthrough** — the physical keyboard still drives the shell through the
firmware's own `ConIn`). Shipped as the **functional half only**: input ownership +
Ctrl+letter correctness (both QEMU-validated). The bounce question is deliberately
NOT settled here (see the overclaim note below).

**Who reads the raw keyboard (the fork).** The primary take-over consumer is a
*resident driver* with no foreground input loop, yet the relay *evicts* the very
firmware keyboard a consumer would otherwise read. So the device supports **both**:
- `cfg.read_physical` (the resident default): the device runs its OWN periodic timer
  that reads the evicted keyboard's `ReadKeyStrokeEx` and re-injects — the consumer
  needs no input path. The read-cadence lever (the unexplored bounce candidate) lives
  here, in the substrate, which is the right home for it.
- the public `axl_console_device_inject_key{,_ex}` / `_inject_text` family (works
  whenever `take_input`): a foreground/remote consumer feeds synthesized keys.

**The key-repeat gate** (`cfg.debounce_ms` / `min_gap_ms`) sits on the read_physical
path and is **OFF by default** — present so the shipped defaults are tunable once the
real-HW A/B runs, NOT a claimed bounce cure.

**Shared engine.** The ring, the `EFI_SIMPLE_TEXT_INPUT_EX` key-notify registry, the
Ctrl+letter fold, and the `inject_text` VT decoder live in the internal
`AxlConsoleInput` engine, shared with the tap (the input twin of `AxlConsoleEmit`).
The device is the sole owner, so its ConInEx methods read the engine with no
passthrough; the tap layers passthrough on top for its non-capture mode.

**On the key bounce specifically — do not overclaim.** Per kbtune §7 (see the
umbrella + `AXL-KbTune-Design.md`), axedit is immune **with the debounce OFF**, so
the debounce filter is *not* the cure; the leading candidate is the **render**
cadence (which §3's take-over already provides), and the input *read* path is an
unexplored candidate. So routing input through AGT here is worthwhile for
**input ownership** (no keyboard contention), **Ctrl+letter correctness** (below),
and a *possible but unproven* read-cadence contribution to the bounce — **not** as
the established bounce fix. Settle the bounce question with the kbtune A/B on a
bouncing link, not by assuming this relay cures it.

The input take-over mirrors the output one exactly: register our own
`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL` as the **sole ConIn device** (self-tag
`gEfiConsoleInDeviceGuid`, `ConnectController`, then evict the raw firmware
keyboard from the split by uninstalling *its* tag), fed by the read loop and/or the
inject API above. ConSplitter merges console-in devices exactly as it
fans console-out ones, and the input aggregate **is** `gST->ConsoleInHandle`,
so a full-screen app reading `SimpleTextInputEx` off `ConsoleInHandle` (the path
`edit` uses) sees our injected keys natively — no `ReinstallProtocolInterface`
hack. ConSplitter also aggregates `SimplePointer` / `AbsolutePointer`, so a
relayed mouse rides the same mechanism (the BMC-mouse work already proved the
`AbsolutePointer` half).

**Ctrl+letter folding falls out of this** and is already solved on the sibling
tap: EDK2's `edit` maps Ctrl+Q → Exit by indexing its command table on
`UnicodeChar` (`MainTextEditor.c:201`), and the Shell registers its Ctrl+C break
with an exact modifier match — so an injected key must carry the shift-state
(`AXL_CONSOLE_SHIFT_STATE_VALID | …CONTROL_PRESSED`), and the Simple-read path
folds the letter to its C0 code. Both producers share this via `AxlConsoleInput`.
The POSIX line discipline (echo, `ISIG`, cooked mode) has no UEFI equivalent — the
Shell + our injection re-implement only the bits we need. Validated in QEMU: the
`--sendkey` path reaches the shell through the relay (raw keyboard evicted, no
double-delivery), and the fold is unit-tested (Ctrl+C → 0x03 on the Simple read).

---

## 6. Portability caveat

The take-over relies on **EDK2's console-device model** — ConSplitter, the
`gEfiConsoleOutDeviceGuid` tag, ConPlatform's driver-binding — none of which the
UEFI specification defines (a grep of UEFI 2.11 for `splitter` = 0 hits; the tag
GUIDs live in `MdeModulePkg`, the impl package, not `MdePkg`, the spec package).
See `consplitter-research.md` for the spec-vs-EDK2 boundary.

- **EDK2-family firmware is fine, per-target verify.** The diagnostic-tool
  targets — **AMI Aptio, Insyde** — are EDK2-derived, so the ConSplitter model
  almost certainly holds; treat as **"works until validated per platform."**
- **Biggest real risk = OEM BMC/SOL console-redirect stacks.** A vendor console
  redirection that caches function pointers or paints outside `gST->ConOut` is
  the most realistic real-world break; validate on target hardware.

Unlike the in-place vtable wrap (§4), the device model is on much firmer ground
here: it uses only *documented* boot-services and EDK2's *intended* console-
device admission path — it installs its own protocols and lets the platform bind
them, rather than overwriting a foreign struct in place.

---

## 7. Place in the console family

This doc covers `axl-console-device` (take-over) only. For the contract it
feeds, the sibling producers/consumers, and the PTY lens, start at
`AXL-Console-Design.md`:

- `axl-console-tap` (shipped) — the **swap** producer, and the **child-shell /
  forkpty** sibling of this take-over; also what `axl-console-mirror` rides on.
  The deterministic-vs-fragile contrast in its §6/§7 is the mirror image of §3–§4
  here.
- `axl-console-mirror` (shipped) — the ops → VT-wire consumer (SoftBMC remote).
- `axl-vterm` (shipped) — the other producer, parsing a real VT byte stream.
- AGT `AGT-Console-Relay-Design.md` — the AGT-side adoption stub (`AgtTerminal`
  binds `AxlConsoleOps`; the `conprov-*` spikes live under `agt/spike/`).
