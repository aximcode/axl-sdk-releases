# AXL Console Terminal — a local interactive terminal on the framebuffer

**Status:** BUILT (component, 2026-07-12). The `AxlConsoleTerm` renderer —
cell model, scrollback, render, reflow, selection + copy, and pointer/hotkey
interaction — is implemented and unit-tested (`src/util/axl-console-term.c`,
`include/axl/axl-console-term.h`). The standalone `fbcon` tool on it lands next
(plan Task 9). Spec for the increment that adds a reusable terminal renderer to
axl-sdk and a standalone `fbcon` tool on it.

`AxlConsoleTerm` is the **local** sink for `AxlConsoleOps` — the counterpart to
`AxlConsoleMirror` (the **remote** sink that serializes ops to a VT byte stream). It
turns the take-over console's op stream into a rendered, interactive cell grid on the
GOP framebuffer: scrollback, drag-select + copy, and font zoom. It is what AGT's
`AgtTerminal` re-implements today; the goal is that `AgtTerminal` becomes a thin
themed layer over this, and a standalone `fbcon` tool needs no AGT code at all.

| Type | Source | Sink |
|------|--------|------|
| `AxlConsoleMirror` | `AxlConsoleOps` | VT bytes → **remote** terminal |
| **`AxlConsoleTerm`** | `AxlConsoleOps` | cell grid → **local** GOP (or an offscreen buffer) |

## 1. Motivation

`axl-console-device` already does the firmware-hard work: take over a running UEFI
Shell's console (output ops + input relay, `AXL-Console-Device-Design.md`). What's
missing to make a *usable* local terminal is ordinary UI code — an ops → cell grid →
pixels renderer plus the interaction (scrollback, selection, zoom). That renderer has
no reusable form in the library today; the only ops→pixels code is the
`console-device-smoke.c` test driver, which re-implements a crude grid. This spec
promotes that into a proper component and a real tool.

Non-goal: a VT *parser*. Input is the structured `AxlConsoleOps` from a producer;
`axl-vterm` already parses a raw VT byte stream into the same ops if a consumer ever
needs that path.

## 2. Architecture — three pieces, built in order

### Piece 1 — `axl-console-device` key-filter hook (small)

The read loop (`read_physical`) injects every physical key straight to the shell. To
let a consumer peek a few keys for itself (the terminal's Shift+PgUp scrollback,
Ctrl+Shift+C copy) BEFORE they reach the shell, the device config gains:

```c
/// Called for each physical key the read loop reads, BEFORE it is forwarded to the
/// shell. Return true to CONSUME the key (the shell never sees it). Only the
/// read_physical path is filtered; inject_* (consumer-fed keys) are never filtered.
bool (*key_filter)(void *user, const EFI_KEY_DATA *kd);
void  *key_filter_user;
```

`dev_read_timer_cb`, before `axl_console_input_push_notify`:
`if (d->key_filter && d->key_filter(d->key_filter_user, &kd)) continue;`. General
mechanism (any consumer can intercept keys), minimal surface. Unit-tested via the
existing device seam (consumed vs forwarded).

### Piece 2 — `AxlConsoleTerm` (`src/util/axl-console-term.{c,h}`, public `axl/axl-console-term.h`)

**Mechanism, not policy.** The component owns the grid + rendering + interaction
*mechanisms*; the consumer binds *gestures* to them. This is what lets `AgtTerminal`
keep custom keybindings and theming while reusing the plumbing.

**Data model.** A cell buffer = a scrollback history ring + the visible screen, both
built from the ops. Each cell holds a UTF-8 grapheme (BMP; one-per-codepoint, per the
device's `set_cell_rule`), fg/bg (indexed into the palette), and attributes. The
component also owns geometry (cols×rows), font + cell metrics, palette, cursor
(position + visibility + style), scroll offset (0 = live screen; >0 = scrolled back),
and the current selection range.

**Ops sink (output).** `axl_console_term_ops(t, &user)` returns the
`AxlConsoleOps` vtable to hand to `axl_console_device_install`. `output_text`,
`set_cursor`, `set_pen`, `clear_screen`, `set_term_prop` mutate the cell buffer + mark
dirty rows; scroll pushes the top line into the scrollback ring.

**Rendering.** `axl_console_term_render(t)` blits changed cells with `axl-gfx`
immediate-mode ops (`axl_gfx_fill_rect`, `axl_gfx_draw_text`) + `axl-font`. It keeps a
per-cell **damage shadow** of each cell's last-drawn appearance (glyph, effective
colours after selection-invert, cursor caret) and, within a dirty row, re-blits only
the cells whose appearance actually changed — so a one-character edit repaints one
cell, not the row. This bounds the framebuffer writes to the real change, which is
what keeps the CPU low on a **dirty-tracked display** (VNC/KVM write-protect the
framebuffer, so every pixel write faults into the hypervisor; re-blitting whole rows
per tick pegged a core). The caret is folded into its cell's render (erased on move,
not redrawn every frame). Called from a consumer loop timer (the component owns no
timer — same as the tap/mirror).
The **render target** is the caller's choice via the config's optional
`AxlGfxBuffer *target`: NULL = the GOP screen (fbcon); non-NULL = an offscreen buffer
(`axl_gfx_target_buffer`) the consumer composites (AGT). `render()` sets the target,
draws, restores — no new gfx primitives needed (the offscreen target + per-buffer
damage tracking already exist in `axl-gfx-surface.h`).

**Interaction (mechanisms).**
- `axl_console_term_scroll(t, delta_rows)` — move the scrollback view; clamps to
  [0, history]. Marks the whole viewport dirty.
- Selection: `selection_start(t, col, row)`, `selection_extend(t, col, row)`,
  `selection_clear(t)`, `selection_copy(t)` (extracts the selected text, trims
  trailing blanks per line, → `axl-clipboard`). Selection coordinates are in
  *viewport* space (the component maps them to buffer rows via the scroll offset).
- `axl_console_term_handle_pointer(t, evt)` — a convenience that applies the
  xterm-default bindings to an `AxlInput`-style pointer event: wheel → scroll, drag →
  select, Ctrl+wheel → a `on_zoom(delta)` callback the consumer supplies (the
  component does not choose fonts — the consumer picks and calls `set_font`). A
  consumer wanting custom bindings ignores this and drives the primitives directly.
- `axl_console_term_handle_hotkey(t, kd) -> bool` — the fn wired into the device's
  `key_filter`. Default hotkeys: Shift+PgUp/PgDn scroll, Ctrl+Shift+C copy; returns
  true (consumed) for those, false (forward to shell) otherwise. A consumer can wrap
  it or supply its own filter.

**Config / geometry.**
```c
typedef struct {
    uint32_t       cols, rows;      // 0 = auto from the target size / GOP and the font
    const AxlFont *font;            // NULL = axl_gfx_default_font()
    uint32_t       scrollback_rows; // history depth (0 = a sensible default)
    const AxlGfxPixel *palette;     // NULL = the default 16-colour console palette
    AxlGfxBuffer  *target;          // NULL = GOP screen; else an offscreen buffer
    uint32_t       x, y, w, h;      // render bounds within the target (0/0/0/0 = full)
    void (*on_zoom)(void *user, int32_t delta); // Ctrl+wheel from handle_pointer; NULL = ignore
    void  *cb_user;                 // passed to on_zoom (and future callbacks)
} AxlConsoleTermConfig;
```
- `axl_console_term_new(cfg)` / `_free(t)` — the component is a plain heap object
  (not a singleton; a consumer may host several, e.g. tabs), so it takes no global
  state and is independent of the device singleton.
- `set_font`, `resize(cols, rows)`, `set_bounds(x,y,w,h)`, `set_palette` — the
  reflow/zoom mechanisms. Changing the font or bounds recomputes cell metrics; the
  consumer decides the new cols×rows and calls `resize` + `axl_console_device_set_size`
  so the shell reflows.

### Piece 3 — `fbcon` (launcher app `tools/fbcon.c` + resident driver `tools/fbcon-drv.c`)

`fbcon.efi` ships as ONE runnable **application** (subsystem 10) that **embeds** the
resident take-over **driver** (`fbcon-drv.efi`, subsystem 11) via `AXL_EMBED_DECLARE`
and loads it from memory with `axl_driver_load_buffer_with_image_info` — the same
app-launches-driver pattern do.efi / mkrd use. So the user runs `fbcon.efi` as a
command instead of `load`-ing a driver. The launcher first **reaps** any resident
fbcon instance (locating the driver's presence marker — `tools/fbcon-marker.h` — and
`UnloadImage`-ing it; a driver can't unload itself, but the separate launcher image
can), then starts a fresh one. So `fbcon.efi` always leaves you in a clean take-over
with no accumulation. Leave with Ctrl+\ or by exiting the shell.

The **driver** (`fbcon-drv.c`) gives the running Shell a graphical terminal on the
framebuffer:
1. Resolve GOP + default font; compute cols×rows for the full screen.
2. `term = axl_console_term_new(cfg)` (GOP target, full bounds).
3. `axl_console_device_install(&dev, axl_console_term_ops(term, &u), u,
   &{take_input=true, read_physical=true, key_filter=hotkey_thunk,
     key_filter_user=term, cols, rows})`.
4. Attach a UEFI pointer (`axl-input` attach_mouse / SimplePointer) if present; pump
   its events into `axl_console_term_handle_pointer`. No pointer → output +
   keyboard only (graceful).
5. Loop timer → `axl_console_term_render`, and drop the take-over on Ctrl+\ or when
   the hosted shell exits (see below).
6. Install the presence marker on its image handle (so the launcher can reap it).
7. Unload → uninstall marker + `axl_console_device_uninstall` + `axl_console_term_free`.

The `key_filter` (`fbcon_hotkey`) peeks fbcon's own **Ctrl+\** (folds to FS `0x1C`)
to drop the take-over, else delegates to `axl_console_term_handle_hotkey(u, kd)` for
the terminal's scrollback/copy keys. **Leaving** happens in three ways, all funnelling
through one idempotent `fbcon_teardown()` (detach mouse → `axl_console_device_uninstall`
→ free term): (a) Ctrl+\ sets a flag the render timer acts on next tick; (b) the render
timer watches `axl_shell_kind()` and self-restores once the hosted UEFI shell exits
(so BDS / a re-launched "EFI Internal Shell" gets a working console instead of fbcon
squatting on it); (c) `unload`. The self-restore paths run the uninstall from the
render-timer callback, which is only safe because `axl_console_device_uninstall`
disconnects our (full-screen, non-80x25) device from the ConSplitter aggregates before
re-adding GraphicsConsole — otherwise the mode reconstruction asserts
(`ConSplitter.c:2983`). On load fbcon prints a one-line "press Ctrl+\ to leave" notice.
Regression: `test-console-device-qemu.sh` Scenarios 7 (fbcon-leave), 8 (fbcon-exit),
9 (fbcon-reload — re-running `fbcon.efi` reaps the lingering instance + re-takes-over).

## 3. Testing

- **Component (unit, both arches, RED→GREEN).** Headless seam (like the tap/device
  seams): feed ops, assert the cell-buffer + cursor + scrollback + selection state and
  the rendered output *against the in-memory grid* (no GOP needed — render into a
  small `AxlGfxBuffer` and read pixels, or assert the grid model directly). Cover:
  `output_text`/wrap/scroll into history, `set_pen`→cell colours, `clear_screen`,
  `scroll` clamping, `resize`/`set_font` reflow, selection extract + copy (assert the
  clipboard bytes), `handle_hotkey` consume/forward.
- **Device key_filter (unit).** Via the existing device seam: a filter that consumes
  Shift+PgUp drops it from the ring; a plain key forwards. Confirms the read-loop hook.
- **Firmware smoke (DEBUG-OVMF).** A `fbcon` scenario: take over, `--sendkey ver`
  renders through the terminal (keys reach the shell — same discriminator as the
  device input-relay scenario) with 0 fatals; a `--sendkey` Shift+PgUp exercises the
  scrollback hotkey. Pointer-driven selection/zoom is impractical to drive in QEMU, so
  it stays unit-tested + code-reviewed — **disclosed** as a firmware-coverage gap
  (the honest line, per the ConIn-teardown precedent).

## 4. What this reuses (nothing new below the terminal)

- `axl-console-device` (take-over + input relay + the new key_filter hook).
- `AxlConsoleOps` (the op contract) — already the two-producer / two-sink vtable.
- `axl-gfx` immediate-mode draw + `axl-gfx-surface` offscreen `AxlGfxBuffer` target +
  damage tracking (the pixmap render target **already exists**).
- `axl-font` (monospace cell metrics, `axl_gfx_draw_text`, the default font).
- `axl-clipboard` (copy), `axl-input` (pointer attach), `axl-loop` (the render timer).

## 5. Place in the console family

`AXL-Console-Design.md` is the umbrella. This adds the local-render leaf, sibling to
`axl-console-mirror` (remote). `AgtTerminal` is expected to re-parent onto
`AxlConsoleTerm` (keeping its theming, custom keybindings, tabs, and Ctrl+wheel
zoom as consumer policy on top of these mechanisms); that migration is AGT-side and
out of scope here.
