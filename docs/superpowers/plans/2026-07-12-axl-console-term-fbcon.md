# AxlConsoleTerm + fbcon Implementation Plan

> **PROGRESS (2026-07-12, inline execution, GATED/UNPUSHED):**
> - ✅ **Task 1** — device `key_filter` hook (`f1115ba0`)
> - ✅ **Task 2** — `AxlConsoleTerm` cell model + output ops (`aeb64ed9`)
> - ✅ **Task 3** — scrollback ring + `scroll()` (`f12170dc`)
> - ⏭ **RESUME AT Task 4** (render to GOP / offscreen `AxlGfxBuffer`).
>
> State: 7519/0 unit both arches; `include/axl/axl-console-term.h` +
> `src/util/axl-console-term.c` exist with the cell model + scrollback + seams;
> `axl.h`/`Makefile`/`console-mirror.rst` wired. Tasks 4-10 unstarted. Note the
> "confirm the name" flags in each remaining task's Step 1 — read the header
> (`axl-gfx-surface.h` buffer ctor/accessor for Task 4; `axl-clipboard.h` for Task 6;
> `axl-input.h` pointer type + `attach_mouse` for Tasks 7/9) BEFORE writing the test.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable local terminal renderer (`AxlConsoleTerm`) that turns the
take-over console's `AxlConsoleOps` into an interactive cell grid on the GOP
framebuffer, plus a standalone `fbcon` tool on it.

**Architecture:** `AxlConsoleTerm` is the local sink for `AxlConsoleOps` (counterpart
to `AxlConsoleMirror`, the remote VT sink). It owns a cell buffer + scrollback +
selection + rendering *mechanisms*; the consumer binds gestures. Keys reach the shell
through `axl-console-device`'s existing input relay; a new `cfg.key_filter` hook lets
the terminal peek hotkeys first. `fbcon` is a resident driver wiring device + term.

**Tech Stack:** C (gnu2x, freestanding UEFI), `axl-console-device`, `axl-console-ops`,
`axl-gfx` / `axl-gfx-surface` (offscreen `AxlGfxBuffer` target already exists),
`axl-font`, `axl-clipboard`, `axl-input`, `axl-loop`. Full design:
`docs/AXL-Console-Terminal-Design.md`.

## Global Constraints

- **Naming:** `axl_snake_case` functions, `AxlPascalCase` types, `AXL_SCREAMING_CASE`
  macros. Public API uses standard C types only (no EDK2/UEFI types in
  `axl/axl-console-term.h`; `EFI_KEY_DATA` is allowed only where it already crosses
  the boundary, i.e. the device `key_filter` typedef which lives in the device header
  that already exposes UEFI types via its inject family? NO — check: the device header
  is standard-C. So `key_filter` takes an opaque key. See Task 1.)
- **Both arches:** every change builds + tests on X64 and AARCH64 (`make ARCH=aa64`).
- **TDD:** write the failing test first, confirm RED, implement, confirm GREEN. Unit
  tests in `test/unit/axl-test-util.c` (the console home) via headless seams; run one
  binary with `TEST_APPS_ONLY=AxlTestUtil ./test/integration/test-axl.sh --arch X64`.
- **Exact-string / exact-value assertions** for output; `test_check(cond, "msg")`.
- **Ratchet:** unit count only grows; leave `test/integration/.last-pass-count`
  uncommitted. Don't `git add -A`; commit only your own paths. Repo is GATED — do not
  push.
- **Coding style:** `docs/AXL-Coding-Style.md`; `///<` inline param docs, `@brief` /
  `@return` block comments; multi-line signatures; `make check-ascii` + `make
  check-docs` before commit.
- **Doc sync:** new public header needs a `doxygenfile` directive + a README note
  (see Task 9).

## File Structure

- **Create** `include/axl/axl-console-term.h` — public API (opaque `AxlConsoleTerm`,
  `AxlConsoleTermConfig`, all `axl_console_term_*`).
- **Create** `src/util/axl-console-term.c` — implementation + `_test_*` seams.
- **Modify** `include/axl/axl-console-device.h` — add `key_filter` / `key_filter_user`
  to `AxlConsoleDeviceConfig`.
- **Modify** `src/util/axl-console-device.c` — call the filter in `dev_read_timer_cb`;
  copy the two cfg fields in install.
- **Modify** `Makefile` — `src/util/axl-console-term.c` in `LIB_SOURCES`; `fbcon` +
  `fbcon-smoke` targets; `.PHONY`.
- **Modify** `include/axl.h` — include `axl/axl-console-term.h`.
- **Modify** `test/unit/axl-test-util.c` — device key_filter test; term seam + tests.
- **Create** `tools/fbcon.c` — the resident-driver tool.
- **Create** `test/integration/fbcon-smoke.c` — firmware smoke driver (or reuse the
  console-device smoke pattern), **Modify** `test-console-device-qemu.sh` +
  `analyze-console-device-shot.py`.
- **Modify** `docs/sphinx/modules/console-mirror.rst` (doxygenfile) + `src/util`/a
  README + `docs/AXL-Console-Terminal-Design.md` status.

Each `axl_console_term_*` layer (model, scrollback, render, reflow, selection,
interaction) is one task ending in a green unit test + commit.

---

### Task 1: Device `key_filter` hook

**Files:**
- Modify: `include/axl/axl-console-device.h` (the `AxlConsoleDeviceConfig` struct)
- Modify: `src/util/axl-console-device.c` (`dev_read_timer_cb`, install cfg copy, struct)
- Test: `test/unit/axl-test-util.c` (extend `test_console_device_input` or add
  `test_console_device_key_filter`)

**Interfaces:**
- Produces: `AxlConsoleDeviceConfig.key_filter` = `bool (*)(void *user, const void *key)`
  and `.key_filter_user` (void*). The key is passed as `const void *` (opaque) so the
  public header stays standard-C; the terminal casts it to `EFI_KEY_DATA*` internally
  (it already includes uefi headers). The read loop calls
  `key_filter(user, &kd)` before `axl_console_input_push_notify`; a true return drops
  the key. `AxlConsoleDevice` gains `key_filter`/`key_filter_user` fields set in install.

> Note on the opaque key: the terminal's `handle_hotkey` (Task 8) takes the SAME
> `const void *` and interprets the bytes as an `EFI_KEY_DATA`. This keeps the public
> device + term headers free of UEFI types while letting the two cooperate. Document
> the pointee shape (scan/unicode/shift) in both headers.

- [ ] **Step 1: Write the failing test** in `test/unit/axl-test-util.c`, appended
  inside `test_console_device_input` (after the existing sub-tests, before
  `_axl_console_device_test_end(d)`):

```c
    /* key_filter: the read-loop peek. The seam can't run the timer, so drive the
       filter directly through a tiny helper the seam exposes. */
    static int s_filtered;    /* file-scope static near the other test statics */
    /* (add near notify_calls:) static int s_filtered; static bool
       filter_shift_pgup(void *user, const void *key) {
           const EFI_KEY_DATA *kd = key; (void)user;
           if (kd->Key.ScanCode == 0x09 &&
               (kd->KeyState.KeyShiftState &
                (EFI_LEFT_SHIFT_PRESSED|EFI_RIGHT_SHIFT_PRESSED))) { s_filtered++; return true; }
           return false;
       } */
    s_filtered = 0;
    test_check(_axl_console_device_test_run_filter(d, filter_shift_pgup, NULL,
                   /*scan=*/0x09, /*uni=*/0, /*shift=*/EFI_LEFT_SHIFT_PRESSED|EFI_SHIFT_STATE_VALID)
                   == true && s_filtered == 1,
               "device key_filter: Shift+PgUp is consumed (true), filter ran once");
    test_check(_axl_console_device_test_run_filter(d, filter_shift_pgup, NULL,
                   /*scan=*/0, /*uni=*/'x', /*shift=*/0) == false,
               "device key_filter: a plain key is forwarded (false)");
```

  And add the seam declaration near the other `_axl_console_device_test_*` externs:

```c
extern bool _axl_console_device_test_run_filter(AxlConsoleDevice *d,
        bool (*fn)(void *, const void *), void *user,
        uint16_t scan, uint16_t uni, uint32_t shift);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make ARCH=x64 tests 2>&1 | grep -iE 'error|_test_run_filter'`
Expected: FAIL — link error / undefined `_axl_console_device_test_run_filter`.

- [ ] **Step 3: Add the cfg fields** to `include/axl/axl-console-device.h`
  `AxlConsoleDeviceConfig` (after `min_gap_ms`, before `auto_alt_screen`):

```c
    /// (take_input only) Peek each physical key the read loop reads BEFORE it is
    /// forwarded to the shell. Return true to CONSUME it (the shell never sees it) --
    /// e.g. a terminal claiming Shift+PgUp for scrollback. NULL = forward everything.
    /// @a key points at the firmware key record (scan code / unicode / shift state);
    /// a consumer that needs the fields casts it to its UEFI key type.
    bool (*key_filter)(void *user, const void *key);
    void  *key_filter_user;   ///< opaque context passed to @a key_filter
```

- [ ] **Step 4: Thread the fields** in `src/util/axl-console-device.c`: add
  `bool (*key_filter)(void *, const void *); void *key_filter_user;` to
  `struct AxlConsoleDevice` (near `read_physical`); in `axl_console_device_install`
  after `d->min_gap_ms = cfg->min_gap_ms;` add
  `d->key_filter = cfg->key_filter; d->key_filter_user = cfg->key_filter_user;`.

- [ ] **Step 5: Call the filter** in `dev_read_timer_cb`, inside the
  `while (!EFI_ERROR(ex->ReadKeyStrokeEx(ex, &kd)))` loop, replace the body:

```c
        while (!EFI_ERROR(ex->ReadKeyStrokeEx(ex, &kd))) {
            if (d->key_filter != NULL && d->key_filter(d->key_filter_user, &kd)) {
                continue;   /* consumed by the consumer (e.g. a terminal hotkey) */
            }
            if (input_gate_admits(d, &kd)) {
                axl_console_input_push_notify(&d->in, kd);
            }
        }
```

- [ ] **Step 6: Add the seam** at the end of `src/util/axl-console-device.c` (near the
  other `_axl_console_device_test_*`):

```c
/* Drive the key_filter directly (the headless seam can't run the read timer): build a
   key, call the filter, return what it returned. */
bool
_axl_console_device_test_run_filter(AxlConsoleDevice *d,
        bool (*fn)(void *, const void *), void *user,
        uint16_t scan, uint16_t uni, uint32_t shift)
{
    (void)d;
    EFI_KEY_DATA kd = {0};
    kd.Key.ScanCode           = scan;
    kd.Key.UnicodeChar        = uni;
    kd.KeyState.KeyShiftState = shift;
    return fn != NULL && fn(user, &kd);
}
```

- [ ] **Step 7: Run test to verify it passes**

Run: `make ARCH=x64 2>&1 | grep -iE 'error|warning'; TEST_APPS_ONLY=AxlTestUtil timeout 90 ./test/integration/test-axl.sh --arch X64 2>&1 | grep -iE 'key_filter|Results'`
Expected: both `key_filter` lines PASS; no warnings.

- [ ] **Step 8: Both arches build**

Run: `make ARCH=aa64 2>&1 | grep -iE 'error|warning' || echo clean`
Expected: clean.

- [ ] **Step 9: Commit**

```bash
git add include/axl/axl-console-device.h src/util/axl-console-device.c test/unit/axl-test-util.c
git commit -m "console: add axl-console-device key_filter hook (read-loop key peek)"
```

---

### Task 2: `AxlConsoleTerm` skeleton + cell model + output ops

**Files:**
- Create: `include/axl/axl-console-term.h`
- Create: `src/util/axl-console-term.c`
- Modify: `Makefile` (`LIB_SOURCES`), `include/axl.h`
- Test: `test/unit/axl-test-util.c` (new `test_console_term_output` + seam externs)

**Interfaces:**
- Produces (public, `axl/axl-console-term.h`):
  ```c
  typedef struct AxlConsoleTerm AxlConsoleTerm;
  typedef struct { uint32_t cols, rows; const AxlFont *font; uint32_t scrollback_rows;
      const AxlGfxPixel *palette; AxlGfxBuffer *target; uint32_t x, y, w, h;
      void (*on_zoom)(void *user, int32_t delta); void *cb_user; } AxlConsoleTermConfig;
  AxlConsoleTerm *axl_console_term_new(const AxlConsoleTermConfig *cfg);
  void axl_console_term_free(AxlConsoleTerm *t);
  const AxlConsoleOps *axl_console_term_ops(AxlConsoleTerm *t, void **user);
  ```
- Produces (seams, no header, used by the unit test):
  ```c
  // in axl-console-term.c
  bool _axl_console_term_test_cell(AxlConsoleTerm *t, uint32_t row, uint32_t col,
                                   char *utf8_out /*>=5*/, uint8_t *fg, uint8_t *bg);
  void _axl_console_term_test_cursor(AxlConsoleTerm *t, uint32_t *row, uint32_t *col);
  ```
- Consumes: `AxlConsoleOps` (from `axl/axl-console-ops.h`), `AxlFont`/`AxlGfxPixel`.

Internal model (document in the .c): `Cell { char utf8[4]; uint8_t len, fg, bg; }`;
`AxlConsoleTerm { cols, rows; Cell *screen /*rows*cols*/; int32_t cur_row, cur_col;
uint8_t pen_fg, pen_bg; bool cursor_visible; const AxlFont *font; AxlGfxPixel
palette[16]; ... AxlConsoleOps ops; }`. Ops mirror the smoke driver's GRID_OPS
(`test/integration/console-device-smoke.c` lines ~208-271) — reuse that proven logic:
`output_text` decodes UTF-8 → `grid_put_cp`; `set_cursor` clamps; `set_pen` maps
INDEXED nibble → pen_fg (&0x0F) / pen_bg (&0x07); `clear_screen` blanks + homes;
`set_term_prop(CURSOR_VISIBLE)` sets the flag.

- [ ] **Step 1: Write the failing test** — add to `test/unit/axl-test-util.c` (after
  `test_console_device_input`), and register it in the runner next to
  `test_console_device_input();`:

```c
static void
test_console_term_output(void)
{
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };   /* font NULL -> default */
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    test_check(t != NULL, "term: new(20x5) succeeds");

    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    test_check(ops != NULL && ops->output_text != NULL, "term: ops vtable exposed");

    /* set_pen(fg=2 green, bg=1 blue) then output 'Hi'. */
    AxlConsolePen pen = { .fg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = 2 },
                          .bg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = 1 } };
    ops->set_pen(u, &pen);
    ops->output_text(u, "Hi", 2);

    char c[5]; uint8_t fg, bg;
    test_check(_axl_console_term_test_cell(t, 0, 0, c, &fg, &bg)
                   && axl_strcmp(c, "H") == 0 && fg == 2 && bg == 1,
               "term: output_text lands 'H' green-on-blue at (0,0)");
    test_check(_axl_console_term_test_cell(t, 0, 1, c, &fg, &bg)
                   && axl_strcmp(c, "i") == 0,
               "term: output_text lands 'i' at (0,1)");

    uint32_t cr, cc;
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 0 && cc == 2, "term: cursor advanced to (0,2)");

    /* newline moves down + carriage-return columns to 0. */
    ops->output_text(u, "\r\nX", 3);
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 1 && cc == 1, "term: CR/LF moved to row 1, 'X' at col 0");
    test_check(_axl_console_term_test_cell(t, 1, 0, c, &fg, &bg) && axl_strcmp(c, "X") == 0,
               "term: 'X' at (1,0) after CR/LF");

    /* clear_screen blanks + homes. */
    ops->clear_screen(u);
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 0 && cc == 0 && _axl_console_term_test_cell(t, 0, 0, c, &fg, &bg)
                   && c[0] == '\0',
               "term: clear_screen blanks (0,0) + homes the cursor");

    axl_console_term_free(t);
}
```

  Seam externs near the other console seams:
```c
extern bool _axl_console_term_test_cell(AxlConsoleTerm *t, uint32_t row, uint32_t col,
                                        char *utf8_out, uint8_t *fg, uint8_t *bg);
extern void _axl_console_term_test_cursor(AxlConsoleTerm *t, uint32_t *row, uint32_t *col);
```

- [ ] **Step 2: Run test to verify it fails** — `make ARCH=x64 tests` → undefined
  `axl_console_term_new`. Expected FAIL.

- [ ] **Step 3: Write the header** `include/axl/axl-console-term.h` — the Interfaces
  block above, with `@file`/`@brief`/`///<` docs matching the style of
  `axl-console-device.h`. Include `<stdint.h> <stddef.h> <stdbool.h>
  axl/axl-console-ops.h axl/axl-font.h axl/axl-gfx.h`. `extern "C"` guard.

- [ ] **Step 4: Write `src/util/axl-console-term.c`** — the `Cell`/struct model, the
  16-colour palette (copy from `console-device-smoke.c` PALETTE), `axl_console_term_new`
  (calloc, resolve font = cfg->font ?: `axl_gfx_default_font()`, copy palette or default,
  alloc `screen = axl_calloc(rows*cols, sizeof(Cell))`, wire the `ops` vtable to static
  `term_output_text`/`term_set_cursor`/`term_set_pen`/`term_clear_screen`/
  `term_set_term_prop` whose `user` is the `AxlConsoleTerm*`), `axl_console_term_ops`
  (returns `&t->ops`, `*user = t`), `axl_console_term_free`. Port `grid_put_cp` /
  UTF-8 decode / cell write from the smoke's GRID_OPS (see referenced lines). Add the
  two `_test_*` seams. `AXL_LOG_DOMAIN("conterm")`.

- [ ] **Step 5: Wire the build** — add `src/util/axl-console-term.c` to `LIB_SOURCES`
  (after `axl-console-input.c`); add `#include <axl/axl-console-term.h>` to `include/axl.h`
  (after the console-device include).

- [ ] **Step 6: Register + run the test**

Run: `make ARCH=x64 2>&1 | grep -iE 'error|warning'; TEST_APPS_ONLY=AxlTestUtil timeout 90 ./test/integration/test-axl.sh --arch X64 2>&1 | grep -iE 'term:|Results'`
Expected: all `term:` lines PASS.

- [ ] **Step 7: Both arches + gates**

Run: `make ARCH=aa64 2>&1 | grep -iE 'error|warning' || echo clean; make check-ascii 2>&1 | tail -1`
Expected: clean.

- [ ] **Step 8: Commit**

```bash
git add include/axl/axl-console-term.h src/util/axl-console-term.c Makefile include/axl.h test/unit/axl-test-util.c
git commit -m "console: AxlConsoleTerm cell model + output ops (local AxlConsoleOps sink)"
```

---

### Task 3: Scrollback (history ring + `scroll`)

**Files:** Modify `src/util/axl-console-term.c`, `include/axl/axl-console-term.h`;
Test: `test/unit/axl-test-util.c`.

**Interfaces:**
- Produces: `void axl_console_term_scroll(AxlConsoleTerm *t, int32_t delta_rows);`
  (positive = back into history, negative = toward live; clamps to `[0, filled_history]`);
  internal: on a newline past the last row, the top visible row is pushed into a
  `Cell *history` ring (`scrollback_rows * cols`), `hist_fill` grows to the cap. A
  `scroll_off` (0 = live) selects what the (future) renderer + selection see.
- Seam: `uint32_t _axl_console_term_test_scroll_off(AxlConsoleTerm *t);` and
  `bool _axl_console_term_test_hist_cell(AxlConsoleTerm *t, uint32_t rows_back, uint32_t col, char *utf8_out);`

- [ ] **Step 1: Failing test** — feed `rows+3` lines of distinct text, assert the
  earliest lines are retrievable from history via `_test_hist_cell`, and that
  `axl_console_term_scroll(t, +2)` sets `scroll_off == 2` while `scroll(t, +999)`
  clamps to `hist_fill`, and `scroll(t, -999)` returns to 0. Full assertions with
  `axl_strcmp` on the recalled glyphs.
- [ ] **Step 2: RED** (`axl_console_term_scroll` undefined).
- [ ] **Step 3: Implement** the history ring: add `Cell *history; uint32_t
  scrollback_rows, hist_head, hist_fill, scroll_off;` to the struct; in the newline
  path (when `cur_row` would exceed `rows-1`), before scrolling the screen, copy row 0
  into `history[hist_head]`, advance `hist_head`/`hist_fill` (ring, capped). Implement
  `axl_console_term_scroll` with clamping. Alloc `history` in `new` when
  `scrollback_rows > 0` (default e.g. 1000 when cfg is 0).
- [ ] **Step 4: GREEN** (run AxlTestUtil).
- [ ] **Step 5: Both arches.**
- [ ] **Step 6: Commit** `console: AxlConsoleTerm scrollback ring + scroll()`.

---

### Task 4: Rendering to a target (`render`, dirty rows)

**Files:** Modify `src/util/axl-console-term.c`, `include/axl/axl-console-term.h`;
Test: `test/unit/axl-test-util.c`.

**Interfaces:**
- Produces: `void axl_console_term_render(AxlConsoleTerm *t);` — blits dirty rows to
  the target (`cfg.target` buffer via `axl_gfx_target_buffer`, else GOP), at the bounds
  origin, using `axl_gfx_fill_rect` for cell bg + `axl_gfx_draw_text` for glyphs +
  a cursor caret; clears dirty flags; restores the previous target.
- Internal: a per-row `bool *dirty` (or a dirty-row range); ops mark rows dirty.

- [ ] **Step 1: Failing test** — create a small `AxlGfxBuffer` (e.g. via
  `axl_gfx_buffer_new(cell_w*4, cell_h*2)` — confirm the ctor name in
  `axl-gfx-surface.h`), a 4x2 term with `cfg.target = buf`, `output_text(u,"A",1)`,
  `render(t)`, then read the buffer's pixels for the 'A' cell and assert at least one
  non-background pixel (glyph drawn) and the bg rect matches `palette[bg]`. Use the
  buffer pixel accessor from `axl-gfx-surface.h` (confirm name).
- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement** `render`: `AxlGfxBuffer *prev = axl_gfx_get_current_target();
  axl_gfx_target_buffer(t->cfg_target);` (store the target ptr at new()); for each
  dirty visible row (accounting for `scroll_off` — render history rows when scrolled
  back), for each col: `axl_gfx_fill_rect(x0+c*cw, y0+r*ch, cw, ch, palette[bg])`, then
  if glyph `axl_gfx_draw_text(font, ..., palette[fg], 1)`; draw the cursor caret if
  `cursor_visible && scroll_off == 0`; clear dirty; `axl_gfx_target_buffer(prev);`.
  Follow the smoke's `blit_cb` (lines ~363-386) for the exact draw calls.
- [ ] **Step 4: GREEN.**
- [ ] **Step 5: Both arches.**
- [ ] **Step 6: Commit** `console: AxlConsoleTerm render() to GOP or offscreen buffer`.

---

### Task 5: Reflow (`set_font`, `resize`, `set_bounds`, `set_palette`)

**Files:** Modify `src/util/axl-console-term.c`, `include/axl/axl-console-term.h`;
Test: `test/unit/axl-test-util.c`.

**Interfaces:**
- Produces: `void axl_console_term_set_font(AxlConsoleTerm *t, const AxlFont *font);`
  `void axl_console_term_resize(AxlConsoleTerm *t, uint32_t cols, uint32_t rows);`
  `void axl_console_term_set_bounds(AxlConsoleTerm *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h);`
  `void axl_console_term_set_palette(AxlConsoleTerm *t, const AxlGfxPixel *palette /*16*/);`

- [ ] **Step 1: Failing test** — `resize(t, 40, 10)` then assert new geometry via a
  `_test_geometry` seam + that a subsequent `output_text` lands within the new grid;
  `set_font` to a different `axl_gfx_*` font (pick a second font available from
  `axl-font.h`) then assert cell metrics changed via the seam. `set_palette` swaps a
  colour and a re-render/`_test_cell` reflects it (colour is an index; assert the
  palette entry the renderer would use — a `_test_palette(t, idx)` seam returning the
  AxlGfxPixel).
- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement** — `resize` reallocs `screen` (+ history if cols changed),
  clamps cursor, marks all dirty; `set_font` updates `font` + cached `cw`/`ch`, marks
  all dirty; `set_bounds` updates origin/extent + all dirty; `set_palette` copies 16
  entries + all dirty. Add the `_test_geometry`/`_test_palette` seams.
- [ ] **Step 4: GREEN.** **Step 5: Both arches.**
- [ ] **Step 6: Commit** `console: AxlConsoleTerm reflow (set_font/resize/bounds/palette)`.

---

### Task 6: Selection + copy

**Files:** Modify `src/util/axl-console-term.c`, `include/axl/axl-console-term.h`;
Test: `test/unit/axl-test-util.c`.

**Interfaces:**
- Produces: `void axl_console_term_selection_start(AxlConsoleTerm *t, uint32_t col, uint32_t row);`
  `void axl_console_term_selection_extend(AxlConsoleTerm *t, uint32_t col, uint32_t row);`
  `void axl_console_term_selection_clear(AxlConsoleTerm *t);`
  `int  axl_console_term_selection_copy(AxlConsoleTerm *t);` (extract selected text,
  trim trailing blanks per line, `\n`-join, → `axl-clipboard`; AXL_OK/AXL_ERR).
- Consumes: `axl-clipboard.h` (confirm `axl_clipboard_set` signature).

- [ ] **Step 1: Failing test** — output "Hello" on row 0, `selection_start(0,0)` +
  `selection_extend(4,0)` (viewport coords: cols 0..4 → "Hello"), `selection_copy(t)`,
  then read `axl-clipboard` and assert bytes == "Hello" (exact `axl_strcmp`). A second
  case spanning two rows asserts the `\n` join + trailing-blank trim.
- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement** — a `sel_active` + `sel_a{row,col}` + `sel_b{row,col}`
  (normalized). `selection_copy` walks the selected cells (mapping viewport rows to
  history/screen via `scroll_off`), builds a UTF-8 string in an `AxlStrBuf`, trims
  per-line trailing blanks, joins with `\n`, calls `axl_clipboard_set`. Selection also
  marks affected rows dirty (the renderer inverts selected cells — a small addition to
  Task 4's render, note it here).
- [ ] **Step 4: GREEN.** **Step 5: Both arches.**
- [ ] **Step 6: Commit** `console: AxlConsoleTerm selection + copy (axl-clipboard)`.

---

### Task 7: Interaction conveniences (`handle_pointer`, `handle_hotkey`)

**Files:** Modify `src/util/axl-console-term.c`, `include/axl/axl-console-term.h`;
Test: `test/unit/axl-test-util.c`.

**Interfaces:**
- Produces: `void axl_console_term_handle_pointer(AxlConsoleTerm *t, const AxlInputEvent *e);`
  (confirm the pointer-event type in `axl-input.h`; wheel→`scroll`, drag→selection,
  Ctrl+wheel→`cfg.on_zoom`), and
  `bool axl_console_term_handle_hotkey(AxlConsoleTerm *t, const void *key);`
  (Shift+PgUp/PgDn→`scroll(±rows)`, Ctrl+Shift+C→`selection_copy`; returns true if
  consumed). `handle_hotkey`'s signature matches the device `key_filter` so a thunk is
  trivial.

- [ ] **Step 1: Failing test** — drive `handle_hotkey` with a synthesized Shift+PgUp
  `EFI_KEY_DATA` (cast through `const void*`), assert it returns true and `scroll_off`
  moved by `rows`; a plain 'a' returns false. For `handle_pointer`, synthesize a wheel
  event and assert `scroll_off` changed; a Ctrl+wheel event calls a test `on_zoom` that
  bumps a counter. (Build the `AxlInputEvent` per `axl-input.h`.)
- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement** both, reusing the SCAN_PGUP/PGDN defines (0x09/0x0A) and
  the shift-state bits already used in Task 1. `handle_hotkey` casts `key` to
  `EFI_KEY_DATA*`.
- [ ] **Step 4: GREEN.** **Step 5: Both arches.**
- [ ] **Step 6: Commit** `console: AxlConsoleTerm handle_pointer + handle_hotkey`.

---

### Task 8: Docs wiring (doxygenfile + README + design-doc status)

**Files:** Modify `docs/sphinx/modules/console-mirror.rst`, a `src/*/README.md`,
`docs/AXL-Console-Terminal-Design.md`.

- [ ] **Step 1:** Add `.. doxygenfile:: axl-console-term.h` to
  `docs/sphinx/modules/console-mirror.rst` (after the `axl-console-device.h` one).
- [ ] **Step 2:** Add a short "AxlConsoleTerm" section to the console README pulled
  into Sphinx (find via `grep -rl console docs/sphinx/modules/*.rst` → the included
  README), describing the local-sink role + the mechanism/policy split.
- [ ] **Step 3:** Flip `docs/AXL-Console-Terminal-Design.md` status header from
  "DESIGNED, not built" to "BUILT (component); fbcon in Task 9".
- [ ] **Step 4:** `make check-docs 2>&1 | tail -1` → clean (no undocumented-header gap).
- [ ] **Step 5: Commit** `docs: wire AxlConsoleTerm into Sphinx + design status`.

---

### Task 9: `fbcon` tool

**Files:** Create `tools/fbcon.c`; Modify `Makefile` (`fbcon` target + `.PHONY`).

**Interfaces:** Consumes everything above + `axl-console-device` (with `key_filter`) +
`axl-input` (attach_mouse) + `axl-loop`. Produces a resident `AXL_DRIVER` EFI.

- [ ] **Step 1: Write `tools/fbcon.c`** — full driver per the design §Piece 3: resolve
  GOP (`axl_gfx_get_info`), pick `axl_gfx_default_font()`, compute `cols =
  info.width/cell_w`, `rows = info.height/cell_h`; `term = axl_console_term_new(&{cols,
  rows})`; `axl_console_device_install(&dev, axl_console_term_ops(term,&u), u, &{
  .cols=cols,.rows=rows,.take_input=true,.read_physical=true,
  .key_filter=hotkey_thunk,.key_filter_user=term })`; attach a pointer if present
  (`axl_input_attach_mouse` — confirm name) and, in the loop timer, drain pointer events
  → `axl_console_term_handle_pointer`, then `axl_console_term_render(term)`; unload →
  uninstall + free. `hotkey_thunk(user,key){ return axl_console_term_handle_hotkey(user,key); }`.
- [ ] **Step 2: Add the Makefile target** mirroring an existing tool (e.g. `mkrd`):
  `fbcon: $(PREFIX)/tools/fbcon.efi` + the `.o`/link rules + `.PHONY`. Build:
  `make ARCH=x64 fbcon 2>&1 | grep -iE 'error|warning|Built'`.
- [ ] **Step 3: Both arches build.**
- [ ] **Step 4: Commit** `tools: fbcon -- graphical terminal take-over of the UEFI shell`.

---

### Task 10: fbcon firmware smoke

**Files:** Modify `test/integration/analyze-console-device-shot.py` (reuse `--input`),
`test/integration/test-console-device-qemu.sh` (Scenario 6). fbcon builds as a driver
already (Task 9); the smoke `load`s it.

- [ ] **Step 1:** Add Scenario 6 to `test-console-device-qemu.sh`: build `fbcon`
  (add to the build list + existence check), then
  `run_scenario "fbcon" "$DRV_DIR/../tools/fbcon.efi" 20 "v e r ret" "--input"
  "fbcon renders the shell + delivers keystrokes through the relay"`. (Confirm the
  driver path; if `fbcon.efi` isn't PE-subsystem-11, run-qemu won't `load` it — build
  it as a driver like the smokes. If it must live under drivers/, add a driver target.)
- [ ] **Step 2:** Run `timeout 480 ./test/integration/test-console-device-qemu.sh
  --arch X64 2>&1 | tail -20`. Expected: all scenarios pass incl. `fbcon`. Inspect the
  screenshot to confirm `ver` output rendered (keys via relay through the terminal).
- [ ] **Step 3:** Update the harness header ("Six scenarios") + `est`.
- [ ] **Step 4: Commit** `test: fbcon DEBUG-OVMF smoke (renders shell + relays keys)`.

---

## Post-plan: final review + release note

After Task 10: run the full unit suite both arches (expect the new `term:` assertions
added to the ratchet), `make check-ascii`/`check-docs`, an independent code-review of
the whole increment (per `feedback_code_review_before_commit`), then update the AGT
handoff (axcon can now re-parent `AgtTerminal` onto `AxlConsoleTerm`). Do NOT push
(gated).

## Self-Review notes

- **Spec coverage:** device key_filter (T1), cell model+ops (T2), scrollback (T3),
  render incl. offscreen target (T4), reflow/zoom (T5), selection+copy (T6),
  handle_pointer/handle_hotkey (T7), docs (T8), fbcon (T9), firmware smoke (T10). All
  spec sections mapped.
- **Unverified API names to confirm at execution time** (marked inline): the
  `AxlGfxBuffer` ctor + pixel accessor + `axl_gfx_buffer_new` in `axl-gfx-surface.h`;
  `axl_clipboard_set` in `axl-clipboard.h`; the `AxlInputEvent`/pointer type + wheel
  fields + `axl_input_attach_mouse` in `axl-input.h`. Each task's Step 1 says "confirm
  the name" — resolve by reading the header before writing the test, not by guessing.
- **Type consistency:** `AxlConsoleTerm`/`AxlConsoleTermConfig` and every
  `axl_console_term_*` name is used identically across tasks; seams are
  `_axl_console_term_test_*`.
