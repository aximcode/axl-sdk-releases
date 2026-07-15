# libvterm (vendored)

An abstract VT/xterm terminal emulator library. AXL vendors it to parse a real
VT byte stream (a serial / SOL console) into structured terminal operations,
as the second producer behind the `AxlConsoleOps` contract — the first being
`axl-console-tap`, which sources the same ops from UEFI's
`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/neovim/libvterm> (neovim's maintained fork) |
| Commit | `934bc2fbf21800ac3458a499df8820ca5fb45fd3` (2025-11-20) |
| Version | `VTERM_VERSION` 0.3.3 |
| License | MIT (see `LICENSE`) |

**The fork is not ABI-identical to upstream 0.3.3 despite reporting the same
version macros.** Relative to Paul Evans' 0.3.3, neovim's fork adds
`VTermStateCallbacks.premove` (Layer 2) and `VTermScreenCallbacks.sb_pushline4`
(Layer 3). Do not verify this vendored copy's contract against a distro
`/usr/include/vterm.h` — read `include/vterm.h` here.

`premove` is opt-in: it fires only after a call to
`vterm_state_callbacks_has_premove()`, so leaving it NULL is safe.

## What is vendored: everything. What is bound: Layer 2.

The complete library is vendored. Which layer AXL *binds* is a separate,
deliberate choice from which files we *ship*.

libvterm is layered:

| Layer | Files | Vendored | Compiled |
|---|---|---|---|
| 1 — parser (bytes to callbacks) | `parser.c`, `encoding.c` | yes | yes |
| 2 — state (a positioned, pen-attributed op stream) | `state.c`, `pen.c`, `unicode.c`, `vterm.c` | yes | yes |
| 2 — input encoding (keys/mouse to bytes) | `keyboard.c`, `mouse.c` | yes | yes |
| 3 — screen (a full cell grid + scrollback) | `screen.c` | yes | **no** |

AXL binds Layer 2 (`VTermStateCallbacks`) for two reasons:

1. The consumer (a terminal widget) already owns a cell grid. Layer 3 would be a
   second one, and then the two must agree on which is the truth.
2. More fundamentally, `AxlConsoleOps` is a **two-producer contract**:
   `axl-console-tap` (over `EFI_SIMPLE_TEXT_OUTPUT`) and `axl-vterm` (over a VT
   byte stream) both push the same ops. The tap has no grid and never can — it
   only observes `OutputString` / `SetCursorPosition` calls. A contract shaped
   like Layer 3's damage-rect *pull* model is one the tap cannot implement.
   Layer 2's op stream is the only shape both producers can speak.

`screen.c` is retained on disk but **never compiled** — it is absent from
`LIB_SOURCES`. Keeping the file makes this a complete copy of upstream, so a
re-vendor is a plain `diff` rather than a file-by-file copy list; not compiling
it keeps ~1200 lines of dead grid code out of every image that merely frees a
`VTerm` (see patch 3).

There is deliberately **no build flag** for this. A flag would create a
configuration CI never exercises, and the patch guarding it would rot unnoticed.
`screen.c` is therefore left **byte-identical to upstream, carrying zero
patches**: a maintainer who adds it to `LIB_SOURCES` gets an immediate compile
error on its `fprintf`/`abort`, which is the honest signal that Layer 3 has never
been made freestanding here.

### What a consumer sees: nothing

libvterm is unconditional in `libaxl.a`, like `deps/lzma` (and unlike the
51-file `deps/mbedtls`, which `AXL_TLS=1` gates). It costs nothing in an image
that does not use it: `libaxl.a` is selectively linked per object, and no
`src/` or `tools/` file references a `vterm_*` symbol.

An SDK consumer cannot reach libvterm at all — `scripts/install.sh` packages none
of these headers, and `axl-cc` puts only `${AXL_INCLUDE_DIR}` and its `compat/`
on `-isystem`. libvterm is an implementation detail behind the `axl-vterm`
module, exactly as lzma sits behind `axl-compress` and mbedTLS behind `axl-tls`.
A consumer such as `axterm` needs no flag and no knowledge of any of this.

In-tree translation units (tools, tests) *can* include `<vterm.h>`, since
`-Ideps/libvterm/include` is on the global `INCLUDES`. Calling a Layer-3 entry
point from one fails **loudly at link** (`undefined reference to
'vterm_obtain_screen'`), never silently at runtime.

Consequences of binding Layer 2, worth knowing:

- Layer 2 has **no** `sb_pushline` / `sb_popline`; those are `VTermScreenCallbacks`.
  Its only scrollback callback is `sb_clear`. The consumer owns the ring.
- Layer 2 hands you the pen **incrementally** (`initpen` + `setpenattr`, one
  attribute at a time). Assembling a pen struct is Layer 3's job.
- `vterm_scroll_rect()` — which decomposes a declined `scrollrect` into
  `moverect` + `eraserect` — lives in `vterm.c`, not `screen.c`, so it survives.
  This is why every Layer-2 callback returns `int`: it is a "did you handle it?"
  protocol. A consumer that cannot blit returns 0 and gets ordinary damage.

`keyboard.c` and `mouse.c` are leaves — nothing in `state.c` calls them. They
are vendored because `vterm_keyboard_key()` owns DECCKM-aware key encoding,
which a bidirectional pane (SOL) needs. `libaxl.a` is selectively linked, so an
image that never calls them carries none of their code.

## Freestanding fit

Built with the same `-ffreestanding` flags as the rest of AXL. All `printf` /
`fprintf` / `abort` calls in the *compiled* files sit behind `#ifdef DEBUG`,
`DEBUG_PARSER`, `DEBUG_PRINT_UTF8`, or `DEBUG_GLYPH_COMBINE`, none of which we
define. (`screen.c` has one ungated `fprintf` + `abort`, in `screen_resize`'s
invariant check — the reason an unmodified `screen.c` cannot simply be dropped
into `LIB_SOURCES`.) libvterm carries its own `wcwidth` / combining / fullwidth
tables with the generated `.inc` files committed, so no Perl is needed at build
time, and it uses no `<ctype.h>`, `<wchar.h>`, locale, `<assert.h>`, or floating
point.

External symbols the Layer-2 objects actually reference, and where each resolves:

| Symbol | Resolved by |
|---|---|
| `memcpy`, `memmove`, `memset` | `src/mem/axl-intrinsics.c` |
| `strncmp`, `strncpy` | `src/data/axl-str-compat.c` |
| `snprintf`, `vsnprintf`, `abs` | `axl-vterm-compat.h` (mapped to `axl_snprintf` / `axl_vsnprintf` / `__builtin_abs`) |
| `malloc`, `free` | eliminated by patch 2 |
| `exit`, `fprintf`, `stderr` | eliminated by patch 4 |
| `vterm_screen_free` | eliminated by patch 3 |

(`screen.c` is not compiled, so its own `abort` / `fprintf` never reach the link.)

## Local patches

Eight, confined to the five compiled files; `screen.c` is unmodified. Each is
marked in-source with an `AXL patch [n/8]` comment so a re-vendor can find them.
Seven are freestanding-portability or memory-safety fixes; patch 8 is the sole
*behavior* change, which raises re-vendor cost — see its entry.
To audit: `grep -rn 'AXL patch \[' deps/libvterm/src/`

1. **`src/vterm_internal.h`** — include `axl-vterm-compat.h` first, so the
   `snprintf` / `vsnprintf` / `abs` mappings are in scope for every Layer-2
   translation unit.
2. **`src/vterm.c`** — route the default allocator through `axl_calloc` /
   `axl_free` (upstream used `malloc` + `memset(0)`, so the zeroing semantics
   are preserved) and drop `<stdio.h>` / `<stdlib.h>`. This keeps plain
   `vterm_new()` working; callers need not reach for
   `vterm_new_with_allocator()`.
3. **`src/vterm.c`** — `vterm_free()` no longer calls `vterm_screen_free()`.
   That reference alone would make the linker pull all of `screen.c` into every
   image that frees a `VTerm`. `vt->screen` is only ever set by
   `vterm_obtain_screen()`, which also lives in the uncompiled `screen.c`, so it
   is provably NULL; an `AXL_DEBUG_ASSERT` states exactly that.
4. **`src/vterm.c`** — `vterm_check_version()` reported mismatches with
   `fprintf(stderr)` + `exit(1)`. libvterm is compiled from source into
   `libaxl.a`, so headers and library are the same tree by construction and
   cannot disagree at runtime. Downgraded to `AXL_DEBUG_ASSERT`; the symbol
   stays so `VTERM_CHECK_VERSION` still links.
5. **`src/parser.c`** — initialize `string_start = NULL` in
   `vterm_input_write()`. The switch assigns it in every arm of
   `enum VTermParserState` but has no `default:`, so GCC cannot prove
   exhaustiveness and warns under `-Wmaybe-uninitialized`. Cosmetic.
6. **`src/state.c`** — **upstream bug fix.** In the DECSCUSR query reply,
   `int reply` was left uninitialized when `state->mode.cursor_shape` matched
   none of `BLOCK` / `UNDERLINE` / `BAR_LEFT`. Upstream then decrements that
   uninitialized stack slot and transmits it to the host. Seeded with `2`
   (steady block, the terminal default; blink makes it `1`). Worth reporting
   upstream.

   Reachability, precisely: `vterm_state_new()` zeroes `mode.cursor_shape` and
   `vterm_obtain_state()` does **not** reset, while
   `VTERM_PROP_CURSORSHAPE_BLOCK` is `1` — so every arm misses until
   `vterm_state_reset()` runs and assigns `BLOCK`. The window is therefore a
   state that has not yet been reset (or one where a caller pushed an
   out-of-range `VTERM_PROP_CURSORSHAPE` through
   `vterm_state_set_termprop()`). A DECSCUSR query on an already-reset terminal
   is *not* affected, and the `DECSCUSR` handler itself cannot store `0`. So
   this is an uninitialized read reachable through ordinary API misuse, not a
   leak that any untrusted byte stream can trigger unaided.
   `test_decscusr_query_before_reset` in `test/unit/axl-test-vterm.c` pins it.

7. **`src/parser.c` + `src/pen.c`** — **upstream memory-safety fix (CSI argument
   bounds, producer + consumer).** The `CSI_ARGS` state
   incremented the argument index `argi` unconditionally on every `;`, so a CSI
   with more than `CSI_ARGS_MAX` (16) arguments wrote `args[16]`, `args[17]`, …
   past the end of `args[]` — and `args[]` is the last field of the `csi` union,
   so the overflow lands on the adjacent `parser.callbacks` pointer. That is an
   out-of-bounds write reachable from **untrusted input** (a serial / SOL byte
   stream feeding `CSI 1;2;…;20 m`), which corrupts the parser and drops the
   sequence plus any trailing text. Bounded on every write path (the digit
   accumulator, the `;`/`:` separators, and the index→count `argi++` on the
   final byte) so `argi` saturates at `CSI_ARGS_MAX` and `args[16]` is never
   written. The net behavior now matches the DEC / xterm / libtsm rule —
   **process the first 16 parameters and silently drop the rest** — rather than
   the earlier bounds-safe-but-last-wins collapse. Still present in the neovim
   fork we vendor; **Vim's** separate libvterm fork added its own bound in patch
   9.2.0279 (2026-04). Worth reporting upstream.

   The **consumer** side (`pen.c`, `vterm_state_setpen`) had the matching hazard:
   the SGR walk reads *past* the current arg — the `CSI 4:<n>` underline
   sub-parameter, the `38`/`48` alternative-palette selector, and the
   end-of-code MORE-flag skip all index `args[argi+1]` or advance `argi`. With a
   full 16-arg list the last in-count slot is `args[15]`, so an unbounded walk
   reads `args[16]` (onto `callbacks`). This is an **OOB read** present in
   pristine upstream too; keeping arg 16 (above) made the MORE-flag path
   reachable, which surfaced it. All four sites are now bounded by `argcount`.
   Pinned by `test_vterm_csi_argcount_overflow_is_bounded` (write stays in
   bounds), `test_vterm_csi_keeps_first_16_args` (first 16 kept, rest dropped),
   and `test_vterm_sgr_arg_walk_bounded_at_16` (the consumer walks survive a full
   arg list) in `test/unit/axl-test-vterm.c`.

8. **`src/state.c`** — **behavior change (the only one).** Restores SCOSC /
   SCORC, the ANSI.SYS save/restore-cursor pair (`CSI s` / `CSI u`). Upstream
   binds `CSI s` unconditionally to DECSLRM (set left/right margin) and has **no
   `CSI u` handler at all**, so both sequences — widely emitted by shell prompts
   and TUIs — are silently lost. xterm's rule: `CSI s` is SCOSC (save cursor)
   when left/right-margin mode is off and DECSLRM when it is on. The patch guards
   `case 0x73` on `state->mode.leftrightmargin`: off → `savecursor(state, 1)`
   (the same routine that backs DECSC/DECRC), on → the existing DECSLRM path,
   unchanged. It adds `case 0x75` calling `savecursor(state, 0)`.

   Unlike patches 1–7, this is **not** a freestanding-portability or
   memory-safety fix — it changes what the parser *does*, so a re-vendor must
   re-apply it by hand (a blind `diff` accept would drop it) and re-run the
   SCOSC/SCORC tests. `test_scosc_scorc_saves_and_restores_cursor` and
   `test_scosc_under_leftright_margin_is_decslrm` in
   `test/unit/axl-test-vterm.c` pin it.

## Re-vendoring

The vendored tree is a complete copy of upstream's `src/` and `include/`, so a
re-vendor is a diff rather than a copy list.

```sh
git clone --depth 1 https://github.com/neovim/libvterm.git upstream

# See exactly what we changed -- only the eight AXL patch [n/8] sites should differ.
diff -ru upstream/src     deps/libvterm/src
diff -ru upstream/include deps/libvterm/include   # expected: identical

# After updating, re-apply the eight patches, then:
make ARCH=x64 && make ARCH=aa64                   # must be warning-clean
TEST_APPS_ONLY=AxlTestVterm ./test/integration/test-axl.sh
TEST_APPS_ONLY=AxlTestVterm ./test/integration/test-axl.sh --arch AARCH64
```

`screen.c` carries no patches, so it diffs clean and needs nothing re-applied. If
you ever do want Layer 3, add it to `LIB_SOURCES` — it will fail to compile on
its ungated `fprintf`/`abort` — then fix those, restore the `vterm_screen_free()`
call that patch 3 removed, and add a test. Do not reintroduce a build flag that
nothing exercises.
