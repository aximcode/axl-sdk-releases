# AXL Console Mirror — Design

**Status:** IMPLEMENTED (2026-06-15) — all phases done; see §8. axl-sdk
substrate that lets a loop-owning AXL application host the **real UEFI
Shell** (or any blocking app) with its console transparently mirrored to
and driven from a remote terminal — including **full-screen interactive
apps** like the Shell's built-in `edit`, not just one-shot commands.

Motivated by the SoftBMC RemoteShell port. This is the one piece of the
old EDK2 SoftBMC behavior the loop-native command-shell can **not**
reproduce, and the design discussion concluded it belongs in axl-sdk as
shared mechanism (consumer-pull from SoftBMC).

**Shipped surface:** `<axl/axl-console-mirror.h>`
(`src/util/axl-console-mirror.c`), `<axl/axl-shell.h>` +
`axl_image_run` (`<axl/axl-image.h>`). The pump is the existing
`axl_loop_attach_driver`. All five test rungs (§6) pass on x64 + aa64.
What remains is **consumer-side**: SoftBMC's sink⇄WebSocket bridge,
RBAC, and which-shell policy (§4, §9).

---

## 1. History & the requirement (why this exists)

SoftBMC began as **Telcon / Netcon**. From the start the hard problem
was a remote console that gave **direct access to the real UEFI Shell**
— and, critically, could run the Shell's interactive tools, the
canonical example being the built-in **`edit`** command.

`edit` is the line in the sand. A one-shot command (`ls`, `drivers`,
`dmpstore`) writes some text and returns — a request/response shell can
fake that. `edit` is a **full-screen interactive editor**: it takes over
the whole screen, positions the cursor, redraws regions, queries the
terminal size, reads keystrokes continuously (arrows, F-keys, Esc,
PgUp/PgDn), and only exits on a command key. A "command shell" that
dispatches one line at a time **physically cannot host `edit`** — there
is no persistent interactive terminal for it to drive.

The only thing that hosts `edit` faithfully is a **real terminal**: the
firmware `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` / `EFI_SIMPLE_TEXT_INPUT(_EX)`
the Shell already talks to. So the requirement is: **mirror that console
to the wire** — pipe its output to a remote terminal and inject the
remote terminal's keystrokes back into it — while a real `Shell.efi`
runs.

**This requirement must not be lost in the axl-sdk port.** Hence this
substrate.

---

## 2. How EDK2 SoftBMC did it: a TSR (and why that's the right word)

The EDK2 SoftBMC RemoteShell is, precisely, a DOS-style **Terminate-and-
Stay-Resident** program (think Sidekick), expressed in UEFI:

| DOS TSR (Sidekick) | EDK2 SoftBMC (`Core/ConsoleWrapper.c`, `ShellLauncher.c`) |
|---|---|
| Go resident; hand the foreground to the running program | `gBS->StartImage(Shell.efi)` — **blocks**; the Shell owns the foreground |
| Hook **INT 16h** (keyboard read) | Wrap `gST->ConIn->ReadKeyStroke` → pump + key-ring pop |
| Hook **INT 10h / screen writes** | Wrap `gST->ConOut->OutputString` (+ cursor/clear/attr) → emit ANSI to a listener |
| Hook **INT 1Ch** (timer tick) heartbeat | A periodic timer event (`mConsolePollTimer`) |
| Do work inside the hook | `MyReadKeyStroke()` → `PollServices()` (pump the HTTP server) before returning a key |
| Inject/peek keystrokes | `ConsoleWrapperPushKey()` → ring → popped by the wrapped read; signals `WaitForKey` |

The HTTP server got CPU from **inside** the Shell's `ReadKeyStroke`,
from an output-call counter, and from the timer. ConOut writes were
mirrored to web terminals as ANSI; web keystrokes were pushed into a
ring the wrapped ConIn returned. That is a textbook TSR: resident code
that hands the foreground to another program and steals cycles by
hooking the I/O calls that program makes.

---

## 3. The control-flow tension with axl-sdk (and the cleaner pump)

axl-sdk's model is the **inverse** of the TSR arrangement:

- **TSR model:** the *Shell* owns `main` (`StartImage` blocks); SoftBMC
  is resident, *pumped by* the Shell's I/O. Control flows shell → hooks.
- **axl-sdk model:** *your app* owns `main` and runs `axl_loop_run`; you
  are the foreground and pump everything from your own loop. Control
  flows loop → handlers.

You cannot have both `axl_loop_run` and `StartImage(Shell)` blocking the
same thread. To host a real Shell, the foreground must go to the Shell —
which means **the loop must be pumped some other way while the Shell
blocks.**

**Key point: axl-sdk already has the pump primitive — and it's cleaner
than the ReadKeyStroke hook.** `axl_loop_attach_driver(loop, tick_ms)`
dispatches the loop from a firmware periodic timer (the resident-driver
model). So the run shape is:

```
axl_console_mirror_install(&m, &cfg);     // wrap gST ConIn/ConOut
axl_loop_attach_driver(loop, tick_ms);    // network pumped by the timer, in the background
status = axl_shell_launch();              // StartImage(Shell.efi) — blocks; Shell owns foreground
axl_loop_detach_driver(loop);
axl_console_mirror_uninstall(m);
```

The HTTP/WS server runs off the timer heartbeat; the Shell runs in the
foreground; the mirror pipes ConOut→sink and injects keys→ConIn. No
`ReadKeyStroke` hook is needed *for the pump* (the timer does it),
though the mirror still wraps ConIn for **key injection**. This is the
EDK2 behavior with a cleaner, axl-native heartbeat.

### 3.1 The TPL hazard (must design around)

The attached-driver loop dispatches at **`TPL_CALLBACK`** (raised). Any
loop handler that needs `TPL_APPLICATION` breaks — exactly the
synchronous-TLS-handshake-nesting-an-ephemeral-loop bug already fixed in
this tree (the resident-loop TLS fix). So:

- The WS/HTTP server feeding the mirror must use the **async** send/recv
  paths (no nested ephemeral `axl_loop_run` at raised TPL).
- HTTPS under the timer-pumped loop is the known-sharp case; it works
  with the async handshake but is the first thing to regression-test.

This is a real constraint, not a blocker — it's the same envelope the
shared-driver / resident-service model already lives in.

---

## 4. Layering: what's substrate vs. what's the consumer's

**axl-sdk substrate (this design):**
- `AxlConsoleMirror` — wrap `gST->ConIn`/`ConOut`(`/StdErr`), translate
  console output to a byte sink (ANSI), inject keys into ConIn, report
  remote dimensions, alt-screen. The reusable, finicky firmware surgery.
- `axl_image_run(path, args, *exit)` — the generic foreground launcher:
  load a blocking UEFI app, run it to completion (`StartImage` blocks),
  unload. Nothing Shell-specific — works for any blocking app (a diag
  tool, a vendor setup app, a recovery menu). `axl_shell_launch()` is the
  thin Shell wrapper over it: locate `Shell.efi` (the EDK2 `ShellLauncher`
  search) + run with `-nostartup`. The mirror hosts *whatever* runs in the
  foreground; the Shell is just the canonical (and hardest) case.
- The pump is **existing** substrate (`axl_loop_attach_driver`).

**Consumer policy (SoftBMC):**
- Bridging the mirror's byte sink ⇄ a per-client WebSocket terminal
  (xterm.js), session/RBAC, which shell, when to launch.
- Deciding terminal size from the browser and calling
  `axl_console_mirror_set_size`.

Layering rule (per `AXL-Pointer-Cursor-Design.md`): substrate owns the
shared system resource (the one global console) and reusable mechanism;
the consumer owns transport and policy.

---

## 5. The `AxlConsoleMirror` interface (contract)

`<axl/axl-console-mirror.h>` (new). Standard C types; no EFI in the API.

```c
typedef struct AxlConsoleMirror AxlConsoleMirror;

/// Console output, already translated to a terminal byte stream (UTF-8 +
/// ANSI/VT control sequences) suitable for xterm.js / a VT100 terminal.
typedef void (*AxlConsoleSinkFn)(const char *bytes, size_t len, void *user);

typedef struct {
    AxlConsoleSinkFn sink;            ///< receives the mirrored output stream
    void            *user;            ///< sink context
    uint32_t         cols;            ///< remote terminal width  (e.g. 80)
    uint32_t         rows;            ///< remote terminal height (e.g. 25)
    bool             passthrough_local; ///< also write to the physical console (default true)
} AxlConsoleMirrorConfig;

/// Install: save gST->ConIn/ConOut(/StdErr), swap in the wrappers, and
/// route output to cfg->sink. From here the firmware (and any app the
/// caller StartImages, incl. the Shell) talks to the wrappers.
int  axl_console_mirror_install(AxlConsoleMirror **out, const AxlConsoleMirrorConfig *cfg);

/// Restore the original console protocols. Always pair with install.
void axl_console_mirror_uninstall(AxlConsoleMirror *m);

/// Inject one keystroke (from the remote terminal) — UEFI shape:
/// printable keys set unicode (scan 0); special keys set scan (unicode 0),
/// e.g. SCAN_UP=0x01 … SCAN_F2=0x0C, SCAN_ESC=0x17. Pushed into the ConIn
/// ring and the WaitForKey event is signalled so a blocked reader wakes.
int  axl_console_mirror_inject_key(AxlConsoleMirror *m, uint16_t scan, uint16_t unicode);

/// Inject a run of terminal input bytes (xterm/VT) — decodes CSI/SS3
/// escape sequences (arrows, F-keys, Home/End/PgUp/PgDn, Del) and UTF-8
/// printables into the keystrokes above. This is the reusable, finicky
/// half that makes `edit` usable from a browser; consumers feed raw
/// xterm.js keydown bytes here.
int  axl_console_mirror_inject_text(AxlConsoleMirror *m, const char *bytes, size_t len);

/// Update the remote terminal size (browser resize). The wrapped
/// QueryMode/Mode report this, so full-screen apps size themselves to
/// the web terminal, not the physical console.
void axl_console_mirror_set_size(AxlConsoleMirror *m, uint32_t cols, uint32_t rows);

/// Reset per-session state (key ring, cursor/attr tracking, leave alt
/// screen) between Shell restarts or a new client.
void axl_console_mirror_reset(AxlConsoleMirror *m);
```

### 5.1 What the wrapped `ConOut` must translate (for `edit`)

A one-shot command only needs `OutputString`. `edit` needs the full set
→ VT/ANSI:

| Simple Text Output op | Emitted to the sink |
|---|---|
| `OutputString` | UTF-8 text (UCS-2 → UTF-8) |
| `SetCursorPosition(col,row)` | `ESC[<row+1>;<col+1>H` |
| `ClearScreen` | `ESC[2J` `ESC[H` (+ track cleared attr) |
| `SetAttribute(fg/bg)` | `ESC[` SGR (map EFI text colors → SGR 30-37/40-47/1) |
| `EnableCursor(on/off)` | `ESC[?25h` / `ESC[?25l` |
| `QueryMode(n)` / `Mode->Columns/Rows` | report `cfg->cols/rows` (mode 0), so `edit` sizes to the web terminal |
| `SetMode` | accept the matching mode; no-op otherwise |
| (full-screen app start) | `axl_console_mirror_reset` / first ClearScreen → optional alt-screen `ESC[?1049h` so `edit`'s clear doesn't dump into xterm scrollback |

Cursor/attribute state is tracked so re-attaching a client (or a
mid-stream resize) can be reasoned about; v1 may keep this minimal.

### 5.2 What the wrapped `ConIn` / `ConInEx` must do

- `ReadKeyStroke` / `ReadKeyStrokeEx`: return the next injected key from
  the ring; else fall through to the physical ConIn (local keyboard still
  works).
- `WaitForKey` / `WaitForKeyEx`: an AXL-owned event, **signalled by
  `inject_key`** so a Shell blocked in `WaitForEvent` wakes on remote
  input. (This is why injection is an event-signalling ring, not a poll.)
- `Reset`: drain the ring.

---

## 6. Proof: the test plan (the part that matters)

The feature is only real if **`edit` works over the mirror.** The test
ladder, each rung a QEMU integration test (real OVMF + a real
`Shell.efi` staged on the ESP — available from the edk2 ShellPkg build):

1. **One-shot sanity.** Install the mirror with a sink that captures the
   byte stream; `axl_shell_launch()` a Shell running `-nostartup`; inject
   `"ver\r"`; assert the captured stream contains the Shell's version
   banner. Proves ConOut text mirror + ConIn injection + the wake path.

2. **Cursor / clear / attr.** Inject `"cls\r"` then a command that
   positions the cursor; assert the stream carries `ESC[2J` and a
   `ESC[..H` cursor move. Proves the VT translation.

3. **Full-screen `edit` (the headline proof).** Stage a file, inject
   `"edit t.txt\r"`, then type a known line, then the save key (F2) and
   exit (Esc/Ctrl-Q via the documented scan codes). Assert:
   - the stream shows alt-screen / full-screen framing and the editor's
     title/status row,
   - the typed text appears at the tracked cursor position,
   - after exit, **reading back `t.txt` shows the typed line** — i.e. the
     interactive editor actually ran and saved over the wire.

   This is what a command-shell can never pass, and is the acceptance
   bar for the feature.

4. **Arrow / special-key decode.** Feed raw xterm sequences
   (`ESC[A`, `ESC[1;5C`, F-keys) via `inject_text`; assert they reach the
   app as the right `SCAN_*` codes (a small harness app echoing
   `ReadKeyStrokeEx` results is enough — no Shell needed).

5. **Pumped-loop coexistence.** With `axl_loop_attach_driver` driving a
   server in the background, run rung 1 and assert the server still
   answers a request *while* the Shell is foreground — proving the timer
   pump + foreground Shell coexist (and exercising the TPL envelope).
   **Rung 5 must be HTTPS, not plain HTTP:** SoftBMC is HTTPS-only, so
   the deployment-faithful case is a TLS handshake completing under the
   `TPL_CALLBACK` pump (the async-handshake path from `4c5977b8`). The
   coexistence *spike* (the concurrency linchpin) was proven with plain
   HTTP — `test/integration/test-shell-coexist-qemu.sh` — and the
   resident-loop TLS handshake is independently covered by
   `test-https-driver-qemu.sh`; rung 5 proper is the *combination*
   (HTTPS server + foreground Shell), a P3 deliverable.

Both arches. Rungs 1-2 + 4-5 are deterministic; rung 3 is the
real-interactive proof and may be marked the gating "real shell" test.

---

## 7. Risks / open questions

- **`Shell.efi` availability in CI.** OVMF can boot the edk2 ShellPkg
  `Shell.efi`; the harness must stage it on the ESP (like the iPXE/driver
  test bundles). If a given runner lacks it, rung 3 degrades to a
  topology-gated SKIP (balanced both arches) rather than a hard failure.
- **Global `gST` surgery.** Install/uninstall must be exception-safe and
  idempotent; a crash mid-session must restore the console (atexit hook /
  registry). EDK2's wrapper carries re-entrancy guards (`InPoll`) — port
  the lesson.
- **TPL envelope** (§3.1) — async-only server paths under the pumped
  loop; HTTPS is the regression-sensitive case.
- **Key decode completeness.** `edit` needs arrows, F1-F12, Esc, Del,
  Home/End, PgUp/PgDn. The `inject_text` xterm decoder must cover the
  CSI/SS3 set xterm.js emits; start from the EDK2 RemoteKvm `KeysymMap`
  as the reference table.
- **One global console = one session.** The mirror is a single global
  resource; concurrent web terminals share one Shell view (as EDK2 did).
  Per-client independent shells would need multiple console instances —
  out of scope; documented as a single-session mirror.
- **Late-join screen state.** A client connecting mid-session sees a
  blank terminal until the foreground app's next full redraw — the
  mirror streams *deltas*, not a framebuffer. EDK2 replayed a ~64 KB
  scrollback ring to new clients, but a raw replay is messy under
  `edit`'s alt-screen (absolute-positioned escapes replayed out of
  context corrupt the view). The cleaner primitive is a **tracked-screen
  snapshot**: the mirror already tracks cursor/attr; extending that to a
  cell grid would let a new client request "re-emit the current screen"
  as a clean repaint. **v1 leaves this to consumer policy** (SoftBMC may
  keep its own scrollback ring and replay on WS connect); a
  `axl_console_mirror_snapshot()` primitive is a candidate follow-up if
  the snapshot approach wins. Documented, not built in P1–P3.
- **Input fan-in across clients.** Multiple web terminals *and* the local
  keyboard all inject into the one ConIn ring (EDK2 did the same). That's
  fine mechanically, but there is no per-client input ownership: keys
  from any source interleave into the single Shell. Between *sessions*
  (a client disconnect, or a new client taking over),
  `axl_console_mirror_reset` drains the ring and clears tracking so stale
  input/attributes don't bleed across — the consumer decides when a
  "new session" begins. Concurrent *simultaneous* drivers are a shared
  keyboard by design, not a bug.

---

## 9. Security (consumers MUST read)

A mirrored real UEFI Shell is **full pre-OS control over the wire**:
arbitrary firmware access, raw block-device and filesystem read/write,
driver load/unload, NVRAM edits, `dmpstore`, flash tools — everything a
person at the physical console could do, exposed to whoever can reach the
sink's transport. This is the most powerful surface in the whole system.

The substrate deliberately ships **no** authentication or authorization
— it is pure mechanism. **Consumers MUST gate it hard** before bridging
the sink to any network transport:

- Strong authentication on the channel that carries the mirror (SoftBMC:
  the already-authenticated WebSocket upgrade — never an unauthenticated
  WS).
- Authorization: admin-/operator-only, and (SoftBMC) license-gated.
- Transport encryption end to end (SoftBMC: HTTPS/WSS only — see rung 5).
- An explicit, audited "open remote shell" action — not an ambient
  always-on bridge.

Shipping the mirror behind a weak or missing gate is a remote firmware
compromise. This is called out next to the feature precisely because the
power is easy to under-estimate.

---

## 8. Phasing (all DONE)

- **P1 — core mirror + one-shot proof. DONE.** `AxlConsoleMirror`
  install/uninstall, ConOut text + cursor + clear + attr translation,
  ConIn ring + WaitForKey injection, `inject_key`, dimensions.
  `axl_shell_launch()` (now a thin wrapper over the generic
  `axl_image_run`). Proven by the isolated self-test
  (`test-console-mirror-qemu.sh`, rungs 1-2 + the injection round-trip).
- **P2 — full-screen + keys. DONE.** QueryMode size override and the
  `inject_text` xterm CSI/SS3 decoder were folded into P1 (the §5
  contract committed to them). Rung 3 (`edit`) is the acceptance gate —
  `test-console-mirror-edit-qemu.sh` runs the real full-screen editor
  over the mirror and reads the saved file back. Alt-screen enter/leave
  is the one piece deferred (see §7 "Late-join"); not needed for `edit`.
- **P3 — pumped-loop integration. DONE.** `axl_loop_attach_driver` +
  foreground Shell + background **HTTPS** all coexist —
  `test-console-mirror-https-qemu.sh` (rung 5), the SoftBMC-faithful
  shape. Plain-HTTP coexistence is the spike
  (`test-shell-coexist-qemu.sh`).

**Implementation notes worth keeping:**
- The positive proofs run in **isolated boots**, not the combined unit
  suite: installing the mirror wraps the console the parent harness Shell
  uses, which wedges that Shell for the next test binary even after a
  clean uninstall (a firmware-lifecycle hazard). The self-tests print
  results *after* uninstall, then idle. Unit coverage is safe negatives.
- `edit` quirks the live run surfaced: **F2 (Save) opens a `File to
  Save:` dialog that needs ENTER** to commit; UEFI `edit` saves new files
  as **UTF-16LE** (the "UNICODE" file type), so the read-back strips NULs.
  Injection is milestone-gated (watch the sink for the prompt / editor /
  save-dialog) rather than fixed-delay, for runner-speed robustness.

The §5 contract was reviewed with the user before P1; an independent
mid-point review of the core hardened it (QueryMode-current-mode,
TPL-guarded ring, end-of-call escape flush). Remaining work is
consumer-side (the sink⇄WebSocket bridge).
