Toolkit-agnostic input substrate: mouse, keyboard, and touch as raw
event sources on an `AxlLoop`. Unified `AxlInputEvent` callback type
for consumers that want a single dispatch path; per-device wrappers
that bridge UEFI input protocols (`EFI_SIMPLE_POINTER_PROTOCOL`,
`EFI_SIMPLE_TEXT_INPUT_PROTOCOL`, `EFI_ABSOLUTE_POINTER_PROTOCOL`)
into the loop's existing source pattern.

Header: `<axl/axl-input.h>`

## Role

This module is the **input substrate** for higher-level toolkits.
Per [docs/AGT-Design.md](../../docs/AGT-Design.md) §"Substrate
discipline rules", `axl-input` is pure C and paradigm-agnostic — it
produces raw events, not widget messages. Toolkits translate
`AxlInputEvent` into their own dispatch model (AGT message maps,
GTK-style signals, immediate-mode polling, etc.) at the layer above.

Sibling of `axl-gfx` in axl-sdk core. Neither depends on the other,
and neither depends on a toolkit.

## Loop integration

axl-input does **not** introduce its own queue, poll, or wait API.
Each input source registers with `AxlLoop` using the same primitives
the loop already exposes for any other UEFI event source:

- `axl_input_attach_mouse` / `axl_input_attach_touch` wrap
  `axl_loop_add_event` around the protocol's `WaitForInput` event.
  The dispatch trampoline calls `GetState`, translates the result
  into one or more `AxlInputEvent` values, and invokes the callback.
- `axl_input_attach_key` wraps `axl_loop_add_key_press` (which
  internally registers `EFI_SIMPLE_TEXT_INPUT_PROTOCOL`'s
  `WaitForKey`) and translates each `AxlInputKey` into an
  `AxlInputEvent` with `type = AXL_INPUT_KEY_DOWN`.

The benefit is uniformity: any code path that already understands
`axl_loop_run`, `axl_loop_remove_source`, timeouts, and idle callbacks
"just works" with input — no parallel event-pump for the consumer to
remember to drive.

## Unified event

```c
typedef struct {
    AxlInputType  type;            // discriminator (mouse / key / touch / ...)
    uint64_t      timestamp_us;    // wall-clock microseconds since boot
    int32_t       x, y;            // cursor or touch position
    uint32_t      buttons;         // AXL_INPUT_BUTTON_* bitmask
    int32_t       wheel_dx;        // horizontal wheel delta (ticks)
    int32_t       wheel_dy;        // vertical wheel delta
    uint32_t      keycode;         // raw scan code (key events)
    uint32_t      unicode;         // translated codepoint (0 if none)
    uint32_t      modifiers;       // AXL_INPUT_MOD_* bitmask
    uint32_t      click_count;     // 1/2/3 = single/double/triple click (mouse buttons)
    bool          dragging;        // held-button press has crossed the drag threshold
    bool          repeat;          // synthetic held-button auto-repeat, not a fresh press
} AxlInputEvent;
```

Fields are populated based on `.type`; unused fields are zero. The
pointer passed to the callback is valid only for the duration of the
call — copy out anything you want to keep. The last three fields are
filled by the built-in recognizers (see *Recognizers* below).

Event kinds shipped in v0.1:

| `AxlInputType` | Emitted by | When |
|---|---|---|
| `AXL_INPUT_MOUSE_MOVE` | mouse | non-zero relative motion |
| `AXL_INPUT_MOUSE_BUTTON_DOWN`/`UP` | mouse | button-state transition |
| `AXL_INPUT_MOUSE_WHEEL` | mouse | non-zero Z motion |
| `AXL_INPUT_KEY_DOWN` | keyboard | each key press |
| `AXL_INPUT_KEY_UP` | (deferred) | requires `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL` |
| `AXL_INPUT_TOUCH_DOWN` | touch | first contact (active-buttons 0 → non-zero) |
| `AXL_INPUT_TOUCH_UP` | touch | contact end (non-zero → 0) |
| `AXL_INPUT_TOUCH_MOVE` | touch | position changed — including hover (no contact) |

`KEY_DOWN` events carry modifier + lock state in `modifiers`:
held `SHIFT` / `CTRL` / `ALT` / `META` (left/right-distinct bits plus
side-agnostic masks — `AXL_INPUT_MOD_SHIFT == LSHIFT | RSHIFT`) and
`CAPS_LOCK` / `NUM_LOCK` / `SCROLL_LOCK`. These come from
`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`; when the firmware doesn't publish
it (e.g. a serial console), `modifiers == 0` — treat absent modifiers
as "none". Only `KEY_DOWN` fires: UEFI delivers no key-up or
standalone-modifier events. (The live keyboard read is exercised on
real hardware, not in the QEMU serial test harness.)

**Ctrl+\<letter\> chords are device-dependent.** A physical keyboard
reports them as the printable letter plus `AXL_INPUT_MOD_CTRL` (Ctrl+A →
`'a'` + the Ctrl bit); a serial console (TerminalDxe) sends the folded
C0 control byte (Ctrl+A → `0x01`) with `modifiers == 0`. Match both with
`axl_input_ctrl_letter(unicode, modifiers)`, which collapses the two
encodings into a single lowercase letter (and returns 0 for the four
chords whose C0 codes are dedicated editing keys — Ctrl+H/I/J/M).

## Usage

A single callback can demultiplex across all three sources:

```c
#include <axl.h>

static bool
on_input(const AxlInputEvent *ev, void *data)
{
    AxlLoop *loop = (AxlLoop *)data;

    switch (ev->type) {
    case AXL_INPUT_MOUSE_MOVE:
        ui_cursor_to(ev->x, ev->y);
        break;
    case AXL_INPUT_MOUSE_BUTTON_DOWN:
        ui_click(ev->buttons);
        break;
    case AXL_INPUT_KEY_DOWN:
        if (ev->unicode == 'q') {
            axl_loop_quit(loop);
            return AXL_SOURCE_REMOVE;
        }
        break;
    case AXL_INPUT_TOUCH_DOWN:
        ui_touch_begin(ev->x, ev->y);
        break;
    default:
        break;
    }
    return AXL_SOURCE_CONTINUE;
}

int main(void) {
    AxlLoop *loop = axl_loop_new();
    axl_input_attach_mouse(loop, on_input, loop);
    axl_input_attach_key  (loop, on_input, loop);
    axl_input_attach_touch(loop, on_input, loop);
    axl_loop_run(loop);
    axl_loop_unref(loop);
    return 0;
}
```

Each `attach_*` returns the loop source ID (use with
`axl_loop_remove_source` to detach), or 0 on failure: the protocol
isn't available, a source of that kind is already attached, or
arguments were NULL. Detach the mouse with `axl_input_detach_mouse`
(frees the single-mouse slot and cancels any auto-repeat timer) and the
touch / absolute pointer with `axl_input_detach_touch` (touch binds several
loop sources, so it needs its own teardown rather than one
`axl_loop_remove_source`).

## Recognizers: gestures, debounce, auto-repeat

`axl_input_attach_mouse` and `axl_input_attach_key` run small built-in
recognizers that annotate events and, where useful, synthesize new
ones. All are pure functions over the event stream, so a consumer with
its own stream (or a unit test) can run the same logic directly.

- **Click gestures** annotate each mouse-button event in place:
  `click_count` is 1 / 2 / 3 for single / double / triple click within
  the multi-click window and movement threshold, and `dragging` latches
  once a held-button press moves past the drag threshold (until release).
  Tune with `axl_input_set_click_tuning(multi_click_ms, drag_px)`; run
  the recognizer standalone with `axl_input_gesture_feed`.

- **Held-button auto-repeat** synthesizes a `MOUSE_BUTTON_DOWN` (with
  `repeat == true`) after a delay, then at an interval, while a button
  stays held — for scrollbar arrows, spinners, press-and-hold. Off by
  default; enable with `axl_input_set_button_repeat(delay_ms,
  interval_ms)`. (Firmware does not auto-repeat *mouse* buttons, unlike
  keys.)

- **Keyboard debounce** drops a same-key `KEY_DOWN` that repeats faster
  than a human would — the fix for a high-latency remote console (a BMC
  virtual console, IPMI SOL) where one keypress registers as held long
  enough that firmware typematic fires it several times. Off by default;
  enable with `axl_input_set_key_debounce(min_repeat_ms,
  printable_only)`, or run it standalone with `axl_input_key_accept`.
  Held-key *repeat itself* comes from firmware typematic (UEFI delivers
  no key-up, so software can't synthesize it) — the debounce only
  suppresses the spurious extras.

## Modifier state on pointer events

UEFI delivers keyboard modifier state (`KeyShiftState`) **only with a
keystroke**, so "Shift held while scrolling" or "Ctrl held while
clicking" would otherwise be invisible to a mouse callback. The substrate
keeps a **live modifier state** and stamps it onto every pointer event
(`ev->modifiers` on mouse + touch), so a consumer reads Shift+wheel /
Ctrl+click directly. To stay current *between* character keystrokes the
keyboard backend enables `EFI_KEY_STATE_EXPOSED`, which makes the
firmware deliver modifier-only **partial keystrokes** on shift/ctrl/alt
down+up; the substrate tracks those to update the live state and filters
them out of the `KEY_DOWN` stream (a partial carries no character).

Caveats: the live state is 0 until a keyboard source is attached
(`axl_input_attach_key`) and the firmware publishes
`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL` — a serial console (no Ex protocol)
reports no modifiers, so pointer events carry `modifiers == 0` there.
Enabling `EFI_KEY_STATE_EXPOSED` also resets the caps/num/scroll-lock
toggle LEDs (UEFI offers no read-modify-write for them). The partial-
keystroke path is firmware-gated and validated on real hardware (the
serial QEMU test harness has no Ex protocol to exercise it).

## Per-device notes

**Mouse** (`axl_input_attach_mouse`). Locates
`EFI_SIMPLE_POINTER_PROTOCOL` via `LocateProtocol`. Cursor positions
are accumulated relative deltas starting at `(0, 0)` — most firmware
reports motion as deltas, not absolute screen coordinates. Callers
wanting screen-bounded positions should clamp in the callback. Each
dispatch may produce multiple discrete events (motion + button +
wheel) from a single `GetState` call.

**Keyboard** (`axl_input_attach_key`). Thin translator over
`axl_loop_add_key_press` — the existing loop primitive does the
protocol handling. The wrapper exists so a consumer that wants one
callback for mouse + keyboard + touch doesn't have to maintain a
separate `AxlKeyCallback`. Callers who already have a key-only flow
can keep using `axl_loop_add_key_press` directly.

**Touch / absolute pointer** (`axl_input_attach_touch`). Drives the seat
from `EFI_ABSOLUTE_POINTER_PROTOCOL` — a touchscreen, pen / digitizer, VNC
absolute pointer, or the virtual mouse a BMC remote console presents. On
modern firmware (UEFI ≥ 2.30) the BIOS multiplexes pointer devices through
`gST->ConsoleInHandle`, and that is where a remote-console pointer's events
actually arrive — so `attach_touch` binds **every** absolute-pointer handle,
**ConsoleInHandle first**, not a single located one. Each handle gets a
`WaitForInput` event source (the efficient path — idle means no wakeups),
plus a low-rate poll fallback for firmware whose `WaitForInput` never
signals; a dispatch reads the first handle with data and stops (`GetState`
consumes, so the sources never double-count). Tear it **all** down with
`axl_input_detach_touch` — not a single `axl_loop_remove_source`, since it
registers several sources. (The compositor wraps this as
`axl_compositor_attach_touch`, scaling the normalized position onto the
output and running the click/drag recognizer so an absolute pointer gets
double-click + drag.)

Positions are **normalized** from the device's native
`EFI_ABSOLUTE_POINTER_MODE` range into `[0, AXL_INPUT_ABS_RANGE)` on both
axes, so the value is display-independent — the consumer maps it onto its
own surface (`px = ev->x * surface_w / AXL_INPUT_ABS_RANGE`);
`axl_input_abs_normalize` exposes the exact mapping. A `TOUCH_MOVE` fires
whenever the position changes, **including hover** (no contact), so a pen /
tablet / VNC absolute pointer that reports position without a button still
drives a pointer; `buttons` on a move is the live contact state (0 on
hover), so a drag is distinguishable from a hover.

Tune the read path with `axl_input_set_touch_config` when a given firmware
misbehaves: pick the mechanism (`AXL_INPUT_TOUCH_EVENT_AND_POLL` default /
`EVENT_ONLY` / `POLL_ONLY`), the poll interval (some BMC consoles flicker or
stall the pointer protocol if polled too fast), and whether to bind only
`ConsoleInHandle` (skipping a separate physical handle that would otherwise
deliver the same device twice). To see what a platform actually exposes,
`axl_input_probe_pointers` enumerates every simple / absolute pointer handle,
flags the `ConsoleInHandle` aggregator, and runs a live event-vs-poll
comparison — handy when a remote-console pointer "doesn't move."

## Virtual pointer (install + drive a synthetic pointer)

Where `axl_input_attach_*` **consume** a pointer, `axl_virtual_pointer_*`
**publish** one — the pointer twin of `axl_console_mirror_inject_key`. A
remote / synthetic source (a VNC server's RFB `PointerEvent`, an automated UI
test) drives the firmware Setup browser / HII FrontPage on a box with no
physical mouse (a headless server, QEMU `-vga none`):

```c
AxlVirtualPointer *vp = NULL;
axl_virtual_pointer_install(&vp, NULL);          // range = active GOP resolution
// ... on each RFB PointerEvent (absolute pixel coords + button mask):
axl_virtual_pointer_inject(vp, px, py, buttons); // bit0=left, bit1=right
// ...
axl_virtual_pointer_uninstall(vp);
```

`install` publishes an `EFI_ABSOLUTE_POINTER_PROTOCOL` (absolute coords map
1:1 from framebuffer pixels; range defaults to the active GOP resolution, or
set `cfg.width/height`), and with `cfg.also_simple` a relative
`EFI_SIMPLE_POINTER_PROTOCOL` too. `inject` updates the protocol's
`CurrentState` and signals `WaitForInput` so a consumer blocked in
`WaitForEvent` wakes and reads it via `GetState`.
`axl_virtual_pointer_scroll(vp, dy)` injects a wheel notch (on the
`also_simple` SimplePointer's `RelativeMovementZ`, which `attach_mouse`
decodes to `AXL_INPUT_MOUSE_WHEEL`); EFI exposes only one wheel axis.

It also doubles as a **deterministic input-test driver**: because it's
firmware-internal, it delivers pointer events where QMP `input-send-event`
can't (headless CI runners). The input unit tests use it to drive the real
consumption path — install a virtual pointer, `attach_touch` to it, inject a
press/move/release, pump the loop, and assert the `TOUCH_DOWN/MOVE/UP` (and
relative-move / button / wheel via the SimplePointer) arrive at the callback.

The subtle part it solves once: the Setup browser reads the pointer the
console aggregator (ConSplitter) publishes on `gST->ConsoleInHandle` (via
`LocateProtocol` / `HandleProtocol`), not a blind handle. So `install`
**replaces** the AbsolutePointer on `ConsoleInHandle`
(`ReinstallProtocolInterface`, saving the original to restore on uninstall) —
the same technique `AxlConsoleMirror` uses for `SimpleTextInputEx` so `edit`
sees injected keys. Pair `axl_virtual_pointer_inject` (pointer) with
`axl_console_mirror_inject_key` (keyboard) for the full remote "seat" under one
WS endpoint. Singleton (one console pointer); the end-to-end "the browser
visually responds" is real-hardware, while the install + route + inject +
`WaitForInput` round-trip is what the unit test pins under QEMU.

### v0.1 constraints

- Single source per device kind per process. Multi-device or hotplug
  support can be added when a consumer requires it.
- Mouse positions are relative deltas accumulated from `(0, 0)` (not
  screen-clamped — the substrate doesn't know the consumer's coordinate
  system); touch positions are normalized to `[0, AXL_INPUT_ABS_RANGE)`.

## Visual demo

[`sdk/examples/input-demo.c`](../../sdk/examples/input-demo.c)
attaches all three sources, runs the loop for 5 seconds, and renders
a live status panel via `axl-gfx`. Run with
`scripts/run-qemu.sh out/x64/input-demo.efi`.
