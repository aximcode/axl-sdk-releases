# AxlConsoleScreen — public screen model + snapshot serializer (handoff, 2026-07-14)

**Repo:** axl-sdk. **GATED** — commit OK, do NOT push, never `git add -A`, leave
`test/integration/.last-pass-count` + `devkit.conf` (pre-existing) uncommitted.
Self-contained; RESUME POINT for the SoftBMC screen-snapshot feature (the deferred
"Finding B" from the console/vterm review, now greenlit).

## The ask (from SoftBMC)

Unblock **SOL (serial-over-LAN) late-join**: a WS client that connects mid-session
sees a blank screen until the guest next repaints. SoftBMC wants a **server-side
terminal screen model** it feeds the serial stream, and on a new client connect,
**serialize the current screen to a VT "repaint"** so the late joiner immediately
sees what everyone else sees. The *same* helper fixes `axl-console-mirror`'s
late-join (raw byte-tail replay can't recover screen contents or alt-screen state).

Full ask + acceptance: `softbmc/docs/axl-sdk-screen-snapshot-handoff.md` (the API
shape below is copied from it). Alt-screen background:
`softbmc/docs/axl-sdk-console-vterm-handoff.md` (that alt-screen finding is FIXED in
axl-sdk `9e0f3c03`, so a mirror snapshot must also carry the alt-screen flag).

## Proposed public API (shape from SoftBMC; axl-sdk owns final naming)

New header `include/axl/axl-console-screen.h`, impl in `src/vterm/`:

```c
typedef struct AxlConsoleScreen AxlConsoleScreen;
AxlStatus axl_console_screen_new(AxlConsoleScreen **out, uint32_t rows, uint32_t cols);
void      axl_console_screen_feed(AxlConsoleScreen *s, const uint8_t *bytes, size_t len);
// Serialize the CURRENT screen as a self-contained VT repaint (clear + per-cell
// text with run-length pen + final cursor pos/visibility + alt-screen state), UTF-8,
// coalesced (a mostly-empty 80x25 must NOT be ~2KB of spaces). Emitted via a sink.
AxlStatus axl_console_screen_snapshot(const AxlConsoleScreen *s,
              void (*sink)(const char *bytes, size_t len, void *user), void *user);
void      axl_console_screen_resize(AxlConsoleScreen *s, uint32_t rows, uint32_t cols);
void      axl_console_screen_free(AxlConsoleScreen *s);
```

Consumers: SOL (`softbmc/src/sol.c`) — `new` on port open, `feed` on rx (still
broadcasts raw bytes live), `snapshot(ws_sink, client)` on connect then live stream,
`resize` on browser resize, `free` on close. console-mirror uses `snapshot()` on WS
connect instead of the raw byte-tail replay. **No SoftBMC code ships against this
yet** — design the API cleanly first (contract-first review — see below).

## Design decision — the fork (RECOMMENDATION: Option B)

**Option A — compile in libvterm Layer 3 (`screen.c`).** `deps/libvterm/src/screen.c`
IS vendored (35KB) but excluded from the build; commit `2d035747` dropped the
`AXL_VTERM_WITH_SCREEN` gate (see `Makefile:482` "screen.c is deliberately absent
[from the compile set]"). You'd re-add it to the build and read cells back via
`vterm_screen_get_cell`. Pro: libvterm handles every VT edge (wide chars, scrollback,
reflow). Con: re-introduces the compiled dep + gate; `axl-vterm.h` Layer 2 binds
libvterm's STATE layer, not the screen layer, so mixing is awkward; heavier.

**Option B (RECOMMENDED) — own a lean cell grid fed by the existing Layer 2.**
`AxlConsoleScreen` implements `AxlConsoleOps` and feeds an owned rows x cols cell
grid; drive it with the ALREADY-COMPILED, tested `axl_vterm` (Layer 2):
`axl_vterm_new(&v, rows, cols, &screen_ops, screen)` + `axl_vterm_feed`. Then
`snapshot` walks the grid and emits VT. **Prior art to reuse (do NOT hand-roll):**
- **`src/util/axl-console-term.c`** already owns a `TermCell` grid (cells+len, fg/bg
  pen, cursor row/col, scrollback) fed by exactly these ops — copy its grid model +
  ops handlers (output_text at cursor with current pen, clear_screen, set_cursor,
  set_pen, set_cell_rule, set_term_prop -> alt-screen). Keep it LEAN (no GOP/font/
  render — just the model).
- **`src/util/axl-console-mirror.c`** already serializes the reverse direction
  (AxlConsoleOps -> VT bytes) incl. pen->SGR (`~:207`) and alt-screen 1049h/l
  (`:241`). The snapshot is its cousin: walk the grid, emit cursor-address (CUP) +
  run-length SGR + text, then final cursor pos/visibility + alt-screen. Reuse its SGR
  + escape helpers.
Pro: no new compiled dep, full control of the coalesced snapshot format, reuses two
tested modules. Con: a cell grid (but it already exists to copy). This aligns with
"don't compile a 36KB dep when the grid + serializer already exist in-tree."

Whichever you pick: **the snapshot must round-trip** (feed -> snapshot -> feed into a
fresh screen -> identical grid), so the grid model + serializer are the same in both
directions.

## Implementation plan (test-first, STRICT — new public API)

1. **Header + docstrings FIRST** (`include/axl/axl-console-screen.h`) — the docstring
   IS the contract (rows/cols semantics, feed byte ownership, snapshot self-
   containment + coalescing guarantee, sink lifetime, resize reflow behavior, NULL-
   safety, AxlStatus fields). **Contract-first independent review before implementing**
   (`feedback_code_review_before_commit`) — this is the expensive-to-change surface.
2. **Failing tests pinning the acceptance:** the SoftBMC round-trip — feed a known
   ANSI seq (CUP + SGR-colored text + a mid clear + alt-screen enter), snapshot, feed
   the snapshot into a SECOND fresh screen, assert identical grids (cells + pen +
   cursor row/col + cursor visibility + alt flag). Plus: coalescing (a blank 80x25
   snapshot is small — assert a byte-size bound, e.g. < 200 B, not ~2KB); exact VT for
   a tiny known screen (`axl_strcmp` exact, per the output-format rule); resize;
   NULL-safety. Test binary: `test/unit/axl-test-vterm.c` already tests axl-vterm +
   has the `probe_*` AxlConsoleOps + the `cm_cap` VT-capture pattern to model against.
   Confirm RED.
3. Implement (Option B), confirm GREEN, both arches, no ratchet drop.
4. **Doc-sync (MANDATORY, same change):** add `.. doxygenfile:: axl-console-screen.h`
   to the vterm module `.rst`; a `modules/*.rst` page if it's a standalone type; the
   `src/vterm/README.md` prose; the **module table in `CLAUDE.md`**; `make check-docs`.
5. Independent review, commit. GATED — no push. Ping SoftBMC when it lands so they
   wire SOL + the mirror against the real API.

## Key facts / files

| Purpose | Path |
|---|---|
| The ask (full) + acceptance | `softbmc/docs/axl-sdk-screen-snapshot-handoff.md` |
| Layer 2 parser to drive the grid | `include/axl/axl-vterm.h` (`axl_vterm_new`/`_feed`/`_resize`/`_free`), impl `src/vterm/axl-vterm.c` |
| Cell-grid model to copy (lean, drop the render) | `src/util/axl-console-term.c` (`TermCell`, ops handlers) |
| Reverse serializer (ops->VT, SGR, 1049h/l) to mirror | `src/util/axl-console-mirror.c` (`~207` SGR, `241` alt) |
| libvterm screen layer (Option A) | `deps/libvterm/src/screen.c` (vendored, uncompiled; `2d035747`, `Makefile:482`) |
| AxlConsoleOps vtable | `include/axl/axl-console-ops.h` (set_cell_rule/clear_screen/set_cursor/set_pen/output_text/set_term_prop) |
| New home | `include/axl/axl-console-screen.h` + `src/vterm/axl-console-screen.c` |
| Tests to model + extend | `test/unit/axl-test-vterm.c` (`probe_*` ops, `cm_cap` capture) |

## Notes

- **UTF-8 out** (SoftBMC frames the snapshot as a WS TEXT frame); input is arbitrary
  bytes.
- The snapshot is **self-contained** (applied to a blank terminal -> identical to
  every live viewer), which is exactly the mirror's alt-screen late-join fix too.
- Prior state this session (all committed, GATED/unpushed): the auto_alt_screen
  bracket fix (`9e0f3c03`) is why a mirror snapshot must carry the alt-screen flag;
  the fbcon Ctrl+C / `-d/-g` fixes are unrelated. See
  `[[project_auto_alt_screen_redesign_2026-07-14]]`, `[[feedback_tdd_mandatory]]`,
  `[[feedback_code_review_before_commit]]`, `[[feedback_change_apis_freely]]`.
