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
} AxlInputEvent;
```

Fields are populated based on `.type`; unused fields are zero. The
pointer passed to the callback is valid only for the duration of the
call — copy out anything you want to keep.

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
| `AXL_INPUT_TOUCH_MOVE` | touch | active contact, position changed |

Modifier bits (`AXL_INPUT_MOD_SHIFT` / `CTRL` / `ALT` / `META`) and
`AXL_INPUT_KEY_UP` events require `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`
— deferred until a consumer asks. For v0.1, `modifiers == 0` and only
`KEY_DOWN` fires.

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
arguments were NULL.

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

**Touch** (`axl_input_attach_touch`). Locates
`EFI_ABSOLUTE_POINTER_PROTOCOL`. Positions are reported in the
device's native `(CurrentX, CurrentY)` range — see
`EFI_ABSOLUTE_POINTER_MODE`'s `AbsoluteMin`/`Max` for the coordinate
system. Callers wanting screen pixels should rescale in the callback.

### v0.1 constraints

- Single source per device kind per process. Multi-device or hotplug
  support can be added when a consumer requires it.
- Mouse / touch positions are not screen-clamped — the substrate
  doesn't know what the consumer's coordinate system is.

## Visual demo

[`sdk/examples/input-demo.c`](../../sdk/examples/input-demo.c)
attaches all three sources, runs the loop for 5 seconds, and renders
a live status panel via `axl-gfx`. Run with
`scripts/run-qemu.sh out/x64/input-demo.efi`.
