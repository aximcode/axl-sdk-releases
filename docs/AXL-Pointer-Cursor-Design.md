# AXL Pointer, Cursor & Interactive-Input — Design

**Status:** proposed. Three related substrate additions that turn AXL's
raw input + present primitives into the shared mechanism every
interactive `axl_gfx` consumer (a GUI toolkit like AGT, a game, a boot
menu) would otherwise re-invent:

1. **`AxlCursor`** — a software mouse-cursor compositor (the big one;
   needs a spike for present-pipeline integration).
2. **Click gestures** — double/triple-click and drag-threshold
   recognition over the existing pointer events.
3. **Auto-repeat** — held-button repeat (and an honest accounting of
   what keyboard repeat can and can't be on UEFI).

Layering rule (from the design discussion): **substrate owns shared
system resources and reusable mechanism; the toolkit owns widget policy
and interaction semantics.** A cursor on the screen, gesture timing, and
held-button ticks are mechanism. Selection, caret behavior, focus, and
paste-into-document are policy → they stay in AGT, built on these.

**The event loop is caller-owned.** AGT creates and drives the
`AxlLoop`; the substrate never spins up its own. Everything here that
needs a timer or event source (`AxlCursor` tracking, held-button repeat)
adds it to the loop the caller passes in — `axl_input_attach_mouse(loop,
…)` and `axl_cursor_attach(c, loop, …)` already take it — so AGT keeps
ownership of the run/quit lifecycle.

---

## 1. AxlCursor — software mouse-cursor compositor

### 1.1 Why substrate

The mouse cursor is a **single global screen resource**. Exactly one
sprite tracks the pointer; it must sit on top of whatever is presented;
and it must cooperate with the present/damage pipeline so moving it
doesn't smear or force a full-frame redraw. If each consumer drew its
own cursor (as `sdk/examples/pointer-demo.c` does today — recompositing
the whole background on every move), two consumers would fight over it
and every one would re-implement the same save/restore dance, badly.
GOP exposes **no hardware-cursor API** (the GPU usually has a cursor
plane, but UEFI surfaces only a framebuffer + Blt — no portable way to
drive it), so this is necessarily a software compositor — which is
precisely why it belongs in one shared place.

### 1.2 API sketch (contract — review before implementing)

```c
typedef struct AxlCursor AxlCursor;   /* opaque */

/* Create a cursor bound to the consumer's back-buffer "scene" — the
   source of truth for the pixels under the cursor. Pass NULL for a
   direct-to-screen consumer (save-under mode, §1.3). */
AxlCursor *axl_cursor_new(AxlGfxBuffer *scene);
void       axl_cursor_free(AxlCursor *c);

/* Set the sprite (RGBA, alpha-composited) and its hotspot. NULL sprite
   selects the built-in arrow AXL ships. */
int  axl_cursor_set_image(AxlCursor *c, const AxlGfxBuffer *sprite,
                          int32_t hot_x, int32_t hot_y);

void axl_cursor_show(AxlCursor *c);
void axl_cursor_hide(AxlCursor *c);

/* Move the hotspot to (x, y) in screen pixels; erases the old position,
   draws the new. Clamps to the framebuffer. Cheap — touches only the
   cursor region(s), not the whole frame. */
void axl_cursor_move(AxlCursor *c, int32_t x, int32_t y);

/* Convenience: attach to the physical pointer on a loop. Internally
   consumes axl_input_attach_mouse/_touch, calls axl_cursor_move on
   motion, and forwards every event to the consumer's callback (so the
   consumer still does hit-testing / widget logic). */
uint32_t axl_cursor_attach(AxlCursor *c, AxlLoop *loop,
                           AxlInputCallback cb, void *data);

/* Present bracket: the cursor must be lifted before the consumer
   re-presents its scene and dropped after, or it smears / hides content.
   See §1.3 for why this exists and the alternative. */
void axl_cursor_lift(AxlCursor *c);   /* restore under-pixels, mark hidden */
void axl_cursor_drop(AxlCursor *c);   /* re-composite on top */
```

The consumer still owns hit-testing and widget behavior; `AxlCursor`
only owns *the sprite on the screen* and *the current position*.

### 1.3 The crux: present-pipeline integration (spike target)

When the cursor moves A→B we must erase at A and draw at B; when the
consumer re-presents its frame the cursor must end up on top without
smearing. Three candidate mechanisms, in increasing order of
preference:

**Option A — full recomposite per move (the demo's approach).**
On each move the consumer re-presents the whole scene, then the cursor
draws on top. Simple, no saved pixels, but re-presents the entire frame
per mouse event and couples the cursor to the consumer's redraw. Reject
for production (fine as the demo baseline).

**Option B — save-under (classic software cursor).**
Keep a small "under" buffer the size of the sprite. To draw: capture the
screen pixels under the cursor rect (`axl_gfx_capture`) into the under
buffer, then alpha-composite the sprite. To move: restore the under
buffer to the old rect, then save+draw at the new rect. Only touches the
cursor region per move. **Hazard:** if the consumer presents a new frame
while the cursor is up, the saved pixels are stale → restoring them
paints old content. Requires the lift/drop bracket around consumer
presents. Works with NO back-buffer (direct-to-screen) — this is the
fallback mode (`scene == NULL`).

**Option C — back-buffer-bound (recommended primary).**
Bind the cursor to the consumer's back-buffer `scene`. The back-buffer
*is* the "what's under" truth, so no `axl_gfx_capture` is needed:
- erase at old rect → `axl_gfx_buffer_present_rect(scene, old_rect)`
  (re-blit the clean scene region);
- draw at new rect → composite the sprite over the scene region and
  present that rect.
When the consumer presents a new frame, the cursor re-draws on top
afterward (or the consumer uses a cursor-aware present). This reuses the
existing `present_rect` + dirty-rect (G18) machinery, needs no pixel
capture, and is cheap (two small rect presents per move). Most AXL gfx
consumers already use a back-buffer, so this is the common path; Option B
covers the direct-to-screen minority.

> **Refinement (flicker fix, post-spike).** The naive Option C above
> presents the erase (old rect) and the draw (new rect) as **two separate
> GOP writes**, and the lift/drop bracket erased the cursor from the GOP,
> flushed the scene, then redrew it. Both leave a brief frame where the
> screen shows the scene *without* the cursor — invisible at fast event
> rates but a visible flicker at a throttled poll (e.g. a 10 Hz BMC
> remote-console pointer). The fix matches what real software-cursor
> compositors do (wlroots `wlr_output_render_software_cursors`, Qt
> `QFbCursor`): composite the cursor **into the scene as its top layer**,
> then present **once** so scene+cursor land atomically. Concretely:
> `axl_cursor_lift` *folds* the sprite into the bound scene (saving the
> overwritten pixels) so the consumer's / compositor's single flush carries
> it; `axl_cursor_drop` *unfolds* (restores the saved pixels) to keep the
> scene byte-clean for the next partial-damage repaint; and a move presents
> the `old∪new` region in one atomic present when the rects overlap. Option
> B (save-under) keeps the two-write path — it has no scene buffer to fold
> into and is the rarely-used direct-to-screen fallback.

**What the spike must answer:**
- Does Option C's two-small-rect present per move read clean (no tearing
  / no trail) over a real GOP, at mouse-move event rates?
- Is the lift/drop bracket ergonomic, or should there be a cursor-aware
  `axl_cursor_present(c)` that the consumer calls instead of
  `axl_gfx_buffer_present`, hiding the bracket entirely?
- Confirm `axl_gfx_capture` round-trips for the Option B fallback
  (capture region → restore it pixel-exact).
- Sprite alpha compositing onto the scene region (reuse
  `axl_gfx_blit` / the source-over path).

Spike on a throwaway branch, viewed over reverse VNC + the
mouse-enabled OVMF (the `pointer-demo` rig), per the project's
spike-for-design-questions discipline. The API above is provisional
until the spike proves the integration reads clean.

### 1.4 Built-in arrow

AXL ships a default arrow sprite (a small RGBA bitmap with a
top-left-ish hotspot) so a consumer gets a usable cursor with zero
assets — mirroring `axl_gfx_default_font`.

---

## 2. Click gestures

### 2.1 Why substrate

Double/triple-click detection and the drag threshold are **pure
timing/distance logic** over events AXL already delivers, identical for
every pointer consumer. Putting them in the substrate means AGT, a file
picker, and a game all agree on what a "double-click" is.

### 2.2 API sketch

A small stateful recognizer fed the raw button events; it annotates them
with a click count and drag state. Exposed as an opt-in layer over
`axl_input_attach_mouse` so consumers that don't want it pay nothing.

```c
/* Tunables (sensible defaults; override globally). */
void axl_input_set_click_tuning(uint32_t multi_click_ms,   /* default 400 */
                                int32_t  drag_threshold_px); /* default 4   */

/* New event fields (populated on MOUSE_BUTTON_DOWN / _UP / MOUSE_MOVE):
     ev->click_count   1, 2, 3 on successive BUTTON_DOWNs within
                       multi_click_ms AND within drag_threshold_px of the
                       previous click; resets otherwise.
     ev->dragging      true once the pointer has moved past
                       drag_threshold_px with a button held, until release.
   These are additive AxlInputEvent fields; existing consumers ignore them. */
```

Recognizer state (last-click time, position, count; per-button held
origin) lives in the input layer. Triple-click = third within the
window; the window restarts on a click outside the threshold or after
timeout. Drag start fires once per press when motion crosses the
threshold (so a click with sub-threshold jitter is not a drag).

This is fully unit-testable: feed synthetic timestamped button events,
assert the click_count / dragging transitions — no firmware needed.

---

## 3. Auto-repeat — honest scope on UEFI

### 3.1 The constraint (verified against edk2 source)

**UEFI console input is press-only: there is no key-up event for normal
keys.** `EFI_SIMPLE_TEXT_INPUT_PROTOCOL.ReadKeyStroke` and the Ex
variant return a keystroke only on press; `EFI_KEY_STATE_EXPOSED`
partial keystrokes surface modifier/toggle *state changes*, not normal-
key releases (MdePkg `Protocol/SimpleTextInEx.h`). So a consumer above
SimpleTextIn *cannot* synthesize "repeat while held" — there is no
held signal to time against, and AXL reads keys via this layer.

But the **firmware already does keyboard auto-repeat**, and it reaches
consumers as repeated `KEY_DOWN` keystrokes. Confirmed in edk2:

- **USB** (`MdeModulePkg/Bus/Usb/UsbKbDxe`): the driver runs its own
  repeat timer — `USBKBD_REPEAT_DELAY = HZ/2` (0.5 s) then
  `USBKBD_REPEAT_RATE = HZ/50` (20 ms). It knows the held set from each
  HID boot report (which lists *all* currently-pressed keys), so on
  press it arms the timer and `USBKeyboardRepeatHandler` re-enqueues the
  key (`UsbKey.Down = TRUE`) at the repeat rate; when the report shows
  the key gone it detects the release and cancels the timer. ReadKeyStroke
  therefore returns the key, then repeats, then stops — no consumer
  key-up needed.
- **PS/2** (`MdeModulePkg/Bus/Isa/Ps2KeyboardDxe`): no driver-level
  repeat — it relies on the 8042 controller's *hardware* typematic
  (repeated make codes while held).

**Reliability nuance for AGT:** USB-keyboard repeat is deterministic
(driver-synthesized in the guest). PS/2 repeat depends on the hardware
controller / host typematic, and a VNC client sends a single key-down +
a key-up on release with **no client-side repeats** — so over VNC a PS/2
keyboard may not repeat, while a USB keyboard (UsbKbDxe) will. AGT (and
its QEMU/host) should prefer a USB keyboard where held-key repeat
matters. The repeat rate is firmware-fixed and not portably settable.

### 3.2 What the substrate *can* add: held-button repeat

The **pointer** is different: `EFI_SIMPLE_POINTER`/`AbsolutePointer`
`GetState` reports the *current* button state every poll, so we **know**
a button is still held. That makes held-button auto-repeat
synthesizable — and it's genuinely useful and not firmware-provided
(scrollbar arrows, spinner +/- buttons, press-and-hold to repeat).

```c
/* While a pointer button stays held, re-emit a synthetic
   MOUSE_BUTTON_DOWN (flagged ev->repeat = true) after delay_ms, then
   every interval_ms. 0 delay disables. */
void axl_input_set_button_repeat(uint32_t delay_ms,      /* default 400 */
                                 uint32_t interval_ms);  /* default 50  */
```

Implemented with a loop timer armed on BUTTON_DOWN and disarmed on the
button's release (which the pointer *does* report, unlike the keyboard).
Unit-testable with the mock SimplePointer + a fake clock.

### 3.3 Recommendation

- **Keyboard repeat:** document that it is firmware typematic (works
  today via repeated `KEY_DOWN`); provide no synthetic layer (can't be
  done correctly without key-up). If a consumer needs repeat where
  firmware gives none, the honest answer is a consumer-side timer keyed
  off its own "is this key still the active action" model — but the
  substrate shouldn't pretend to know a key is held.
- **Held-button repeat:** implement as above (the feasible, useful half
  of "auto-repeat").

This is the one place I'd push back on the original ask: "key
auto-repeat" as a substrate feature is mostly a no-op on UEFI; the value
is in **button** repeat. Flagging rather than silently shipping a
keyboard repeater that can't actually detect a held key.

---

### 3.4 Key debounce / repeat suppression (remote consoles)

The flip side of repeat, and a real consumer need. Over a high-latency
remote console — **iDRAC Virtual Console**, IPMI Serial-over-LAN, Intel
AMT — a single intended keypress routinely registers as *held* long
enough that the firmware's own typematic fires, so typing one character
yields several ("aaa"). This is a widely-reported iDRAC problem, and the
mechanism is **latency-induced typematic, not hardware bounce**: the
classic mitigation is the DEC disable-auto-repeat escape `ESC [ ? 8 l`
("turn typematic off"), and Dell's own guidance is about redirection
timing, not contact bounce.

The catch: **UEFI exposes no portable way to disable firmware typematic**
— there is no SimpleTextIn knob for repeat rate or on/off, and the repeat
is generated in the keyboard driver/controller below the protocol (§3.1).
So unlike a Linux tty, a UEFI app **cannot** ask the firmware to stop;
the only place to fix it is in **software, above SimpleTextIn**, by
filtering the unwanted repeats out of the key stream. This is a genuine
substrate addition — the "do it in software" instinct is right here, for
the *opposite* reason it was wrong for synthesizing repeat: we're
*removing* events we can see, not inventing ones we can't.

Same tension as repeat, resolved by policy rather than magic (a held
*navigation* key should repeat; a held *printable* over a laggy link
should not, and timing alone can't tell intended from latency-induced):

- A per-key **minimum accept interval** — drop a same-key KEY_DOWN that
  arrives within `min_repeat_ms` of the last *accepted* instance of that
  key. Different keys never interfere, so normal typing of different
  characters is untouched.
- **Char-aware** by default — apply the interval to **printable**
  characters (you essentially never hold a letter/digit on purpose) and
  exempt **navigation/editing** keys (arrows, Backspace, Delete,
  Page/Home/End) so held-key navigation still repeats.
- Opt-in, tunable, **off by default** (preserves today's behavior); a
  consumer turns it on for text-entry contexts.

```c
typedef struct { uint32_t last_keycode, last_unicode; uint64_t last_us; } AxlKeyDebounce;

/* true = deliver the KEY_DOWN, false = drop as a too-fast same-key
   repeat. Pure (reads ev->keycode/unicode/timestamp_us); updates d. */
bool axl_input_key_accept(AxlKeyDebounce *d, const AxlInputEvent *ev);

/* min_repeat_ms == 0 disables (default). printable_only (default true)
   exempts navigation/editing keys so they still repeat. */
void axl_input_set_key_debounce(uint32_t min_repeat_ms, bool printable_only);
```

`axl_input_attach_key` runs one internally when enabled; the pure core is
exported for reuse + deterministic unit tests (synthetic timestamped
events), mirroring the click-gesture recognizer.

**Set the default from data, not a guess.** Before fixing
`min_repeat_ms`, capture real keystroke inter-arrival times over an
actual iDRAC session: a tiny `keytrace` tool logging each KEY_DOWN's
char + microseconds since the previous one. That reveals whether the
duplicates cluster at the ~20 ms USB typematic rate or elsewhere, and
pins a window that suppresses them without clipping human typing
(same-key digraphs land in the ~80–300 ms range). Run it over iDRAC,
then choose the default.

## 4. Phases

1. **This doc + contract review** of the API sketches.
2. **Click gestures** — done (test-first, both arches).
3. **Held-button repeat** — test-first, both arches (in progress). No
   firmware-display dependency. (Keyboard repeat: firmware-owned per §3.1
   — documentation + a live typematic check, no synthetic layer.)
4. **Key debounce** — `keytrace` diagnostic first (run over a real iDRAC
   session to measure duplicate-keystroke timing), then the char-aware
   per-key min-interval filter test-first, with the default set from the
   captured data (§3.4).
5. **AxlCursor spike** — throwaway proving Option C (+ B fallback) over
   reverse VNC; settle the present bracket vs cursor-aware-present
   question.
4. **AxlCursor productionize** — implement test-first where possible
   (compositing logic over an in-RAM scene is unit-testable; the live
   present path gets an integration test via `run-qemu.sh --gpu` +
   `axl_gfx_capture` readback, like `test-gfx-present-qemu.sh`), ship the
   built-in arrow, a `cursor-demo` example, and fold the cursor into
   `pointer-demo`.

## 5. What stays in AGT (not this)

Selection model + interaction, caret blink/navigation/rendering, focus,
event routing, paste-into-document (uses `AxlClipboard` +
`axl_piece_tree_apply_edits`), undo grouping, layout, theming. The
substrate gives AGT: the cursor on screen, gesture-annotated events,
button repeat, the text model (`AxlPieceTree`: `offset_at` hit-test,
`cp_next/prev`, affected-range out-params), and the clipboard transport.

## 6. Open questions

- Cursor present integration: lift/drop bracket vs cursor-aware
  `axl_cursor_present` (spike decides).
- Click tuning + button-repeat tunables: global (simplest) vs per-source
  (flexible). Leaning global for v1.
- Built-in arrow: ship one fixed sprite, or a couple (arrow + I-beam +
  hand)? v1: arrow only; consumers set their own for the rest.
- Additive `AxlInputEvent` fields (`click_count`, `dragging`, `repeat`)
  vs a separate annotated event type. Leaning additive (zero cost to
  existing consumers).
