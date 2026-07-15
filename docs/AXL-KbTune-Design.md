# AXL `kbtune` — Keyboard Debounce Tuner (Design + Implementation Plan)

`kbtune` is an axl-sdk tool that diagnoses and **fixes** the "bouncing" /
repeating keystrokes seen when typing at the **UEFI Shell command line over a
remote console** (a BMC/KVM virtual console — iDRAC, Avocent, IPMI SOL, ...).
It presents a live GOP UI to compare input strategies and tune a debounce
window, and — on commit — installs a **resident console filter** so the chosen
setting stays in effect for the shell after the tool exits. Re-running it
reattaches to the resident filter for re-tuning (e.g. when moving from LAN to
WAN latency).

Status: Phase 0.5 + Phase 1 done. Phase 1.5 (lag fix) and a corrected root-cause
model are in flight — see §1, §1.5, §4. Phase 0 (CLI probe) prototyped in
`local/kbprobe/`.

> **2026-07-08 correction (real-hardware evidence).** The original premise —
> "axedit is immune to the shell bounce *because* it runs the software debounce
> recognizer" — is **false**. On a link where the shell command line bounces,
> `axedit` on the **same link at the same time** echoes every key exactly once
> **with debounce OFF** (it is off by default and AGT/axedit never enables it).
> axedit's immunity is therefore **architectural** (its read + render path), not
> a repeat filter. Separately, kbtune Phase 1 has a large input **lag** on real
> hardware that axedit does not — a kbtune render/read-loop problem, unrelated to
> the bounce. Both are worked through below.

---

## 1. The problem (root cause, established)

Typing one key at the shell prompt registers as several characters over a
KVM. Established by reading EDK2 + axl-sdk and a CLI capture:

- **Source — USB typematic + KVM key-up latency.** A KVM emulates a USB HID
  keyboard. `UsbKbDxe`'s typematic state machine (initial delay **500 ms**,
  repeat rate **~20 ms**; `MdeModulePkg/Bus/Usb/UsbKbDxe`) synthesizes repeated
  key-downs while a key appears "held," and clears only on a clean key-up
  (break) report. Over a laggy link the key-up arrives late / is dropped / is
  followed by a spurious re-press, so one physical tap yields a short **bounce**
  burst or a typematic **train**. (Pure duplicate HID reports *are* de-duped by
  the driver; it's the down/up/down or lost-break that leaks through.)
- **Amplifier — the shell's greedy read.** The Shell's StdIn reader
  (`ShellPkg/.../FileHandleWrappers.c`) is a tight blocking
  `WaitForEvent(ConIn->WaitForKey) → ReadKeyStroke` loop that drains every
  queued keystroke immediately, so the whole burst surfaces as characters.
- **Why full-screen editors don't bounce (CORRECTED — this is NOT the debounce).**
  Verified on real hardware: on a link where the *shell* command line bounces,
  `axedit` on the **same link at the same time** echoes every key exactly once —
  **with the software debounce OFF** (AGT/axedit never calls
  `axl_input_set_key_debounce`, and it is off by default; `min_repeat_ms == 0`).
  So axedit's immunity is **architectural** — but which axis is NOT yet pinned
  (§7). Note what does **not** distinguish them:
    - **Read is NOT the difference.** BOTH drain greedily. axedit reads via
      `axl_compositor_attach_keyboard` → `axl_input_attach_key` → the axl-loop
      keypress dispatch, which drains the entire firmware queue per wake
      (`ReadKeyStrokeEx`); the shell's StdIn reader (§1 amplifier) *also* drains
      every queued keystroke per wake. So "drain-all" alone is not the cure.
    - **Render / per-key processing (candidate).** axedit repaints only damaged
      regions (compositor invalidate/redraw) and does per-key editor work; the
      shell re-echoes the command line. The EDK2 `edit` command's slow
      one-key-per-heavy-pass cadence (`Stall`-gated) is a *separate* incidental
      rate-limit. Whether a redraw/processing cadence is what actually meters the
      burst is the open question in §7.
  **Open question (§7):** which of drain cadence / editor processing / redraw
  cadence actually suppresses the typematic burst is not yet pinned. The software
  debounce is an *available* filter kbtune exposes for experiment — **not** the
  demonstrated axedit mechanism.
- **Not a protocol issue.** Plain `SimpleTextInput::ReadKeyStroke` and
  `SimpleTextInputEx::ReadKeyStrokeEx` drain the **same** firmware key queue
  (UsbKbDxe `EfiKeyQueue`, ConSplitter `KeyQueue`), so switching protocol alone
  does *not* fix it. The fix is a **software debounce filter**.

## 1.5 A second, separate problem — kbtune's own input lag

Distinct from the bounce, kbtune Phase 1 shows a large type-to-display **lag** on
real hardware (a BMC GOP + KVM) that axedit does not. Root-caused to kbtune's
render/read loop — **not** to any shared-SDK change (the Phase 0.5 Ex-unify caches
its `SimpleTextInputEx` handle, so it adds zero per-read cost; the console
non-blocking fix is what lets `read_key(0)` see queued keys at all):

- **Full-screen repaint per keystroke.** `render()` fills the whole framebuffer
  (`fill_rect(0,0,W,H)`) and redraws the entire HUD every dirty frame. On a BMC
  GOP the full-frame write is slow, and it forces the KVM to re-encode the whole
  frame each key. axedit repaints only damaged regions.
- **One key per pass (axEdit / edit modes).** kbtune reads a single key per loop
  pass, then repaints; fast typing / typematic bursts back up in the firmware
  queue and surface later and later. axedit drains the whole queue per wake.

**Fix — kbtune Phase 1.5:** mirror axedit on both axes.
1. **Incremental redraw.** Split the static HUD (title, mode buttons, help — drawn
   once per mode change) from the per-key dynamic regions (echo line, event log,
   stat strip), and repaint only the dynamic regions, clearing each to its
   background first. No full-screen `fill_rect` per key.
2. **Drain all queued keys per pass**, in every mode, matching the axl-loop
   keypress dispatch; when a mode enables the debounce filter it applies per
   drained key. This also makes the axEdit mode a faithful mirror of the axedit
   read path it claims to reproduce (F3 `edit` deliberately keeps its
   one-key-per-pass + stall — that throttle *is* the cadence it demonstrates).

## 2. The fix already exists in axl-sdk (dogfood inventory)

Everything needed is present; `kbtune` composes it.

| Need | axl-sdk building block |
|------|------------------------|
| Debounce logic (OPTIONAL) | `axl_input_key_accept(AxlKeyDebounce*, AxlInputEvent*)` + `axl_input_set_key_debounce(min_repeat_ms, printable_only)` (`axl/axl-input.h`) — drops a same-key KEY_DOWN faster than a human types; printable-only exempts navigation keys. **OFF by default** (`min_repeat_ms == 0`) and, per the §1 correction, **NOT** the proven axedit fix — an available filter kbtune exposes so you can test whether it helps a given link. **Already unit-tested.** |
| Console (ConIn/ConInEx) wrapping | `src/util/axl-console-mirror.c` — the working template: custom `EFI_SIMPLE_TEXT_INPUT[_EX]_PROTOCOL`, own `WaitForKey[Ex]` via `EVT_NOTIFY_WAIT`, `ReinstallProtocolInterface` on `gST->ConsoleInHandle` (so `edit`/HandleProtocol consumers see it), `gST` pointer swap **after** the reinstall, full restore on uninstall + `atexit`. |
| Resident driver + embed + **reattach** + launcher RPC | **`AxlSharedDriver`** — one layer for all of it. `axl_shared_driver_locate(name, drv_file, embed_blob, len, &iface)` = *reattach-or-load-embedded* in a single call (already-resident short-circuits → that's re-tuning on restart). Driver-side `axl_shared_driver_publish(name, &vtable, &h)` publishes a **consumer-owned** vtable — GUID derived from the name (no fresh GUID, no bespoke protocol). `axl/axl-shared-driver.h`, `docs/AXL-Shared-Driver-Recipe.md`, `sdk/examples/shared-driver-demo/`. **NOT** `AXL_SERVICE_DRIVER` — that composes an `AxlLoop` the filter never uses (the wrap is firmware-`WaitForKey`-driven, passive). |
| GOP UI (HUD + text + double-buffer) | `axl_gfx_*` + `axl_font` — template `sdk/examples/pointer-demo.c` (`axl_gfx_default_font`, `axl_gfx_draw_text`, `axl_gfx_buffer_present`, `axl_loop_add_key_press`). |
| Raw plain-path read (Shell mode) + timing | `axl_console_read_key` (plain `ReadKeyStroke`), `axl_time_get_us`, `axl_msleep`. |

## 2.5 Related SDK change — unify the console key-read on `ReadKeyStrokeEx`

Independent of kbtune but motivated by it, and a prerequisite for clean modifier
display: today the public `axl_console_read_key` / `axl_console_readline` read via
plain `SimpleTextInput::ReadKeyStroke` (no modifiers), while the input/loop layer
reads via `ReadKeyStrokeEx`. Unify the public path on the Ex read.

- **Decision: replace, don't flag.** `axl_backend_console_read_key_ex` already
  does *Ex with automatic plain fallback* (Ex when `ConsoleInHandle` publishes it;
  plain `ReadKeyStroke` + `modifiers = 0` on serial/TerminalDxe). Ex is a strict
  superset (same key queue per the §1 finding, plus modifier/toggle state), so a
  caller flag or a parallel `_ex` API would only add surface. We control all
  consumers (`feedback_change_apis_freely`).
- **Changes:** add `uint32_t modifiers;` (AXL_INPUT_MOD_*, 0 when unavailable) to
  `AxlKey`; point `axl_console_read_key` (and `axl_console_readline`'s internal
  reads) at `axl_backend_console_read_key_ex`. The wait already prefers
  `WaitForKeyEx`. **Do not** enable `EFI_KEY_STATE_EXPOSED` on this path — Ex
  returns per-keystroke modifiers without it; EXPOSED/partial events stay the
  input-layer's job, so simple readers never see modifier-only partials.
- **Not a bounce fix** — same queue, so it changes nothing about typematic/bounce;
  it just gives every reader modifiers for free and collapses two read paths to
  one.
- **Test-first (bucket A):** existing console-read/readline tests must still pass
  (modifiers default 0 under QEMU serial); add an assertion that `AxlKey.modifiers`
  is populated/zeroed as specified and scan/unicode are unchanged.

Sequence it as **Phase 0.5** (before kbtune Phase 1 uses modifiers), or land it
standalone — it benefits every consumer.

## 3. Architecture — two components in one tool

```
 tools/kbtune.c   (launcher + GOP UI)         ships: tools/kbtune.efi
   │  axl_shared_driver_locate("kbtune", "kbtune-drv.efi", embed, len, &iface)
   │      → reattach if resident, else load the embedded blob
   └─ embeds ─▶  kbtune-drv (resident driver, NO event loop)
                    • wraps gST->ConIn + ConInEx (ConsoleInHandle) with the
                      debounce filter (axl_input_key_accept)   [mirror console-mirror]
                    • axl_shared_driver_publish("kbtune", &vt)  — vt = {get,set}
                    • stays resident after kbtune exits; on unload: unpublish +
                      restore ConIn
```

**Config channel = the published shared-driver vtable** (consumer-owned; no
separate protocol GUID — `axl_shared_driver_guid("kbtune")` derives it):
```c
typedef struct { uint32_t version;
                 bool     enabled;         // filter active?
                 uint32_t min_repeat_ms;   // debounce window
                 bool     printable_only;  // exempt navigation keys
} AxlKbTuneConfig;
typedef struct {                            // the vtable kbtune-drv publishes
    uint32_t version;
    int (*get)(AxlKbTuneConfig *out);
    int (*set)(const AxlKbTuneConfig *in);  // applies live to the wrap filter
} KbTuneVtable;
```
(The standard `AXL_SHARED_DRIVER` macro publishes the SDK `{run}` vtable — the
common command-dispatch case; kbtune wants a richer `{get,set}` channel, so the
driver calls `axl_shared_driver_publish` with its own `KbTuneVtable` and the
launcher casts the located `iface` to it. The `run`-based path would have to
smuggle the window through an argv/rc side-channel — clumsier for read-back.)

**Lifecycle**
1. `kbtune` starts → `axl_shared_driver_locate("kbtune", "kbtune-drv.efi",
   embed, len, &iface)`.
   - resident → **reattach**: `vt->get(&cfg)`, pre-select that mode/window.
   - absent → it loads the embedded `kbtune-drv`, whose entry wraps ConIn and
     publishes the vtable; then resolves `iface`.
2. GOP UI: pick mode, tune the window live, watch echo + event log.
3. **Exit keys:** `Esc` = leave the committed setting unchanged; `F10`
   (or `Enter`) = **commit** → `vt->set(&cfg)` so the resident filter uses the
   chosen window; the driver stays resident so the shell inherits it.
4. Re-run any time → step 1 short-circuits to the resident driver → re-tune
   (LAN↔WAN).

**Event-loop model (they differ):**

- **Launcher (`kbtune`) — a hand-rolled poll loop, NOT `axl_loop`.** The three
  modes need three read *cadences* (greedy drain / one-per-pass / one-per-pass +
  stall) — exactly the shape of the EDK2 `edit` input loop — so kbtune polls the
  non-blocking `axl_console_read_key(0)` + a per-pass `axl_msleep` and renders on
  change. This also sidesteps `axl_loop`'s intrinsic keypress handling, which
  would compete for the shared key queue in the greedy mode. The debounce fix
  (axEdit) is `axl_input_key_accept` applied per key, fed an `AxlInputEvent` built
  from the read.
- **Driver (`kbtune-drv`) — NO loop.** The ConIn wrap is *passive*: its
  `WaitForKey[Ex]` is an `EVT_NOTIFY_WAIT` event whose notify fires when a
  consumer (the shell) waits on it — firmware-driven, not driver-driven. The
  debounce is a *reactive drop* filter (`axl_input_key_accept`: drop a same-key
  repeat that arrives too soon), with no buffering/delayed delivery, so no timer
  is needed either. That's precisely why `AxlSharedDriver` (resident, no loop)
  fits and `AXL_SERVICE_DRIVER` (which composes an `AxlLoop`) does not. (If we
  ever moved to a *delayed*/coalescing debounce that holds a key to see whether
  more follow, the driver would then need a timer event — noting it, but the
  current reactive filter doesn't.)

**Wrapped `WaitForKey` (the one hard part, solved by the mirror template):** an
`EVT_NOTIFY_WAIT` event whose notify pulls from the real ConIn, runs each key
through `axl_input_key_accept`, queues survivors, and leaves the event signaled
only while a survivor is pending — so a *dropped* key never leaves the shell
spinning on an empty read. Runs at `TPL_CALLBACK`, kept light. **No
`RegisterKeyNotify`** (can't suppress keys, and it caused a ConSplitter TPL/CPU
regression — see the note in `axl-backend-native.c`).

## 4. Modes & UI

**Three radio modes — the three real UEFI consumers, reproduced with a
hand-rolled poll loop over `axl_console_read_key`.** Plain vs Ex read the same
queue, so the axes that matter are the debounce FILTER and the read CADENCE, not
the protocol. Each mode is a distinct read strategy:

| Key | Mode | How it reads | What it isolates |
|-----|------|--------------|--------|
| `F1` | **Shell (greedy)** | drain EVERY queued key each pass, no filter | the shell read path — the bounce reproducer |
| `F2` | **axEdit read + debounce** | drain all queued keys/pass (Phase 1.5), each optionally through `axl_input_key_accept` (window; **OFF by default**) | the FILTER *experiment* — NOT what makes axedit immune (axedit runs debounce-off; see §1). Isolates whether the software filter helps *your* link on top of the drain-all read |
| `F3` | **edit (throttled poll)** | one key/pass + a per-pass stall, no filter | the CADENCE mechanism (what EDK2 `edit` does: `CheckEvent` + `Stall`) — keeps one-key-per-pass on purpose, to see whether slowing the reader *alone* cures it |

> **Note.** After the §1 correction, F2 no longer claims to *be* axedit's fix. Its
> read now mirrors axedit (drain-all per pass); the debounce on top is opt-in and
> off by default. The most axedit-faithful configuration is F2 with the filter
> off — which is exactly what should be A/B-tested against F1 on a bouncing link
> to see whether the *read + render* change alone (independent of any filter)
> already matches axedit's immunity.

- **Polls the non-blocking `axl_console_read_key(0)` + a per-pass `axl_msleep`
  for pacing.** (Phase 1 found and FIXED a bug here: the 0-timeout read used to
  gate on `CheckEvent(WaitForKeyEx)`, which OVMF's ConSplitter does not fire for
  a queued key, so it silently dropped keys; it now reads the queue directly —
  the authoritative non-blocking check. See the console commit.)
- **One tunable per mode:** `Up`/`Down` adjust the axEdit debounce window, or the
  edit poll stall (ms), live in the HUD. `F4` toggles printable-only (axEdit).
- **Control keys are all NON-printable** (`Tab`/`F1`/`F2`/`F3`/`Up`/`Down`/`F4`/
  `Esc`), handled *before* the filter, so every printable key stays free to test
  and mode-switch/tune works in every mode. `Esc` quits (Phase 2 adds `F10` =
  commit-and-persist).
- **Echo** pane echoes like a shell line (printable append, Backspace erase,
  Enter clears) so a bounce is *visible* as extra characters; a debounced drop
  shows as a `DROP` row.
- **Layout** (mirrors `pointer-demo`/`input-demo`): HUD legend (radio buttons,
  active highlighted, the active tunable, driver status); echo pane; event log
  `#n d=…µs scan=… char=… mod=… <tag>` (BOUNCE / typematic~20ms / DROP); stat
  strip (seen, debounced-drops, last key, burst, min gap).

## 5. Persistence

- **Per-boot (this design):** the resident driver holds the setting for the
  current session (until reset). This is what "stays in effect for the shell"
  means here.
- **Cross-reboot (later phase):** a `Driver####` + `DriverOrder` NVRAM
  load-option so firmware auto-loads `kbtune-drv` at boot. Out of scope for v1.

## 6. Implementation phases

### Phase 0 — Characterize (prototype: `local/kbprobe/`) ✅ mostly done
CLI probe that reads like the shell and logs each key with a µs delta + a
bounce/typematic tag. **Deliverable:** real-KVM captures that confirm
bounce-vs-typematic and a starting debounce window.
- **Test:** QEMU serial-driven smoke (done); real-HW capture over the KVM (owner).

### Phase 0.5 — Unify console key-read on `ReadKeyStrokeEx` (see §2.5) ✅ DONE
Add `modifiers` to `AxlKey`; point `axl_console_read_key` (+ `axl_console_readline`
internals) at `axl_backend_console_read_key_ex` (Ex with plain fallback). Standalone
SDK change; benefits every consumer; prerequisite for kbtune's modifier display.
- **Test:** existing console-read/readline unit + integration green; new assertion
  on `AxlKey.modifiers` (populated when Ex present, 0 on serial fallback); no
  change to scan/unicode. Independent code review before commit.

### Phase 1 — `kbtune` UI app (no driver yet) ✅ DONE (three modes)
GOP UI with three radio modes (Shell greedy / axEdit / edit), `Up`/`Down` tuning,
echo pane, event log, `Tab`/F switching. Debounce applied per-key via
`axl_input_key_accept` in axEdit mode (global window via
`axl_input_set_key_debounce`); no ConIn wrapping. Ships as `tools/kbtune.efi` (in
`TOOL_NAMES` + `devkit.conf`).

### Phase 1.5 — Fix kbtune's input lag (see §1.5) ⬅ NEXT
Mirror axedit's read + render so kbtune stops lagging on real hardware:
1. **Incremental redraw** — draw the static HUD once per mode change; per key
   repaint only the echo / event-log / stat regions (clear-to-bg + redraw), no
   full-screen `fill_rect`.
2. **Drain-all per pass** in every mode (F3 keeps its throttle stall), so the
   firmware key queue never backs up; the debounce (when the mode enables it)
   applies per drained key.
- **Tests:** QEMU smoke stays green (modes switch, keys echo, Esc exits);
  screenshot confirms the static HUD + dynamic regions render correctly and the
  echo box contains its text. Real-HW: confirm the type-to-display lag is gone and
  kbtune feels like axedit. (QEMU can't reproduce the BMC-GOP lag, so the lag
  win is owner-verified.)
- **Tests:**
  - QEMU smoke (`test-kbtune-qemu.sh`): drive keys over the serial socket
    (`drive-serial.py`), assert modes switch (Tab/F), the HUD renders the active
    mode, the event log logs keys with deltas, and Esc exits. GOP present-only
    (`-nographic` won't show pixels) — assert via a debug/text trace the tool
    also emits, or screenshot via `run-qemu --screenshot` (see
    `reference_gop_visual_testing`).
  - Real-HW: run over the KVM, confirm Shell-mode bounces and axEdit-mode is
    clean; find the window.

### Phase 2 = "A" — resident input-conditioning shim (debounce + min-gap)

> **Scope (post-correction, user-approved 2026-07-09).** Debounce is NOT the proven
> axedit fix, but it and a source-side **min-gap** are the two levers a resident
> `ConIn` shim can actually apply to the real shell (the shell's own read cadence /
> redraw are not ours to change — those stay kbtune-UI-only simulation knobs). So
> "A" persists exactly the **source-side input conditioning**; the windowed
> live-shell view ("B" = `axterm`) is handed off to the **AGT** project
> (`agt/docs/AGT-Axterm-Design.md`) and reuses A's config + conditioning.

**What persists (the config channel — consumer-owned vtable, no fresh GUID):**
```c
typedef struct {
    uint32_t version;
    bool     enabled;         // filter active?
    uint32_t debounce_ms;     // drop a same-key repeat faster than this (reactive)
    uint32_t min_gap_ms;      // min spacing between ANY delivered keys (timer-gated)
    bool     printable_only;  // exempt navigation/editing keys from both
} AxlKbTuneConfig;
typedef struct {              // published under axl_shared_driver_guid("kbtune")
    uint32_t version;
    int (*get)(AxlKbTuneConfig *out);
    int (*set)(const AxlKbTuneConfig *in);   // applies live to the wrap filter
} KbTuneVtable;
```
`drain`/`stall`/`redraw` are deliberately absent — they describe a *reader's* loop,
which the shim cannot impose on the shell.

**Ex protocols throughout (user requirement).** The wrap reads the real console via
`ReadKeyStrokeEx` / `WaitForKeyEx` (modifiers preserved, authoritative queue —
consistent with Phase 0.5), and wraps BOTH the Ex interface on `ConsoleInHandle`
AND the Simple `gST->ConIn` that the shell's line reader actually calls, so the
shell receives the conditioned stream. Mirror `axl-console-mirror.c`'s
`WaitForKey`-survivor notify so a *dropped* or *held* key never leaves the event
signaled with nothing to read (shell busy-loop).

**The two conditioners:**
- **debounce** — reactive drop via `axl_input_key_accept` (no timer).
- **min-gap** — hold a key that arrives < `min_gap_ms` after the last *delivered*
  key; release it when the gap elapses. Needs a **timer event** in the wrap to
  flush the held key and signal `WaitForKey[Ex]`. Uses the new `axl-input.h`
  min-gap release-gate primitive (pure + unit-tested), so the driver and `axterm`
  share identical, tested logic. Build/test debounce first (green), then layer
  min-gap.

**Build/load:** `kbtune-drv.efi` built with `AXL_DRIVER` + `axl_shared_driver_publish`
(custom vtable, not the `{run}` macro). Because `kbtune` is a busybox multicall
tool, the driver is **staged as a sibling** and loaded via
`axl_shared_driver_locate_sibling("kbtune", "kbtune-drv.efi", &iface)` — no embed,
version-pinned. Launcher (`kbtune`): `get` to pre-select the committed window, **F10**
commits (`set`, persists), **Esc** leaves unchanged; re-run reattaches to re-tune.

- **Tests:**
  - Unit: min-gap gate + debounce accept/drop/hold over synthetic rapid same-key +
    mixed streams (pure timing, deterministic); config get/set round-trip.
  - Integration (`test-kbtune-driver-qemu.sh`): load driver; inject rapid identical
    keys; read via `gST->ConIn->ReadKeyStroke`; assert in-window repeats drop and
    spaced keys pass; **exit the app**, assert the wrap persists and a second run
    **reattaches** (no double-install). **Hazard-safe** per
    `feedback_uefi_firmware_test_hazards`: assert only clean negatives; the driver
    MUST restore `gST->ConIn`/`ConInEx` on unload (test the restore path); run this
    binary FIRST so a wedge can't starve the suite.
  - Real-HW (definitive): commit a window, exit to the shell, confirm the prompt no
    longer bounces over the KVM; re-run and re-tune (LAN↔WAN).
  - Gate: **no delldiags regression** — `test-doefi.sh` 208/208 before any release.

### Phase 3 — Cross-reboot persistence *(optional)*
`Driver####`/`DriverOrder` NVRAM load-option for `kbtune-drv`; a `kbtune
--install-boot` / `--uninstall-boot` action.
- **Test:** integration reboot cycle (or documented real-HW).

### Phase 4 — Auto-suggest *(optional)*
From a short capture, propose a window (just above the observed typematic
interval, below the human same-key minimum).

## 7. Risks / open questions

- **⚑ MECHANISM VALIDATED in QEMU; axedit's app-level immunity remains real-HW-only.
  Why is axedit bounce-immune with debounce OFF?** The bounce is UsbKbDxe's
  bounded key queue DROPPING synthesized repeats when the consumer lags, so a
  slower reader surfaces fewer of them. QEMU confirms *that lever exists* — but it
  does NOT prove axedit's own rendering supplies enough of it, and the distinction
  matters:
  - **What QEMU proves (the mechanism).** `run-qemu --holdkey "a:MS"` holds a USB
    key down via QMP past the typematic delay, so OVMF's UsbKbDxe synthesizes
    repeats like a KVM's delayed key-up. `test/integration/test-kbtune-bounce-qemu.sh`
    reads it back through `kbprobe` with *explicit* cadence knobs (hold=800 ms):
    greedy **16** → `--throttle 80` **5** → `--debounce 50` **2** (14 dropped);
    a 60 ms tap is a clean **1**. So an explicit throttle and a debounce gate
    demonstrably suppress the burst.
  - **What QEMU does NOT prove (app-level immunity).** Running the real apps
    (axcon / axterm / **axedit**) under the same `--holdkey a:800` renders **~16
    each** — axedit, the real-HW-immune reference, is NOT immune in QEMU. QEMU's
    GOP renders far too fast to reproduce axedit's slow BMC-GOP per-key redraw, so
    its per-key redraw supplies no meaningful throttle here. Therefore
    **"axedit's per-key redraw = implicit throttle" is render-SPEED-dependent**
    (holds on a slow real BMC GOP, not in QEMU), and candidate (b) is *plausible
    and consistent with the mechanism* but NOT confirmed by QEMU.
  - **Consequence for A's defaults.** The render-INDEPENDENT cure is the
    **debounce / min-gap gate** (kbprobe `--debounce` collapses the burst
    regardless of render speed) — which is exactly what "A" ships. Whether a
    render-throttle alone (axterm's architecture) suffices is a real-HW question.
  - **CAVEAT.** QEMU models the *mechanism* (bounded queue + cadence-dependent
    drops), not the real-KVM link or real GOP-render timing. The real-HW A/B
    (shell greedy vs axedit on an actually-bouncing link) stays the definitive
    confirmation of app-level immunity AND the source of the shipped default
    `debounce_ms` / `min_gap_ms`.
- **`WaitForKey` with dropped keys** — must never leave the event signaled with
  nothing to read (shell busy-loop). Mirror the console-mirror notify exactly.
- **ConSplitter rewrites `gST->ConIn`** after `ReinstallProtocolInterface` —
  swap `gST` pointers *after* the reinstall (as the mirror does).
- **TPL** — filter runs in the `WaitForKey` notify at `TPL_CALLBACK`; keep it
  allocation-free and light. No `RegisterKeyNotify`.
- **Compose with `AxlConsoleMirror`** — both wrap `gST->ConIn`. Decide ordering
  / detect-and-chain if a mirror is already installed (kbtune wraps the mirror's
  ConIn, not the raw one — order matters). Document; likely rare together.
- **Unload/restore correctness** — the resident driver must restore cleanly; an
  app crash mid-session leaves a wrapped ConIn (atexit covers clean exit only).
- **Printable-only default** — keep held-arrow/Backspace navigation working;
  default `printable_only = true`.
- **Old EFI Toolkit shell** — explicitly out of scope (no `EFI_SHELL_PROTOCOL`,
  different console model).
- **Driver entry + publish** — the resident driver must install the ConIn wrap
  AND `axl_shared_driver_publish` its `{get,set}` vtable on entry, and unpublish
  + restore on unload. Small decision: use `AXL_SHARED_DRIVER` (gives the
  init/run/unload lifecycle but publishes the SDK `{run}` vtable) vs. a plain
  driver entry that publishes the custom `KbTuneVtable` directly. No fresh GUID
  — `axl_shared_driver_guid("kbtune")` derives it from the name.

## 8. Deliverables checklist

- [x] Phase 0.5: `AxlKey.modifiers` + `axl_console_read_key`/`readline` on `read_key_ex` (Ex + plain fallback); tests + review. **DONE.**
- [x] Phase 1: `tools/kbtune.c` (GOP UI, 3 modes, tuning, echo, log); `TOOL_NAMES` + `devkit.conf`; `test-kbtune-qemu.sh`. **DONE.**
- [x] Phase 1.5 (partial): orthogonal live tunables (debounce/stall/drain/redraw + Left/Right selector) + echo-box fix landed. Incremental static/dynamic redraw still optional (redraw throttle + drain-all cover most of it).
- [ ] Phase 2 = **A**: `axl-input.h` min-gap release-gate primitive (+ unit tests); `AxlKbTuneConfig`+`KbTuneVtable` shared header; `kbtune-drv` resident Ex `ConIn`/`ConInEx` shim (debounce + min-gap) + `axl_shared_driver_publish`; `kbtune` `locate_sibling` + `get`/`set` + F10 commit; `test-kbtune-driver-qemu.sh`.
- [ ] **B = `axterm`** handed off to AGT (`agt/docs/AGT-Axterm-Design.md`) — AGT terminal emulator hosting a nested real Shell, reusing A's conditioning. Its own brainstorm→spec→plan cycle over there.
- [ ] Investigate the §7 research question (F1 vs F2-filter-off on a bouncing link) — sets A's default window + validates `axterm`.
- [ ] Real-HW KVM verification (mark HW-verified in the commit). **delldiags 208/208 gate before release.**
- [ ] Phase 3/4 optional (cross-reboot NVRAM; auto-suggest).
