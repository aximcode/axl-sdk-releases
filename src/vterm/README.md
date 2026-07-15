# AxlVterm — the second `AxlConsoleOps` producer

`axl-vterm` parses a real VT/xterm byte stream — a serial / SOL console, or any
pipe carrying xterm escapes — into the structured
[`AxlConsoleOps`](../util/README.md) operations a consumer renders. It is the
**second producer** behind that contract; the first is `axl-console-tap`, which
sources the same ops from UEFI's `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`. A consumer
binds one vtable and renders both — a nested UEFI Shell and a remote serial pane
paint through identical code.

## Why this is the module that validates the contract

`AxlConsoleOps` was widened (rects, scroll, RGB pens, the `set_term_prop`
channel) specifically so a VT parser could speak it. The tap can never exercise
those ops — it only observes `OutputString` / `SetCursorPosition`. `axl-vterm` is
the only producer that drives `scrollrect`, `moverect`, `erase`, and RGB pens, so
it is where the widened contract is actually proven end to end.

## Layer 2 only

Built over vendored [libvterm](../../deps/libvterm/README.md) (MIT), binding its
**Layer 2** (`VTermStateCallbacks`) only. libvterm's own cell grid (Layer 3,
`screen.c`) is never compiled, because the consuming terminal widget already owns
a grid and a scrollback ring. Layer 3 would be a second grid the two would then
have to keep in sync — and, more fundamentally, its damage-rect *pull* model is a
shape the tap could never implement, so it is the wrong contract for a
two-producer world. Layer 2's push op-stream is the only shape both producers
speak.

Two consequences of binding Layer 2 that this adapter absorbs so the consumer
does not:

- **Glyph coalescing.** libvterm's `putglyph` is a *positioned single glyph*, but
  `AxlConsoleOps::output_text` is a cursor-relative *run*. The adapter buffers
  consecutive glyphs at advancing positions into one run, flushing on a position
  jump, a pen change, or any other op. It tracks a logical cursor so a
  `movecursor` to where a run already left the cursor emits no redundant
  `set_cursor`. Because a run can remain buffered when `feed()` returns, the API
  exposes `axl_vterm_flush()` for the consumer to call at a repaint boundary.

- **Pen accumulation.** libvterm hands the pen out incrementally (`initpen` +
  one `setpenattr` per attribute), but `AxlConsolePen` is a snapshot. The adapter
  accumulates the attributes and emits `set_pen` lazily — right before a paint —
  so a burst of `setpenattr` collapses into one `set_pen`. This is the deliberate
  "Layer 2.5" slice: a minimal piece of Layer 3's bookkeeping, done once here
  instead of in every consumer.

## Two load-bearing returns

Every Layer-2 callback returns `int` — a "did you handle it?" protocol — and two
of those returns are propagated verbatim through `AxlConsoleOps`:

- **`scrollrect`.** The adapter returns exactly what the consumer's `scrollrect`
  returned (0 if unbound). A 0 (decline) lets libvterm's `vterm_scroll_rect()`
  decompose the scroll into `moverect` + `erase`; a non-zero (the consumer blit
  it itself) suppresses the decomposition. Because that decomposition
  dereferences the erase callback unconditionally, **a consumer that can ever
  decline a scroll must bind `erase`** (see `axl_vterm_new`).

- **`set_term_prop`.** The adapter returns what the consumer's `set_term_prop`
  returned (1 if unbound). libvterm only latches a property if the callback said
  it was happy — notably for alt-screen switching. Returning 0 unconditionally
  would silently break the alternate screen.

The rest (`putglyph`, `movecursor`, `erase`, `bell`, …) return 1 unconditionally;
their returns are only decoration inside libvterm.

`moverect`'s argument order is `(dest, src)` — matching libvterm's `vterm.c` call
site, not the mis-named parameters in `vterm.h`'s prototype.

## Scrollback: only `sb_clear`

Layer 2 keeps no scrollback and exposes no `sb_pushline` (that is Layer 3). Its
one scrollback callback is `sb_clear`, driven by xterm's `CSI 3J`; the adapter
maps it to `AxlConsoleOps::clear_scrollback`. The consumer owns the ring — lines
leaving the top of the screen arrive as an ordinary `moverect` + `erase`, and the
consumer captures them there — so `clear_scrollback` is the only history op the
adapter can offer.

## One width authority

`axl_vterm_char_width()` wraps libvterm's own `wcwidth`/combining/fullwidth
tables and is the **single** width authority shared by producer and consumer: 2
for double-width (CJK, emoji), 0 for combining marks and other zero-width
codepoints, 1 otherwise. A consumer decoding an `output_text` run under
`AXL_CONSOLE_CELLS_WIDTH_RESOLVED` uses it to rebuild cell boundaries, merging
zero-width codepoints into the preceding cell. There must not be a second table.

## What the consumer sees of libvterm: nothing

libvterm is an implementation detail behind this module — an SDK consumer cannot
reach its headers, exactly as lzma sits behind `axl-compress` and mbedTLS behind
`axl-tls`. Consumers depend only on `axl/axl-vterm.h` and `axl/axl-console-tap.h`.

## `AxlConsoleScreen`: a snapshot-serializing screen model

`<axl/axl-console-screen.h>` builds a **server-side terminal screen model** on
top of this parser, for **late-join repaint** of a shared console (SOL / mirror
WebSocket viewers). It drives an `AxlVterm` into an owned rows×cols cell grid and
serializes that grid back to a self-contained VT "repaint": feed it the live
serial stream, and on a new client connect,
`axl_console_screen_snapshot()` emits one burst that — applied to a blank
terminal of the same size — reproduces the current screen, so the late joiner
immediately sees what every existing viewer sees.

It is the inverse of `axl-console-mirror`'s encoder and shares its pen→SGR
encoder (`src/util/axl-console-vt.h`), so the two serializers cannot drift. The
snapshot is **coalesced** — blank cells in the default background and fully-blank
rows emit nothing, and consecutive cells sharing a pen collapse under one SGR — so
a mostly-empty 80×25 screen is a handful of bytes, not ~2 KB of spaces. The model
owns a **primary and an alternate grid**, swapping on the guest's `DECSET/DECRST
1049` exactly as this parser reports it, so the primary survives a full-screen app
and a snapshot taken after it exits repaints the intact primary — not the stale
alternate buffer. The repaint carries cell glyphs, per-cell pen (indexed / RGB /
styles), cursor position and visibility, the active/alternate selection, and
whole-screen reverse video (DECSCNM).
