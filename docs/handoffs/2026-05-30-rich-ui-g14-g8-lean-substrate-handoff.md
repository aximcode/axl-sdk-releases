# Handoff — Rich UI: G14 + G8 lean stroker shipped & PUSHED (2026-05-29 → 30)

Session-close snapshot. Continues
`2026-05-29-rich-ui-g-phases-handoff.md` (which covered G5/G6/G7).

## TL;DR state

- Branch `main`, tip **`f48edeaa`**, working tree **clean**.
- **PUSHED to `origin/main`** — origin/main == local main. This is the
  big change from prior sessions: everything is no longer "held
  pending approval"; it's on GitHub. **CI + Docs both green** on
  `f48edeaa` (verified via `gh run list`). Docs redeployed to
  Cloudflare Pages.
- **No new release** — last release is still **`v0.21.0`**. The push
  triggered only CI + Docs; Release is tag-gated (`release.yml` runs
  on `v*` tags only). Next minor would be ~`v0.22.0`, on explicit
  approval only (`[[release-approval-gate]]`).
- Tests **4126 / 4126** both arches (X64 + AARCH64), 0 failures.
- All CI gates verified locally before push: 4 integration suites
  (tools 31, tcp-echo 4, http 177, cpu-idle PASS), TLS builds both
  arches, yield-ctrlc, axl-cc-service, run-qemu CPU-spike smoke, and
  the full-tree clang-tidy.

## What shipped this session (after the G5/G6/G7 handoff)

1. **G7 — glyph cache + horizontal subpixel positioning** (`59c9f556`).
   Per-font 256-slot LRU cache in `axl-truetype.c`, keyed
   `(codepoint, px_size quantized to 1/64px, subpixel bin)`; internal,
   no public API change; OOM fallback. Paired with quarter-pixel
   horizontal subpixel rendering (`stbtt_GetCodepointBitmapSubpixel`);
   whole-pixel pens stay bit-identical. Advance/kerning keep the exact
   scale (matches `axl_ttf_measure`).
2. **G14 — analytic AA path rasterizer** (`dae9585f`). Replaced
   `axl_gfx_fill_path`'s 4×4-supersampled even-odd sampler with
   FreeType's **ftgrays** (the reference "FreeType-smooth" rasterizer),
   vendored under `deps/freetype/` in `STANDALONE_` mode (FTL license).
   Needs only `malloc`/`free`/`memset` externally → routed to axl.
   `src/gfx/axl-gfx-rasterize.c` marshals our `AxlGfxVertex` list →
   `FT_Outline` (26.6 fixed point, fed **y-down — no flip**, inside/
   outside is y-flip-invariant for both fill rules), runs ftgrays in
   DIRECT mode, forwards coverage spans to a sink. Even-odd is the
   public fill rule; **non-zero is also supported** (that's what the
   stroker uses). `compat/setjmp.h` stub added (ftgrays `#include`s it,
   never calls it). FTL attribution in `THIRD_PARTY.md`.
3. **G8 — the lean stroker, in 3 sub-steps:**
   - **G8a** (`1a591e15`) width + round joins/caps (offset quads +
     discs, non-zero union).
   - **G8b** (`656804e3`) `AxlGfxStrokeStyle` + `axl_gfx_stroke_path_ex`;
     caps butt/round/square, joins miter(+limit)/round/bevel. Default
     style is **butt + miter** (CSS-standard). Joins fill **only the
     outer convex side** (`s = -sign(cross)`) — the "emit both sides"
     trick in an early draft spilled inner geometry on short sharp
     corners (review H1).
   - **G8c** (`2ad883d4`) dashes — `dashes`/`n_dashes`/`dash_offset` on
     the style; an arc-length pre-pass splits each subpath into on/off
     intervals, strokes each "on" interval as its own capped open
     piece. Odd patterns repeat to even (SVG); degenerate/all-tiny
     patterns stroke solid (review C1: an all-≤1e-9 pattern with
     positive sum infinite-looped — fixed by rejecting max_elem≤1e-9).
   - **Stroker split** (`8fb5d80d`) into its own
     `src/gfx/axl-gfx-stroke.c`, matching Cairo/Skia/Qt/FT. Boundary:
     `axl_gfx_internal_path_verts` (vertex accessor) +
     `axl_gfx_internal_fill_span` (shared solid/gradient span sink,
     promoted from path.c's static `fill_span_`) in
     `axl-gfx-internal.h`.
   - **Divide-by-zero guard** (`f48edeaa`) on `dash_advance_`'s
     `% d->n` — clang-tidy false positive, guarded for CI safety.

## Architecture decisions recorded (docs/AXL-Rich-UI-Plan.md)

- **AD1** — G14 borrows ftgrays standalone (spike-decided; see
  `docs/spikes/2026-05-29-G14-rasterizer.md`). Delete the supersample
  outright (done).
- **AD2** — keep stb_truetype as the font engine + both vector and
  bitmap font paths; shaping is a consumer-layer concern (Cairo model).
- **AD3** — AxlGfx stays in axl-sdk (it maps to Cairo, not GTK; it's
  deeply coupled to AXL core, unlike Cairo↔GLib). Split criteria
  recorded.
- **AD4** — **DECIDED: vendor the *full* FreeType library as a gated
  `AXL_FREETYPE=1` bundle, mbedtls-style** (the mbedtls precedent kills
  the "too big a port" objection). Additive, off by default. Unlocks
  hinting, formats (CFF2/variable/color/WOFF2), SDF, FT_Stroker.
  Includes the licensing/extractability research (FTL is permissive +
  credit clause; only `smooth`/`raster` are `STANDALONE_`, so it's
  whole-library-or-nothing for the rest).
- **AD5** — **two-tier rendering backend.** `axl_gfx_*`/`axl_ttf_*` are
  single public APIs with two build-time backends: `AXL_FREETYPE=0`
  (lean, default — stb + ftgrays-standalone + hand-rolled stroker) vs
  `AXL_FREETYPE=1` (full FreeType). The lean tier is core API and can't
  be gated off — which is *why* G8's stroker is hand-rolled even though
  FT_Stroker exists.

## Driving consumer — SoftBMC

`aximcode/softbmc` (a software BMC, UEFI app; design phase, no code
yet) serves an HTML/JS/CSS dashboard to a *remote* browser; its local
UEFI screen is only a text splash. The Rich UI goal: render a
**dashboard subset on the local GOP screen** via the future A2 HTML
painter + C2 QuickJS. **Important honest scoping** (in the plan): the
blocker for the full remote SPA is **not the JS** (C2/QuickJS is a
full ES2020 engine) — it's the **browser platform APIs** the
interactive libs bind to (xterm/noVNC need Canvas2D; CodeMirror needs
live layout + reflow + contenteditable — categorical gaps vs A2's
paint-once model). Local target = cards/tables/sensor widgets/markdown
/forms; terminal/editor/KVM stay remote or use AGT-native widgets.

## Where the gfx substrate stands

The **lean (`AXL_FREETYPE=0`) 2D substrate is feature-complete** for
table-stakes: fill (analytic ftgrays, even-odd + non-zero), stroke
(width + 3 caps + 3 joins + dashes), text (glyph cache + subpixel,
analytic AA via stb), plus G1–G6 (paths, images, transforms,
gradients, blur/shadows) and AxlMath M1–M10.

## NEXT: start with G11

**Phase G11 — multi-line text + word wrap.** Lean-tier, builds purely
on the existing `axl_ttf_*` primitives (no FreeType needed). The
SoftBMC dashboard text (labels, paragraphs) needs it.

Design (per the plan's G11 entry):
- `axl_ttf_draw_box(ttf, x, y, w, h, utf8, px_size, color, flags)` —
  wrap UTF-8 within the rect `(x,y,w,h)` on whitespace + grapheme
  boundaries; honor hard breaks (`\n`); advance lines by
  `ascent+descent+line_gap` from `axl_ttf_metrics`.
- Optional `axl_ttf_measure_box(...)` for layout-before-draw (returns
  used width/height or line count).
- Horizontal alignment via flags (LEFT/CENTER/RIGHT — the plan already
  blesses these as toolkit-neutral; reuse/extend the existing align
  flag convention if one exists on the draw calls).
- Edge cases: a word wider than `w` (break mid-word or overflow —
  decide + document), vertical clipping to `h`, trailing whitespace,
  empty lines.
- **Test-first** per CLAUDE.md bucket A: pin exact behavior (line
  count for a known string+width, wrap point, `\n` honored, alignment
  shifts pixels). Both arches. Independent review before commit.

After G11: G9 (display list), G10 (path clip), G12 (pattern fill),
G13 (blend modes), and the big one — **Phase G-FT** (the full FreeType
bundle per AD4/AD5: vendor tree + ftoption trim + ftstdlib shim +
FT_Memory/Stream hooks + build gate + hinting/format font backend;
mbedtls-scale, its own focused multi-session effort).

## Forward-carrying rules / gotchas

- **clang-tidy: local is clang-21, CI is clang-18 (ubuntu-latest).**
  clang-21 is much stricter — it flags `valist.Uninitialized` /
  `security.ArrayBound` false-positives in pre-existing files that CI's
  clang-18 does NOT (proven: those files are unchanged since the
  CI-green v0.21.0). When the full-tree lint errors, check whether the
  erroring file *changed since the last release* — if not, it's
  clang-21-local noise, not a CI blocker. Run **per-file (`-n1`)**.
  Your *changed* files must be clang-21-clean (they all are at
  `f48edeaa`).
- **Push ≠ release.** `git push origin main` → CI + Docs only (Docs
  redeploys public Cloudflare Pages site). Release needs a `v*` tag
  (`[[release-approval-gate]]`, RELEASING.md flow).
- **gfx file map now:** `axl-gfx-path.c` (AxlGfxPath + fill +
  rounded-rect), `axl-gfx-stroke.c` (stroker), `axl-gfx-rasterize.c`
  (ftgrays integration), plus gradient/effects/truetype/pixmap/font.
- Non-zero winding is the stroker's fill rule; even-odd is fill_path's.
- Style rules: `[[module-prefix-macros]]`, `[[feedback-tdd-mandatory]]`,
  `[[feedback-code-review-before-commit]]`, `[[feedback-dogfood]]`.

## How to verify locally

```sh
cd ~/projects/aximcode/axl-sdk
make tests ARCH=x64 && ./test/integration/test-axl.sh --arch X64    # 4126/4126
./test/integration/test-axl.sh --arch AARCH64                       # 4126/4126
# per-file clang-tidy (deterministic):
rm -f compile_commands.json && bear -- make tests tools
clang-tidy -p . -quiet src/gfx/axl-gfx-stroke.c   # (and other changed files)
```
