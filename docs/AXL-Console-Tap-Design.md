# AXL Console Tap -- Design

**Status:** SHIPPED (2026-07-09, `3e132336`; widened to a two-producer
contract 2026-07-10, `c6b919cd`). `axl-console-tap` is the **swap** producer
in the console family -- see `AXL-Console-Design.md` for the umbrella (the
`AxlConsoleOps` contract, the producer/consumer map, and the insertion-
strategy axis this doc is one leaf of).

**Shipped surface:** `<axl/axl-console-tap.h>` (`src/util/axl-console-tap.c`).
Consumed today by `axl-console-mirror` (`src/util/axl-console-mirror.c`, ops
-> VT wire for SoftBMC) and by AGT's `axterm` (ops -> `AgtTerminal` grid,
directly, no VT round trip).

---

## 1. What the tap is, and why

The tap wraps the firmware console -- `gST->ConOut`/`ConIn`(/`StdErr`) and
the `ConsoleInHandle`'s `SimpleTextInputEx` -- and reports every console
*output* call as a structured `AxlConsoleOps` operation
(`clear_screen`/`set_cursor`/`output_text`/`set_pen`/...), not a VT/ANSI byte
stream. `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` has no wire format of its own -- a
full-screen app just calls `SetCursorPosition`/`OutputString`/`SetAttribute`
-- so ops are the honest, lossless representation of what the app actually
did; a consumer that wants VT bytes (SoftBMC) serializes them itself, and a
consumer that wants a cell grid (AGT) binds them straight in, with no
encode -> parse round trip.

*Input* runs the other way: a consumer injects a keystroke, the tap pushes
it into the console's key queue, and a Shell blocked in `WaitForEvent`
wakes up believing a real key arrived.

This is one of two ways axl-sdk knows how to splice a producer into the
live firmware console (the "insertion strategy" axis in
`AXL-Console-Design.md` Sec4): the tap works by **swapping the `gST`
pointers**. The other strategy, `axl-console-device` (design-stage), fans
in as a ConSplitter virtual device instead and never touches `gST->ConOut`'s
value -- see Sec7 below for why that distinction matters operationally, not
just architecturally.

The tap is a straight simplification of the EDK2 SoftBMC `ConsoleWrapper`
(itself descended from TelCon): same firmware surgery, but no HTTP polling
and no owned timer -- the pump is whatever loop the caller is already
driving (`axl_loop_attach_driver` in the common case), consistent with
axl-sdk's "caller owns the loop" model documented in
`AXL-Console-Mirror-Design.md` Sec3.

```c
static void grid_put(void *u, const char *utf8, size_t n) { term_feed(u, utf8, n); }
static void grid_pen(void *u, const AxlConsolePen *p) { term_set_pen(u, p); }
static int  grid_prop(void *u, AxlConsoleProp prop, const AxlConsoleValue *v) {
    if (prop == AXL_CONSOLE_PROP_ALT_SCREEN) term_alt_screen(u, v->u.boolean);
    return 1;   // accept everything, including what we ignore
}
static const AxlConsoleOps ops = {
    .output_text = grid_put, .set_pen = grid_pen, .set_term_prop = grid_prop,
};

AxlConsoleTap *tap;
AxlConsoleTapConfig cfg = {
    .cols = 80, .rows = 25,
    .passthrough_local = false,   // we render it; don't paint the firmware console
    .auto_alt_screen   = true,    // a nested full-screen app enters the alt buffer
    .input_capture     = true,    // WE own the key queue; only injected keys reach it
};
axl_console_tap_install(&tap, &ops, my_term, &cfg);
axl_loop_attach_driver(loop, 10);   // pump our loop while the Shell blocks
axl_shell_launch(NULL);             // real Shell in the foreground; blocks
axl_console_tap_uninstall(tap);
```

---

## 2. The `AxlConsoleOps` contract

Defined in `<axl/axl-console-tap.h>` (its permanent home is the shared
`axl-console-ops.h` split described in `AXL-Console-Design.md` Sec1 --
a planned refactor, not done yet, so today every producer/consumer includes
the tap header for the contract alone). See the header for the full op
list and field-level docs; this section covers only what's load-bearing.

The vtable is shaped for **two producers that must speak the same shape**:
the tap (observing `EFI_SIMPLE_TEXT_OUTPUT` calls, no cell grid, ever) and
`axl-vterm` (parsing a real VT byte stream via libvterm). Because the tap
has no grid to pull damage from, a pull-style contract (libvterm's own
Layer3) is one the tap could never implement -- the vtable is a push
op-stream instead, and every op is optional (`NULL` = ignore).

Of the whole vtable, exactly two ops return `int`, and both returns are
load-bearing rather than decorative:

- **`scrollrect(user, rect, downward, rightward) -> int`.** Non-zero means
  the consumer handled the scroll itself (e.g. a GOP blit); zero declines,
  and the caller falls back to `moverect` + `erase`. The tap never calls
  this (it has no scroll notion of its own to report) -- it exists for
  `axl-vterm`.
- **`set_term_prop(user, prop, val) -> int`.** Non-zero accepts the
  property. This is the one most likely to be got wrong: libvterm only
  *stores* a property's new value if the callback accepted it -- reject
  `AXL_CONSOLE_PROP_ALT_SCREEN` and libvterm's internal alt-screen state
  never flips, desyncing the parser from the grid. The tap ignores this
  return for its own alt-screen assertion (Sec4) because it has no grid to
  keep in sync with a consumer; `axl-vterm` cannot ignore it.

Every other op returns `void`; resist "uniformity" pressure to make these
two `int` as well -- a `return 1` boilerplate on every callback would hide
the two that actually matter from a reviewer.

**What the tap actually produces.** Of the full vtable the tap only ever
calls `set_cell_rule` (once, at install), `clear_screen`, `set_cursor`,
`output_text`, `set_pen`, `set_mode`, and `set_term_prop` (cursor
visibility and alt-screen only -- it never emits blink/title/icon/reverse/
shape/mouse/focus-report). It never calls `erase`, `moverect`,
`scrollrect`, `bell`, or `clear_scrollback` -- those exist for `axl-vterm`,
which parses a real byte stream carrying real erase/scroll/bell sequences.
A consumer bound only to the tap can leave those five NULL.

---

## 3. Config: `AxlConsoleTapConfig`

```c
typedef struct {
    uint32_t cols;
    uint32_t rows;
    bool     passthrough_local;
    bool     auto_alt_screen;
    bool     input_capture;
} AxlConsoleTapConfig;
```

A zeroed config is safe and behaves like the untouched firmware console
(passthrough on, no alt-screen heuristic, physical keyboard still
readable).

- **`cols`/`rows`.** The consumer's terminal geometry. `0` falls back to the
  physical console's current `QueryMode` size. The wrapped `QueryMode`
  overrides *only the current mode's* geometry with these values, so a
  full-screen app sizing itself via `QueryMode(Mode->Mode)` lays out for
  the consumer's terminal, not the physical one; other mode numbers pass
  through untouched so the app's mode enumeration stays truthful.
- **`passthrough_local`.** Also write to the physical console. When
  `false`, the tap is the *only* console writer, which has a real
  consequence beyond "don't paint twice": the tap must then own and
  maintain `SIMPLE_TEXT_OUTPUT_MODE` itself (cursor row/column, attribute,
  visibility) by hand-tracking every `OutputString`/`SetCursorPosition`
  call the same way the reference console driver would -- because with
  passthrough off, nothing else ever calls the real driver to keep that
  state current, and a nested Shell reading a frozen `Mode` back would
  overwrite its own output. With passthrough on, the real driver keeps its
  own `Mode` current and the tap keeps aliasing it unchanged.
- **`auto_alt_screen`.** Heuristically enter the alternate screen on
  `ClearScreen`, leave it once the cursor scrolls past the last row. For a
  consumer hosting an opaque nested full-screen app (axterm hosting the
  Shell's `edit`); a consumer that owns its own TUI leaves this `false` and
  drives `axl_console_tap_enter_alt_screen`/`_leave_alt_screen` explicitly.
- **`input_capture`.** The wrapped `ConIn`/`ConInEx` serve *only* injected
  keys, and `WaitForKey[Ex]` signals only on injected content -- the
  physical key queue is never read by the wrapped reader. Set this when
  the consumer drains the firmware key queue itself (AGT's keyboard
  handling), so the guest can't read a key twice or steal one meant for
  the consumer's own UI. It also moves key-*notify* ownership onto the
  tap -- see Sec5.

---

## 4. Output contract detail

`output_text` carries UTF-8 decoded from the console's UCS-2, with the C0
control range filtered (`tap_sanitize_char`): it keeps everything `>= 0x20`
plus `{BS, TAB, LF, CR}` and substitutes `'?'` for the rest of C0, `ESC`
included (NUL never reaches it -- it terminates the string). Filtering the
escapes serves two ends at once: a UEFI app cannot push VT escapes through
`OutputString` in the first place, and forwarding `ESC` verbatim would hand
any app that prints a user-controlled string an escape-injection vector into
the consumer's terminal or grid. Unlike EDK2 `TerminalDxe`'s ASCII-only wire
(`0x20..0x7F`), non-ASCII BMP text (box drawing, CJK) *does* pass through --
the console is UCS-2 and the op is UTF-8, so this op carries what the wire
could not.

The four C0 controls that survive -- BS, TAB, LF, CR -- are **not**
interpreted by the tap; they ride inside the `output_text` run verbatim.
The consumer owns cursor semantics for them (backspace steps back, CR
homes the column, LF advances the row and must scroll the grid when the
cursor is already on the last row). The tap tracks this same state
internally, in parallel, only to keep its *own* `SIMPLE_TEXT_OUTPUT_MODE`
correct when it owns the console (`passthrough_local=false`, Sec3) -- that
internal tracking is not exposed as a separate op, and a consumer must
still do its own C0 handling on the bytes it receives.

The tap reports its cell-boundary rule once, at install
(`set_cell_rule`), as `AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT`:
`EFI_SIMPLE_TEXT_OUTPUT` is one-cell-per-character by construction, and a
double-width CJK codepoint occupies exactly one cell on the firmware
console -- re-widening it in the consumer would desync the grid from what
the guest believes it drew. `axl-vterm`, parsing a real VT stream, reports
`AXL_CONSOLE_CELLS_WIDTH_RESOLVED` instead and expects the consumer to
apply `axl_vterm_char_width`. A consumer that ignores `set_cell_rule`
entirely must assume one-per-codepoint.

---

## 5. Input path

**`axl_console_tap_inject_key_ex(t, scan, unicode, shift_state,
toggle_state)`** is the general injection call; `axl_console_tap_inject_key`
is the no-modifier convenience wrapper over it. The key is pushed into the
wrapped `ConIn` ring (`EFI_KEY_DATA`, not just `EFI_INPUT_KEY` -- the ring
carries the full key state) and `WaitForKey`/`WaitForKeyEx` are signalled,
so a Shell blocked in `WaitForEvent` wakes.

**Reaching a guest's modifier-qualified key notify is why `_ex` exists at
all.** UEFI fires `RegisterKeyNotify` callbacks at queue-*insert* time, and
the match rule (UEFI 2.11 Sec12.2.5) treats a registered `KeyShiftState`/
`KeyToggleState` of `0` as "don't care" and any non-zero value as an exact
match. The Shell registers its Ctrl+C break four ways -- `UnicodeChar` in
`{'c', 3}` crossed with `{LEFT,RIGHT}_CONTROL_PRESSED`, each OR'd with
`AXL_CONSOLE_SHIFT_STATE_VALID` -- so a bare `unicode=3` with no shift
state will *not* trigger it; the caller must inject
`unicode=3, shift_state = AXL_CONSOLE_SHIFT_STATE_VALID |
AXL_CONSOLE_LEFT_CONTROL_PRESSED`. The `AXL_CONSOLE_*` shift-state bits are
AXL-owned aliases of the `EFI_KEY_STATE.KeyShiftState` constants (a unit
test pins each to its `EFI_*` counterpart so they cannot drift) -- they
exist purely so a tool that includes only `<axl.h>` can name a modifier
without a UEFI header leaking through the public surface.

**Who owns the notify registry follows who owns the queue.** Without
`input_capture`, the firmware still owns the key queue, so
`RegisterKeyNotify`/`UnregisterKeyNotify` simply forward to the original
`SimpleTextInputEx`, and the firmware fires its own notifies on physical
keys as always -- an injected key is readable but triggers nothing. With
`input_capture` set, the tap *is* the queue (a fixed 16-slot table,
`KEY_NOTIFY_MAX`, sized for the Shell's own 4+4 Ctrl+C/Ctrl+S
registrations with headroom), so it must also own the registry and fire
notifies itself from the insert path -- forwarding registrations while
also owning the queue would mean an injected key breaks nothing (nothing
ever inserts into the firmware's queue) while a stray physical keystroke
still fires the guest's notify, exactly backwards from what capture mode
promises.

**Simple-read Ctrl+letter folding.** `EFI_SIMPLE_TEXT_INPUT.ReadKeyStroke`
carries no `KeyState`, so a Ctrl-qualified letter must be folded to its C0
control code (Ctrl+A=1 .. Ctrl+Z=26) for a line editor reading the Simple
protocol to see it as a control action rather than a bare letter it would
insert. The tap folds this only on the injected path
(`simple_fold_ctrl_letter`, gated on `EFI_SHIFT_STATE_VALID` so a stale
control bit never mis-folds a normal letter) -- a physical key from the
real `ConIn` was already folded by the firmware's own `ConSplitter`. A
full-screen app reading the Ex protocol instead (the Shell's `edit`) still
gets the plain letter plus `KeyState`, so injecting the letter with the
control shift-state bit set (not the pre-folded C0 code) satisfies both
readers at once.

**`ConsoleInHandle`'s `SimpleTextInputEx` is reinstalled, not just
wrapped by pointer assignment.** `edit`/`hexedit` fetch
`SimpleTextInputEx` directly via `HandleProtocol(gST->ConsoleInHandle,
...)`, bypassing `gST->ConIn` entirely -- wrapping only the `gST` field
would leave a full-screen editor's keyboard dead. The tap does
`gBS->ReinstallProtocolInterface(ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid,
orig, &my_coninex)` during install, *before* swapping the `gST` pointers
(the reinstall can itself trigger ConSplitter to rewrite `gST->ConIn`, so
doing it first keeps the final swap authoritative), and reinstalls the
original back on uninstall. This is best-effort: absence of the protocol
on that handle is logged and non-fatal, not an install failure.

This reinstall-and-fold pair is prior art, not a tap invention -- TelCon
(`TelConPkg`, predates SoftBMC, which derived its `ConsoleWrapper` from
it) independently hit and fixed both: `ConsoleWrapper.c:1188` reinstalls
`SimpleTextInputEx` on `ConsoleInHandle` before the `gST` swap, and
`ConsoleWrapper.c:330-337` passes a C0 control code through as
`UnicodeChar` so `edit`'s `MenuBarDispatchControlHotKey()` (which indexes
its command table directly by `UnicodeChar`) sees the right key. The tap's
version differs only in shape: TelCon pre-folds to C0 on the wire; the tap
injects letter + shift-state and folds per-protocol at read time, so one
injected keystroke satisfies both the Shell's line editor (Simple, folded)
and `edit` (Ex, unfolded + `KeyState`) without the caller choosing which
protocol will read it.

**`axl_console_tap_inject_text(t, bytes, len)`** decodes a run of raw
xterm/VT input bytes -- CSI/SS3 escapes for arrows, F-keys, Home/End,
PgUp/PgDn, Delete, plus UTF-8 printables -- into the keystrokes above, for
a consumer whose own input source is already byte-shaped (a remote
terminal). Each call is self-contained: an escape sequence left
incomplete at end-of-call is flushed as a bare Esc plus its literal body
bytes rather than held into the next call, so a dropped final byte can't
splice onto and corrupt a later keystroke. Bytes decoded this way carry
`KeyState` 0 (no modifier), which still fires a notify whose own
registered shift state is 0 but cannot reach a Ctrl-qualified one --
that's what `inject_key_ex` is for.

---

## 6. The insertion constraint (critical)

The tap's mechanism is "swap `gST->ConOut`/`ConIn`(/`StdErr`) to point at
our own protocol structs." That swap is only safe when it happens **before**
the console's next command-bracket save, in whichever program does the
installing:

- A **foreground application that installs the tap and then
  `StartImage`s a child `Shell.efi`** (axterm's model, and TelCon/SoftBMC's
  before it). The swap is in place before the child shell exists, so the
  child's first per-command save captures *our* pointer; every subsequent
  save == restore == our pointer, and nothing is ever freed.
- A **BDS-time driver** that swaps before any shell exists at all, for the
  same reason.

What is **not** safe: installing the tap from a **resident driver's
`DriverEntry`, loaded into an *already-running* Shell** (`load
conprov.efi` at an interactive prompt). This was proved, not assumed --
see AGT's `spike/README.md` ("Why the swap is deferred") and
`.superpowers/sdd/edk2-mechanism.md` for the symbolized backtrace under
DEBUG OVMF. In short: the Shell brackets *every* command with a
StdIn/StdOut/StdErr save and restore
(`ShellParametersProtocol.c:753`/`:1420`). If `gST->ConOut` differs from
the value it saved when a command ends, `RestoreStdInStdOutStdErr` assumes
*that command* installed a redirection wrapper and tears it down --
`CloseSimpleTextOutOnFile` calls `FreePool` on both the struct's `Mode`
and the struct itself (`ConsoleWrappers.c:521-522`). `load conprov.efi`
runs the driver's `DriverEntry` **inside** the `load` command's own
bracket; swapping `gST->ConOut` there makes the end-of-command restore
free the tap's static struct, which has no pool header, which trips
`ASSERT [DxeCore] Pool.c(721): Head->Signature == ...` and deadloops the
guest. Two controls pinned this to the Shell, not the tap or the firmware:
a bare `gST->ConOut = &fake` swap with zero AXL code reproduces the
identical assert, and `load -nc` (which skips the connect-all storm
entirely) still crashes, ruling out the earlier "console-reconnect storm"
theory.

Deferring the swap to land *between* command brackets (the spike's
`conprov.c` does this with a one-shot ~2.5s timer) works but is a timing
heuristic, not a guarantee -- there is no Shell "command boundary" event
to gate on. The deterministic fix for that shape is a **different
insertion strategy**, not a better delay: `axl-console-device` (design
stage; `AXL-Console-Design.md` Sec4, `AXL-Console-Device-Design.md`
planned) registers as a ConSplitter virtual console device instead of
swapping `gST->ConOut`'s value, so the Shell's `!=` check at restore time
is never true and nothing is ever freed -- at the cost of becoming one of
several console outputs rather than the sole writer.

**Practical rule:** use `axl-console-tap` from a foreground app that then
starts a child shell, or from BDS. Do not install it from a driver
`DriverEntry` that expects to interpose on a shell already running at the
prompt -- that shape needs `axl-console-device` once it ships, or a
timing-gated defer accepted as a spike-only hack.

---

## 7. Lifecycle

**Single global console => single tap.** There is exactly one live
`gST->ConOut`/`ConIn`, so the tap is a de facto singleton guarded by one
static instance pointer; a second `axl_console_tap_install` call while one
is active fails with `AXL_ERR` and leaves the existing tap untouched.

**Install order matters.** `axl_console_tap_install` saves the original
`ConOut`/`ConIn`/`StdErr` (StdErr is saved and restored independently of
ConOut -- they are separate `EFI_SYSTEM_TABLE` fields even in the common
case where they alias the same pointer), builds the wrapped `ConOut` (and,
unless `passthrough_local`, points its `Mode` at the tap's own owned
copy), publishes the singleton, reinstalls `SimpleTextInputEx` on
`ConsoleInHandle` (Sec5), and only then swaps the `gST` pointers -- in
that order, so the reinstall's possible ConSplitter side effects land
before the final swap, not after.

**Exit safety.** An `axl_atexit` hook restores the console automatically
if the process exits without an explicit `axl_console_tap_uninstall` --
the same lesson EDK2's original `ConsoleWrapper` encoded as a re-entrancy
guard: a crash mid-session must not leave the firmware pointing at a
struct that is about to disappear.

**`axl_console_tap_uninstall`** reverses install exactly: reinstalls the
original `SimpleTextInputEx` if the tap had reinstalled its own, restores
`ConOut`/`StdErr`/`ConIn` to the saved originals, closes the `WaitForKey`/
`WaitForKeyEx` events, removes the atexit hook, and frees the instance.
NULL-safe, and safe to call on an already-uninstalled or foreign handle
(it checks the handle against the live singleton before doing anything).

**`axl_console_tap_reset`** clears per-session state -- drains the key
ring, clears the escape-decoder state, leaves the alternate screen if in
it -- without a full uninstall/reinstall. Use it between Shell restarts or
when a new consumer attaches, so stale input or alt-screen state doesn't
bleed across sessions.

---

## 8. Consumers

**`axterm`** (AGT) binds the tap's ops directly into `AgtTerminal`'s cell
grid -- `output_text`/`set_cursor`/`set_pen`/`clear_screen`/`set_term_prop`
land straight in the grid with no VT encode/parse round trip in between.
It installs with `passthrough_local=false` (it owns the GOP; nothing else
should paint) and `input_capture=true` (AGT drains the physical keyboard
itself and injects into the hosted shell via `inject_key_ex`, carrying
Ctrl's shift-state bits so the Shell's own Ctrl+C notify fires).

**`axl-console-mirror`** (`src/util/axl-console-mirror.c`, SoftBMC) is
built *on top of* the tap, not as a separate producer: internally it holds
an `AxlConsoleTap*` and its own `AxlConsoleOps` implementation serializes
each op to a VT/ANSI byte stream for a remote xterm-class terminal. Its
public API (`axl_console_mirror_install`, `_inject_key`, `_inject_text`,
`_set_size`, ...) is a thin byte-stream-shaped wrapper over the
equivalent tap calls -- see `AXL-Console-Mirror-Design.md` for that
consumer's own design (VT translation table, the `edit`-over-the-wire
test ladder, the security posture required before bridging the sink to a
network transport).

**The tap has no pump of its own**, in either consumer. It never creates a
loop or a timer; the caller drives one and the tap's wrapped ConOut/ConIn
calls run synchronously inside whatever already calls them (the hosted
shell's own writes and reads). A resident driver reaches the tap's install
call from a timer tick via `axl_loop_attach_driver` (subject to the
insertion constraint in Sec6); a foreground app calls it directly before
blocking in `axl_shell_launch`.

---

## 9. Place in the console family

This doc covers `axl-console-tap` only. For the contract it implements,
the other producers/consumers that share it, and the insertion-strategy
comparison in full, start at `AXL-Console-Design.md`:

- `axl-console-device` (planned) -- the other producer, using the
  ConSplitter-registration insertion strategy instead of the `gST` swap;
  the deterministic alternative referenced in Sec6.
- `axl-console-mirror` (shipped) -- the VT-wire consumer described in
  Sec8, with its own design doc.
- `axl-vterm` (shipped) -- the other producer of `AxlConsoleOps`, parsing
  a real VT byte stream via libvterm Layer2 instead of observing
  `EFI_SIMPLE_TEXT_OUTPUT` calls.
