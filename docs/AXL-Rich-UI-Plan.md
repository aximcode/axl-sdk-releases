# axl-sdk — Rich UI exploration plan

**Status:** planning. No commitment to ship. Document captures the
intent, the prerequisite work, and the spike + prototype structure
so the work can be picked up later without re-deriving it.

## Goal

Provide a UEFI-pre-boot UI substrate rich enough to host modern JS
toolkits (React/Preact/Solid/lit-html shape, not just static
widgets). Two engines, intended to compose:

- **Path A** — HTML/CSS rendering (DOM + layout + paint).
- **Path C** — QuickJS runtime exposing AxlGfx / AxlInput / AxlNet /
  AxlFs to JS.

A+C together give JS toolkits a real-ish DOM-shaped target. Path B
(WebAssembly via WAMR / wasm3) is documented as a possible future
but NOT committed to.

### Driving consumer — SoftBMC local on-screen dashboard

The concrete product goal behind this work: give the sibling
**SoftBMC** project (`aximcode/softbmc` — a software BMC that runs as
a UEFI app) a **browser-like view rendered on the local UEFI GOP
screen**, not just the remote web dashboard it serves over HTTP.

Today SoftBMC's management UI is an HTML/JS/CSS single-page app
served to a *remote* browser (assets embedded in the binary as C
arrays); the *local* console is only a text "splash" screen (IP,
ports, password — `Splash.c`). The Rich UI substrate closes that gap:
the **A2 HTML/CSS painter** (Lexbor + AxlGfx) renders SoftBMC's
dashboard markup directly to the framebuffer, and the **C2 QuickJS
runtime** drives its interactivity — so the machine's own screen
shows the same management UI a remote operator sees. This makes
SoftBMC a first-class driving consumer of A2/C2 alongside AGT, and a
real-world validation of the whole "browser engine in UEFI" thesis.

**Realistic scope caveat (to resolve with SoftBMC).** The blocker is
**not "the JS is too heavy"** — C2 (QuickJS) is a full ES2020 engine
(the C1 spike ran async/await, classes, modules), so e.g. `marked`
(Markdown → HTML, pure string→string, no DOM) just works.  The
blocker is the **browser *platform* API surface** the remote SPA's
interactive libraries bind to, which the paint-once A2 + C2 host does
not expose — and some of it is a *categorical* gap, not a
fill-in-more-CSS gap:

- **Canvas2D / WebGL.** `xterm.js` renders via a canvas/WebGL addon
  (it dropped its DOM renderer); `noVNC` blits framebuffer tiles via
  `putImageData`.  A2 paints DOM → framebuffer and has no `<canvas>`
  context (an explicit A2 non-goal).  Hosting them means building
  Canvas2D-on-AxlGfx.
- **Live layout geometry + incremental reflow.** `CodeMirror`
  continuously queries `getBoundingClientRect` / `getComputedStyle` /
  scroll metrics and edits via `contenteditable` + selection.  A2 is
  **paint-once, no reflow** — a live, queryable, incrementally-
  reflowing layout engine is a different engine, not a bigger subset.
- **Event / timing platform.** `requestAnimationFrame`, `ResizeObserver`
  / `MutationObserver`, clipboard, IME composition — assumed by any
  interactive editor/terminal.

And even where it is *technically* reachable, it is the **wrong
trade**: a terminal is a monospace cell grid AxlGfx draws natively in
a fraction of the code it takes to back `xterm.js` with Canvas2D; a
BMC config form does not need CodeMirror; and `noVNC` locally is a
category error (you do not VNC into your own screen).

So the pragmatic target is a **purpose-built local dashboard subset**
(status cards, inventory tables, health/sensor widgets, Markdown via
`marked`, simple forms) sharing SoftBMC's embedded-asset pipeline —
the parts that are genuinely just HTML/CSS + light JS.  The
canvas-/reflow-bound panes (terminal, code editor, KVM) stay
**remote-only or use AGT-native widgets locally**, never
xterm/CodeMirror/noVNC rendered through A2.

## Why this structure

A modern browser engine (Chromium / WebKit / Servo / Gecko / LibWeb)
is not portable to pre-boot UEFI without effectively writing a small
OS — see prior analysis. The realistic substrate is a composition of
narrow, self-contained C libraries that already run on freestanding
targets:

- HTML5 parse: Lexbor (MIT, C99) or NetSurf's libhubbub (BSD, plain C).
- CSS parse + cascade: NetSurf's libcss (BSD, plain C).
- DOM: libdom (BSD) or Lexbor's own DOM tree.
- JS runtime: QuickJS (MIT, portable C, interpreter — no JIT, which
  is a feature in UEFI where W^X page perms aren't available).

None of these depend on POSIX threads, dynamic linking, sockets-as-fds,
or GPU drivers. They DO need a 2D rasterizer with vector text + image
decode — which is the AxlGfx prerequisite below.

## Architecture decisions (AxlGfx)

Standing decisions about the *shape* of the rendering stack, kept
here so they aren't re-argued each session.  AxlGfx is pre-1.0 with
**zero downstream consumers** (it shipped in the v0.20.0/v0.21.0
tags but nothing consumes it yet — *released ≠ consumed*), so per
[[feedback-change-apis-freely]] we can replace implementations
**and** reshape APIs with no deprecation window.  That freedom
applies to inferior *implementations*, not to *capabilities* — the
two pull in opposite directions below.

**AD1 — Replace the path rasterizer outright (no compat mode);
borrow FreeType `ftgrays.c` standalone.**  `axl_gfx_fill_path`'s
4×4-supersampled even-odd rasterizer is an implementation, not an
API.  G14 (analytic coverage + non-zero winding) deletes it outright
— no `*_LEGACY` mode, no pixel-compat shim — because nothing depends
on its exact output.  **DECIDED (spike `spike/g14-rasterizer`,
2026-05-29 — see `docs/spikes/2026-05-29-G14-rasterizer.md`): borrow
FreeType `ftgrays.c` in `STANDALONE_` mode.**  The spike compiled it
with our exact freestanding flags into a clean 14KB object whose
*entire* external surface is `malloc`/`free`/`memset` (→ axl) — no
setjmp (the feared friction is vestigial), no libm, no qsort, no
stdio.  Pure analytic exact-area coverage (the reference "FreeType-
smooth" rasterizer) with a direct span callback (`FT_Span{x,len,
coverage}`) that maps straight onto our blit/gradient path.
**Rejected:** `nanosvgrast.h` (SVG-coupled — clean use = a fork that
violates this AD, and only 5× supersampled, not analytic) and a
hand-roll (viable fallback, but "we wrote our own scanline
rasterizer" is not a differentiator when a 3-symbol reference borrow
exists).  Do NOT fork stb_truetype's internal rasterizer out for
general paths either, even though it is why our *text* is already
analytic.  Cost accepted: vendor `ftgrays.c` + `ftimage.h` +
`ftgrays.h` + a ~6-line shim, marshal our edge list → `FT_Outline`,
and add FreeType's FTL license (attribution-only — see AD4).

**AD2 — Keep the font stack as-is: stb_truetype + both font paths.**
There is no better engine *for our constraints*.  stb_truetype's
only deficits vs FreeType are bytecode hinting and input-trust
(hardened); FreeType is better in absolute quality but strictly
worse for freestanding UEFI (large, config-heavy) — switching trades
our defining zero-dep/tiny advantage for quality no consumer has
asked for.  No lightweight text-shaping option exists (no
"stb_harfbuzz"; HarfBuzz is heavy C++), so **shaping stays a
consumer/toolkit-layer concern — exactly Cairo's model** (toy text
in the lib, Pango/HarfBuzz above it).  Keep BOTH the vector
(`axl-truetype.h`) and bitmap (`axl-font.h`) paths: bitmap is not
redundant with `axl_ttf_default()` — it serves zero-rasterization
fixed-cell console text, Unifont CJK/box-drawing coverage the DejaVu
subset lacks, and a no-stb-linked footprint.  Do not drop or swap
the font engine.

**AD3 — AxlGfx stays in axl-sdk (not its own repo, for now).**  The
intuitive "GTK : GLib" split is the wrong analogy: AxlGfx maps to
**Cairo**, not GTK; AGT (separate repo) is the GTK layer, so the
GTK:GLib split *already exists*.  The real question is a three-way
GNOME-style split (core / gfx / widgets), and the decisive fact is
that **Cairo is independent of GLib — which is why their split is
clean — whereas AxlGfx depends deeply on AXL core** (AxlMem,
AxlMath, the backend/GOP/MP, AxlGfxBuffer, UEFI types, the build,
axl-cc packaging, docs).  A split would be *messier* than GTK:GLib,
not similar.  The usual driver — binary weight — is already
neutralized by `--gc-sections` (non-gfx consumers carry no gfx
code).  **Revisit a split only when all three hold:** a real
consumer exists with stabilized cross-repo coupling; gfx reaches its
own 1.0 wanting independent cadence; monorepo CI/release friction is
actually felt.  Cheap insurance meanwhile: keep gfx a cleanly
isolated module behind the `axl-gfx.h` umbrella (already done) so a
future lift is mechanical.  (Note: a toolkit "owning rendering" à la
GTK4/GSK is a scene/display-list layer *on top of* drawing
primitives — that's our G9, built on AxlGfx, not a reason to fold
gfx into AGT.)

**AD4 — stb_truetype is the default font engine; FreeType is a
future opt-in backend, not a replacement.**  Extends AD2.  FreeType
*can* run freestanding (memory/stream hooks + a custom `ftstdlib.h`
re-pointing its libc calls at axl, the same macro-routing we do for
stb), but it is a days-to-weeks port with an ongoing config/version-
bump tax — vs the afternoon stb took — and a far larger surface.
Crucially, **FreeType is a rasterizer, not a shaper**: it does *not*
close our biggest text gap (complex-script shaping = HarfBuzz,
orthogonal to this choice).  What it *does* buy is **bytecode
hinting** (sharper small-size text on low-DPI panels — the one
potentially load-bearing win for a firmware UI), plus format
coverage (CFF2, variable, color/emoji, WOFF2) and LCD filtering that
firmware UI doesn't need.  stb_truetype is unhinted but tiny,
zero-config, maximally-permissive (PD/MIT vs FTL-or-GPLv2), and
adequate for Western UI labels at moderate sizes.  **DECIDED
(2026-05-29): keep stb_truetype as the lean default, AND vendor the
*full* FreeType library as a gated `AXL_FREETYPE=1` bundle — exactly
like `deps/mbedtls` / `AXL_TLS=1`.**  The mbedtls precedent settles
the old "days-to-weeks port + ongoing version tax" objection: we have
*already* shipped a full-library vendor (deps/mbedtls) with a custom
config header trimming modules, platform shims routing libc → axl,
and a build-flag gate.  FreeType is the same shape of work, not a
harder one.  The bundle is **additive and off by default** (footprint
stays opt-in) and sits behind the unchanged `axl_ttf_*` API — never a
public-surface fork.  It unlocks, when enabled: **bytecode hinting**
(sharper small-size text on low-DPI panels), **format coverage**
(CFF2, variable, color/emoji, WOFF2), LCD filtering, SDF, and
**FT_Stroker** (full caps/joins/miter/dashes for the geometry layer).
The motivating consumers / "why now": (1) the SoftBMC local dashboard
+ A2 painter wanting faithful text; (2) small-size legibility on
low-DPI panels; (3) color / variable fonts.  Complex-script shaping
is **still not** in scope (that's HarfBuzz, a separate future bundle).

*Codebase + licensing research (2026-05-29, verified against the
FreeType master tree):*

- **Licensing is not a blocker.**  FreeType is dual-licensed FTL
  **or** GPLv2+; we take the **FTL** — BSD-style with one extra
  obligation: a **credit clause** (acknowledge FreeType in our
  product documentation) on top of reproducing the license text.  No
  copyleft, no source disclosure.  Per-component licenses are all
  permissive and compatible (BDF/PCF + `base/fthash.c` = X11-style;
  `gzip` = zlib; `autofit/ft-hb-*` = Old MIT; MD5 = public domain).
  FTL is GPLv2-*incompatible* (the credit clause) but that is exactly
  why FreeType offers the GPLv2 alternate — irrelevant to us, we are
  not GPL.  Slots into our existing third-party-attribution process
  (`third_party/.../LICENSE` + `THIRD_PARTY.md`, as for DejaVu).
- **But only the rasterizers are cheaply extractable.**  Of the 28
  `src/` modules, **only `smooth/ftgrays.c` (AA) and `raster/
  ftraster.c` (mono) declare `STANDALONE_`** — usable without the FT
  base library.  Everything else — the **bytecode hinting VM**
  (`truetype/ttinterp.c`), `sfnt`, the font drivers, `autofit`, the
  `cache` (FTC), even the `sdf` rasterizer — `#include
  <freetype/internal/...>` and are welded to the FT object model
  (`FT_Library`/`FT_Memory`/`FT_Stream`/`FT_Module`).  **Verified:**
  `ttinterp.c` and `ftsdf.c` both pull `internal/ftobjs.h`.
- **Consequence: no cherry-pick middle path — which is exactly why we
  go full-bundle.**  Of the geometry pieces, only `ftgrays`/`ftraster`
  are `STANDALONE_`; the stroker (`ftstroke`) already pulls
  `FT_GlyphLoader` + trig + the memory-macro layer, and hinting is
  welded to the TrueType driver + base.  Rather than vendor an
  ever-growing pile of partial subsets (ftgrays-standalone, then
  ftstroke-ish, then…), the `AXL_FREETYPE=1` bundle takes the *whole*
  library once and is done — same call we made for mbedtls.

**AD5 — AxlGfx is a two-tier rendering backend.**  Concretely
resolves how the AD4 bundle coexists with the already-shipped ungated
gfx (G1–G14).  `axl_gfx_*` and `axl_ttf_*` are **single public APIs
with two backends selected at build time**, mirroring how `axl_tls_*`
is present-but-inert without `AXL_TLS=1`:

| | `AXL_FREETYPE=0` (default, lean) | `AXL_FREETYPE=1` (full bundle) |
|---|---|---|
| Fonts | stb_truetype (unhinted) | FreeType — hinting, CFF2, variable, color, WOFF2 |
| Path fill | ftgrays-standalone (G14, shipped) | FreeType raster (or keep standalone) |
| Stroke (G8) | **hand-rolled lean stroker** (width + miter/round/bevel joins + butt/round/square caps; dashes later) | FT_Stroker (adds dashes, hardened edge cases) |

The lean tier is **self-contained and never requires the flag** — it
is core library API, not an optional feature, so it cannot be gated
off (unlike TLS, which genuinely is optional).  The bundle is purely
additive: enabling it upgrades fonts (and optionally geometry) without
changing any public signature or un-shipping G1–G14.  This is why G8's
default stroker is hand-rolled even though FT_Stroker exists — the
lean tier needs strokes with the flag off.

## Prerequisite — AxlGfx 2D upgrade

> **Status (2026-05-29): G1, G2, G3, Gx all SHIPPED** (v0.20.0).
> The phase write-ups below are kept as the original design record;
> they describe work that is now done. Live phase tracking lives in
> `docs/ROADMAP.md`.

**Co-consumer: AGT** (sibling C++ widget toolkit). AGT Phase 2.6
(AgtDrawContext + RecordingDrawContext) and Phase 2.7a (label/
button/frame chain) are already shipped; the G1/G2/G3 primitives
below are additive consumers for AGT, not blockers. AGT consumes
them on its own schedule via additive virtuals on AgtDrawContext.
AGT input on the API shape is captured in
[agt/docs/handoffs/2026-05-28-agt-input-axl-gfx-2d-upgrade.md](../../agt/docs/handoffs/2026-05-28-agt-input-axl-gfx-2d-upgrade.md)
(2026-05-28) and reflected in the G1/G2/G3 surfaces below.

### Consumer audit — AGT substrate usage (2026-05-31)

Audited AGT's `src/render` + `src/core` against the current substrate.
**Verdict: AGT is on the fast paths and no substrate gaps were found.**

What AGT does well (already optimal):
- **Persistent back-buffer** (`AgtWindow::back_buf_`, allocated once at
  window init), not a per-frame allocation.
- **Incremental present** — `present_mode()` returns `PRESENT_FULL`
  (structure change), `PRESENT_DAMAGE`, or `PRESENT_NONE`; the damage
  path computes a widget-level bbox (`AxlGfxClip`) and pushes only it via
  `axl_gfx_buffer_present_rect`. (It tracks damage itself, so it
  correctly does **not** also use axl-gfx's per-buffer
  `add_damage`/`present_damage` — that would be redundant.)
- **No reimplementation** of substrate — zero local pixel/blend/color
  loops; everything routes through `axl-gfx` via `AgtDrawContext`.
- **Transform-aware draws** (`fill_path`, `axl_ttf_draw_transform`,
  `push_clip_rect_transformed`) for the render walk; `fill_rect_i` for
  the axis-aligned common case (path only for rotated/sheared quads).
- **SIMD for free** — AGT correctly never calls `axl_cpu_*`; the
  blend/blur kernels auto-dispatch (and auto-enable AVX) internally.

Inherited from this session's substrate work, **at zero cost to AGT**:
the G17 **NT framebuffer streaming stores** speed every present; the
**SIMD blend/blur** speed AGT's drop-shadows (`axl_gfx_draw_shadow` ×5,
`axl_gfx_buffer_blur` ×4) and translucent fills on capable hardware.

Opt-in levers AGT has **not** adopted (opportunities, not gaps):
- **Gamma-correct compositing (G15)** — AGT is text-heavy and uses
  shadows; `axl_gfx_set_gamma_correct(true)` at app init would give
  cleaner AA text edges (no dark fringe) and correct shadow/translucent
  brightness. A deliberate *visual* change (it shifts the look), so it's
  an evaluate-and-decide, not a free win.
- **Gradients (G5) / pattern fill (G12)** — unused; AGT is flat-fill +
  shadow today. Available if the aesthetic wants them.
- **Clip the render walk to the damage bbox** — AGT re-renders the whole
  widget tree per dirty frame then presents only the damage region; a
  `push_clip` to the damage bbox around the walk would skip off-damage
  widgets cheaply. This is an AGT-internal optimization using *existing*
  substrate (it matches the "incremental-render TODO" in AGT's SCRATCH),
  not a substrate change.

**Design constraint: G1/G2/G3 are toolkit-neutral 2D primitives,
NOT HTML/CSS-shaped.** AGT and Path A2's painter both confirmed
strong agreement on this. Avoid CSS-specific idioms (em units,
line-box metrics, baseline-vs-top text positioning, named colors
past the basic 8) leaking into the axl-gfx API; consumers translate
at their own layer. Horizontal alignment hints (LEFT/CENTER/RIGHT)
on text-draw calls are general UI and OK to expose as opt-in flags.

Today's [axl-gfx.h](../include/axl/axl-gfx.h) gives framebuffer +
double-buffer + clip + rect/line/polyline + blit + alpha blend +
bitmap fonts. That is enough for a widget toolkit. It is NOT enough
to back HTML/CSS layout + paint, which assume:

1. **Vector text** — variable font sizes from one TTF, kerning,
   per-glyph metrics, ascent/descent for line-box layout.
2. **Image decode** — PNG and JPEG at minimum (WebP/AVIF nice to
   have); CSS `background-image` and `<img>` are non-optional.
3. **Vector 2D** — paths, curves, gradients, anti-aliasing,
   transforms. CSS `border-radius`, `box-shadow`, transforms.

### Phase G1 — vector text (stb_truetype)

- Add `<axl/axl-truetype.h>` (or extend axl-font.h) wrapping
  stb_truetype.h (single-header, public domain).
- API (agreed surface — toolkit-neutral, AGT-confirmed 2026-05-28):

```c
AxlTtf  *axl_ttf_load(const uint8_t *bytes, size_t len);
void     axl_ttf_free(AxlTtf *ttf);

uint32_t axl_ttf_measure(AxlTtf *ttf, const char *utf8, float px_size);
uint32_t axl_ttf_measure_prefix(AxlTtf *ttf, const char *utf8,
                                size_t prefix_bytes, float px_size);

int      axl_ttf_draw(AxlTtf *ttf, int x, int y, const char *utf8,
                      float px_size, AxlGfxPixel color);

void     axl_ttf_metrics(AxlTtf *ttf, float px_size,
                         float *ascent, float *descent, float *line_gap);
```

- `px_size` is `float` (stb_truetype native; locking out fractional
  sizes early would block hi-DPI / zoom later). Integer-valued
  calls work fine; the type just doesn't preclude fractional.
- `axl_ttf_measure_prefix` enables future AgtEditField cursor
  positioning via byte-position binary search — cheaper than
  emitting per-glyph advance arrays. AGT-requested; A2 also wins
  (CSS text-cursor / selection).
- UTF-8 in. Invalid sequences → U+FFFD (same convention as
  `axl_gfx_draw_text`).
- Ship one default TTF embedded via xxd in the library (a permissive
  font — DejaVu Sans or similar). Consumers can load their own.
- Test bucket A (STRICT test-first). Render-to-buffer assertions
  against known-good golden buffers; exact-pixel comparison for the
  rasterizer output is too brittle, so assert on dimensions +
  metrics + a cheap perceptual hash (or coverage threshold) of the
  output region. Plus exact-value assertions on `measure`,
  `measure_prefix`, `metrics` returns.
- Skipped (confirmed not needed): per-glyph advance arrays,
  retained `AxlGfxTextRun`, explicit kerning hooks, multi-weight
  from one load.
- Effort: ~1 week.

### Phase G2 — image decode (stb_image)

- Add `<axl/axl-pixmap.h>` wrapping stb_image.h (public domain).
  Module name is `AxlPixmap`, NOT `AxlImage` — `<axl/axl-image.h>`
  is already taken by the UEFI executable-image lifecycle
  (`axl_image_load`/`axl_image_start`/`axl_image_unload`). The
  agreed-with-AGT surface below uses the resolved `axl_pixmap_*`
  prefix; AGT will rename its consumer wrapper accordingly.
- API (agreed surface — AGT-confirmed 2026-05-28; prefix changed
  post-handoff to avoid the axl-image.h collision):

```c
int            axl_pixmap_info(const uint8_t *bytes, size_t len,
                               uint32_t *out_w, uint32_t *out_h);
AxlGfxBuffer  *axl_pixmap_decode(const uint8_t *bytes, size_t len);
```

- `axl_pixmap_info` wraps `stbi_info_from_memory` — lets layout
  consumers (AgtImage, CSS `<img>` intrinsic sizing) size their
  parent container BEFORE committing to allocate the full pixel
  buffer. Tiny pass-through, real win for layout-driven sizing.
- Supports PNG/JPG/GIF/BMP (stb_image defaults).
- Verify against the existing AxlGfxBuffer pixel layout — stb_image
  returns RGBA8888; AxlGfxPixel is BGRA. One swizzle pass on decode.
- Test bucket A; verify against tiny fixture PNGs/JPGs embedded in
  the test binary.
- Pre-validate the input header before handing to stb_image (per
  `feedback_validate_untrusted_bytes_before_stb`) — stb_image's
  documented OOB behavior on malformed input crashes AArch64
  silently passes x64. Check magic bytes + format-specific size
  fields fit in buffer BEFORE `stbi_load_from_memory`.
- Skipped (confirmed not needed): decode-into-existing-buffer
  (consumers cache at widget level), incremental/streaming decode
  (wrong shape for pre-boot UEFI).
- Effort: 2-3 days.

### Phase G3 — path rasterizer + AA

**API shape resolved (AGT-confirmed 2026-05-28):** retained
`AxlGfxPath` object is the primary API + one immediate-mode
convenience helper for the rounded-rect case that will dominate
widget code.

Why retained primary:
- RecordingDrawContext captures the path pointer, not the points
  — trace stays cheap. Immediate-mode `fill_path(points, count)`
  would force the recorder to deep-copy points into each trace
  entry (allocation + UAF risk).
- Widgets build their button / icon path once at construct time
  and fill on every redraw — caching is free with retained shape.
- CSS `border-radius` paths are deterministic per element — same
  built-once-replay-many shape suits A2's painter.

Agreed surface:

```c
typedef struct AxlGfxPath AxlGfxPath;

AxlGfxPath  *axl_gfx_path_new(void);
void         axl_gfx_path_free(AxlGfxPath *p);
void         axl_gfx_path_reset(AxlGfxPath *p);  /* reuse w/o realloc */

void  axl_gfx_path_move_to (AxlGfxPath *p, float x, float y);
void  axl_gfx_path_line_to (AxlGfxPath *p, float x, float y);
void  axl_gfx_path_curve_to(AxlGfxPath *p,
                            float c1x, float c1y,
                            float c2x, float c2y,
                            float x,   float y);
void  axl_gfx_path_arc     (AxlGfxPath *p, float cx, float cy, float r,
                            float start_rad, float end_rad);
void  axl_gfx_path_close   (AxlGfxPath *p);

int   axl_gfx_fill_path   (const AxlGfxPath *p, AxlGfxPixel color);
int   axl_gfx_stroke_path (const AxlGfxPath *p, AxlGfxPixel color, float w);

/* Immediate-mode convenience — internally builds + fills + frees a
 * transient path. The case that'll dominate widget rendering. */
int   axl_gfx_fill_rounded_rect(int x, int y, int w, int h,
                                float radius, AxlGfxPixel color);
```

**Implementation choice (G3a vs G3b) still deferred to Spike A2.**
The API above is implementation-agnostic; both options can back it:

- **Option G3a: hand-rolled scanline rasterizer** (~500 LOC C). Just
  enough for filled paths + strokes + AA. Modeled on Skia's
  SkScan_AntiPath or stb's vendor-included path code. No external
  dep.
- **Option G3b: Blend2D vendored** (C++, zlib, optional SIMD).
  Production-grade. Larger drop-in (~50k LOC), C++ compiler required
  but axl-cc supports it already (see [[v0.19.2_landed]]). Real CSS-
  fidelity gradients and transforms.

G3a unblocks Spike A's renderer cheaply; G3b becomes the answer if
fidelity matters. Defer this choice until Spike A2 has concrete
fidelity targets.

- Effort G3a: ~1 week. G3b: 2-3 days integration + ongoing carry.

### Phase Gx — opportunistic signed-coord rect (AGT ask)

Current `axl_gfx_fill_rect(uint32_t x, y, w, h, color)` forces AGT
to clamp negative coordinates in `AgtDrawContext::fill_rect` for
partially-off-screen widget rendering. Add a signed-coord variant
matching the existing `axl_gfx_draw_line` signed-coord shape:

```c
int axl_gfx_fill_rect_i(int32_t x, int32_t y, int32_t w, int32_t h,
                        AxlGfxPixel color);
```

Negative origins are valid (clipped against active clip + screen
bounds inside the function). Trivial add — internally clamps and
forwards to `axl_gfx_fill_rect` or directly to the per-pixel
write loop. Lets AGT drop the workaround.

Effort: ~1 hour. Land whenever; not gated on G1/G2/G3.

### Prerequisite ordering vs. spikes

G1+G2+G3 are NOT prerequisites for the **viability spikes** (A1, C1
below) — those answer pure "does the engine compile and run in UEFI"
questions with no rendering. G1+G2+G3 ARE prerequisites for the
**prototype phases** (A2, C2) where actual pixels matter. AGT also
consumes G1+G2+G3 on its own schedule, independent of the spikes.
Sequence:

1. **Gx** (`axl_gfx_fill_rect_i`) — trivial opportunistic add, land
   whenever.
2. **G1** (vector text + `measure_prefix`) — start now per the
   plan; AGT has no blockers and adopts via additive
   `AgtDrawContext::draw_text_ttf` virtual when G1 lands.
3. **G2** (image decode + `image_info`) — parallel-safe with G1.
4. Spike A1, Spike C1 — cheap kill-it-fast viability (1-2h each).
   Independent of G1/G2 progress.
5. If A1/C1 both green: prototype A2, C2 in parallel.
6. **G3** (paths) — defer until A2's fidelity target demands it.
   API shape already resolved with AGT; only the implementation
   choice (G3a hand-rolled vs G3b Blend2D) waits on A2.

## Follow-up enhancements — full 2D library + AxlMath

G1+G2+G3+Gx ship the rendering substrate the spikes need.  These
phases extend it into a full 2D drawing library, and round out
`<axl/axl-math.h>` (lifted into a shared module alongside G3 at
commit `8b98f3c`) with the primitives that GFX follow-ups depend
on.

**Demand-driven, NOT prerequisites for A1/C1/A2/C2.**  Sequence
each by waiting for a real consumer (AGT widget, A2 painter, A3
DOM binding, downstream tool) to need it.  Listed roughly by
descending leverage.

### AxlGfx follow-ups (Phase G4+)

> **Status (2026-05-29): G4–G7, G14, and all of G8 SHIPPED.** G4
> (v0.20.0); G5/G6/G7 post-v0.21.0 (G7 = `59c9f556`, glyph cache +
> horizontal subpixel positioning). **G14** (analytic ftgrays
> rasterizer, `dae9585f`) shipped before G8 per AD1. **G8a** (width +
> round joins/caps, `1a591e15`), **G8b** (butt/square caps +
> miter/bevel joins + `AxlGfxStrokeStyle`, `656804e3`), and **G8c**
> (dashes, `2ad883d4`) all shipped — **the lean-tier stroker is
> COMPLETE**, in its own `src/gfx/axl-gfx-stroke.c` (`8fb5d80d`,
> matching Cairo/Skia/Qt/FT). **G13 (blend modes) is also SHIPPED
> (`9e05226b`).** **G10 (arbitrary-path clip) is also SHIPPED**
> (`axl_gfx_push_clip_path`). Genuinely remaining: **G12** (pattern
> fill), **G15–G19**, plus **Phase G-FT** (only `ftgrays` is vendored
> for G14; the full FreeType bundle is not). Entries below are kept as
> the design record; see each phase's SHIPPED tag.
>
> **Update (2026-05-31): transform substrate consolidated + made
> projective; tree containers added — all targeting v0.22.0.**
> - **One transform type.** `AxlMat3` (AxlMath) and `AxlGfxAffine` (gfx)
>   are merged into a single `AxlTransform` (AxlMath, 3×3 double,
>   projective-capable, **cairo a-first** multiply). The gfx CTM and the
>   per-call matrix toolkits hand in are now the same type. See
>   `docs/AXL-Transform-Design.md`.
> - **Perspective-correct rendering.** The transform-aware primitives are
>   no longer affine-only: `axl_gfx_blit_transform` (per-pixel inverse
>   homography), `axl_ttf_draw_transform` (glyph curves flattened in local
>   space then projected), and the new `axl_gfx_push_clip_rect_transformed`
>   all handle a projective `AxlTransform`. New math: `axl_transform_`
>   `perspective` / `quad_to_quad` / `map_rect` / `map_quad` / `classify`
>   (+ `is_identity`/`is_axis_aligned`/`is_affine`). Affine stays an exact
>   fast path. This is the substrate for CSS 3D-ish transforms and warped
>   widget effects.
> - **Tree containers (AxlData).** `AxlNTree` (GLib GNode n-ary tree,
>   public node fields) — the model for AgtTreeView and a future HTML DOM
>   tree — and `AxlTree` (GLib GTree, AVL sorted map), both with
>   callback-free pull iterators. See `src/data/README.md`.

Phase G4 *(SHIPPED)* — **affine transforms + transform stack**.  Public:
`axl_gfx_translate` / `axl_gfx_scale` / `axl_gfx_rotate` /
`axl_gfx_skew` / `axl_gfx_push_transform` / `axl_gfx_pop_transform`
/ `axl_gfx_get_transform`.  Substrate for CSS `transform`, AGT
button hover-scale, animation rotation.  Built on M3 (Vec2 /
`AxlTransform`); the CTM is now an `AxlTransform`, and a projective CTM +
the perspective-correct primitives (see the 2026-05-31 update above) are
the extension of this phase.

Phase G5 *(SHIPPED)* — **gradients (linear + radial)**.  `AxlGfxGradient`
opaque object with `add_stop(t, color)` builder; new `_gradient`
variants of fill_rect / fill_path / fill_rounded_rect.  Table
stakes for any UI built after ~2005 (button backgrounds, hero
panels, CSS `linear-gradient` / `radial-gradient`).  ~1 week.
Quality wins from **M2** (`axl_pow` for sRGB↔linear blending
mid-gradient) but doesn't strictly require it.

Phase G6 *(SHIPPED)* — **Gaussian blur + drop shadows**.  Stack-blur kernel
on `AxlGfxBuffer` (~150 LOC, O(N) per pixel regardless of
radius).  Shadows compose: render to back-buffer → blur → blit at
offset.  Enables CSS `box-shadow`, `text-shadow`, `filter: blur`.
~1 week.

Phase G7 *(SHIPPED — commit `59c9f556`)* — **glyph cache** keyed on
`(font, codepoint, px_size quantized, subpixel bin)` → bitmap.
`axl_ttf_draw` re-rasterized every glyph per call; real text-heavy
UIs hit this wall fast.  Per-font 256-slot LRU eviction, hidden
behind axl-truetype.c (no public API change).  Shipped paired with
**horizontal quarter-pixel subpixel positioning** (the subpixel bin
in the key does real work — whole-pixel pens stay bit-identical to
the old integer-snapped path).

Phase G8 *(SHIPPED — lean tier complete)* — **stroke styling**.
Width > 1, caps (butt / round / square), joins (miter+limit / round /
bevel), dashed lines.  **Per AD5, two-tier:** the **lean default
stroker is hand-rolled** (offset geometry → multi-contour union →
ftgrays *non-zero* fill; this is why G14 added non-zero) so
`axl_gfx_stroke_path` honors width with the flag off; **FT_Stroker
backs it when `AXL_FREETYPE=1`** (hardened cusp/degenerate handling).
Dogfoods AxlMath for join/cap trig.  Lives in `src/gfx/axl-gfx
-stroke.c` (split out at `8fb5d80d`).  Sub-steps: **G8a** *(SHIPPED
`1a591e15`)* width + round joins/caps; **G8b** *(SHIPPED `656804e3`)*
miter/bevel joins + butt/square caps + the `AxlGfxStrokeStyle` API
(`axl_gfx_stroke_path_ex`); **G8c** *(SHIPPED `2ad883d4`)* dashes
(arc-length pre-pass → per-dash open pieces).  Lean tier done.

Phase G-FT — **vendor the full FreeType bundle** (`deps/freetype`,
gated `AXL_FREETYPE=1`, mbedtls-style — AD4/AD5).  The major
follow-up effort: vendor the FreeType tree, a custom `ftoption.h` /
`ftmodule.h` trimming to the modules we want (sfnt + truetype +
smooth + autofit + sdf + stroker; drop bdf/pcf/pfr/type1/cff-extras/
gzip/bzip2/woff as needed), an `ftstdlib.h` shim routing libc → axl,
`FT_Memory` / `FT_Stream` hooks over our byte buffers, the Makefile
gate + install/release plumbing (mirroring mbedtls), and the
`axl_ttf_*` FreeType backend (hinting / formats) behind the unchanged
API.  Replaces the AD4 "future opt-in" with committed work.  Sequence
after the lean G8 (the lean tier must stand on its own first).
~mbedtls-scale (multi-session).

Phase G9 *(SHIPPED — slice 1)* — **first-class display list /
scene graph**.  `<axl/axl-gfx-display-list.h>`: an `AxlGfxDisplayList`
records draw ops via `axl_gfx_dl_*` appenders and replays them with
`axl_gfx_display_list_replay` (invoking the immediate-mode
`axl_gfx_*`).  Explicit-recorder model (decoupled tagged-op buffer,
no global-state intrusion); a public `AxlGfxOp` tagged union +
`axl_gfx_display_list_count`/`_op_at` give the introspection that
supersedes AGT's `RecordingDrawContext`.  Ownership: copies transient
data it's handed (polyline points, blit pixels, text strings, dash
arrays) and borrows handle-typed objects (`AxlGfxPath`, `AxlFont`,
`AxlTtf`) per the path header's documented recording-fixture design.
Slice 1 covers the AGT `AgtDrawContext` op set: fill_rect(_i),
draw_line, draw_rect, draw_polyline, blit, clear, push/pop_clip,
fill_path, stroke_path, fill_rounded_rect, draw_text, draw_text_ttf.
Ops live in an AxlArray (value mode); replay is byte-identical to
immediate mode (proven).  **Follow-up (slice 2):** gradient fills
(rect/path/rounded-rect), the transform-stack ops
(translate/scale/rotate/push/pop_transform), and an optional textual
`dump` serialization.  Pairs with G4 (recorded transforms will
compose cleanly).

Phase G10 *(SHIPPED)* — **path-based clip**.
`axl_gfx_push_clip_path(const AxlGfxPath *)` extends the clip stack to
arbitrary shapes (concave / self-intersecting / multi-contour) by
rasterizing the path (even-odd) to an 8-bit coverage mask over its
device bbox, intersected with the parent clip; every primitive honors it
on both buffer and screen targets.  Combined with G4 transforms =
arbitrary-shape masking; the foundation for CSS `clip-path`.  Hard-edged
(>= 50% coverage), matching the quad clip.

Phase G11 *(SHIPPED)* — **multi-line text + word wrap**.
`axl_ttf_draw_box(ttf, x, y, w, h, utf8, px, color, flags)` wraps
UTF-8 within the rect: greedy whitespace word-wrap, hard `\n`
breaks (a trailing `\n` yields a trailing blank line; `\r`/`\t`
are whitespace), baselines advancing by the `axl_ttf_metrics`
line-height.  Horizontal alignment via `AXL_TTF_ALIGN_{LEFT,
CENTER,RIGHT}` flags (`_MASK` reserves the remaining bits for
future vertical-align / break-mode flags).  Over-wide words are
**not split** — they take their own line and overflow, clipped to
the box (which is pushed onto the clip stack for the draw).
Companion `axl_ttf_measure_box(ttf, w, utf8, px, &width, &height,
&lines)` runs the identical layout without drawing for size-
before-draw and reports the true (possibly `> w`) content width so
callers can detect overflow.  Breaks fall only on whitespace +
codepoint boundaries; full UAX-29 grapheme segmentation stays a
consumer-layer / FreeType concern.  Built purely on the existing
`measure_utf8_prefix` + `ttf_draw_run` primitives (the latter
extracted from `axl_ttf_draw` so both share one length-bounded
render path).  All in `src/gfx/axl-truetype.c`.

Phase G12 *(SHIPPED)* — **pattern fill / tile blit**.
`axl_gfx_fill_pattern(x, y, w, h, AxlGfxBuffer *pattern, AxlGfxRepeat
repeat)` tiles a pattern buffer over a (signed-origin) rect, anchored at
the top-left.  `AxlGfxRepeat` = `BOTH` / `X` / `Y` / `NONE` (CSS
`background-repeat`).  Honors the clip stack, draw target, active blend
mode, and per-texel alpha (transparent texels show through) by routing
each texel through `put_pixel`.  Substrate for CSS
`repeating-linear-gradient` (pre-render one period, then repeat),
textured backgrounds, and nine-slice button art.

Phase G13 *(SHIPPED — `9e05226b`)* — **blend modes** beyond
source-over.  `axl_gfx_set/get/reset_blend_mode` + the pure
`axl_gfx_blend_ex(dst, src, mode)`, with `AXL_GFX_BLEND_OVER` /
`_MULTIPLY` / `_SCREEN` / `_OVERLAY` / `_DARKEN` / `_LIGHTEN` / `_ADD`.
Honored by every buffer-target compositing primitive (fills, gradients,
lines, text, `axl_gfx_blit_transform`).  Enables CSS `mix-blend-mode`.

### AxlGfx rendering-quality follow-ups (Phase G14+)

These close the *quality* gap with the analytic software
rasterizers (Cairo/pixman, Blend2D, AGG, Skia-CPU) rather than
adding new drawing primitives.  **Decision (confirmed): G14 lands
before G8–G13** despite its later number — it is the
highest-leverage item in the entire G-series, and every fill,
stroke (G8), and clip (G10) inherits its edge quality, so doing
strokes or clips first would mean redoing their AA story after.
Build order for the remaining G-work is therefore: **G14 → G8 →
G11 → G9 → G10 → (G12, G13 as demanded)**, with G15/G16 slotted by
the quality bar a given consumer needs.  (G14, G8, G11, and G9
slice 1 are shipped; G11 came next after the lean stroker to unblock
the SoftBMC dashboard's wrapped-text labels, then G9 to give the AGT
toolkit a real display list in place of its test-only recording
fixture.)

Phase G14 *(SHIPPED `dae9585f`)* — **analytic anti-aliased path
rasterizer**.  Replace
`axl_gfx_fill_path`'s former **4×4 supersampled even-odd**
rasterizer with a signed-area / active-edge-table coverage
rasterizer (the AGG "cell" method, same family as FreeType-smooth,
stb_truetype's v2 rasterizer, and font-rs).  Today's sampler gives
17 discrete coverage levels and pays 16× the per-pixel work on
covered spans; analytic coverage computes the *exact* fractional
area each pixel is covered in a single scanline pass — continuous
edges (visibly smoother on thin / near-axis features) at
`O(edges)` instead of `O(16 × covered pixels)`.  Also add
**non-zero winding** alongside even-odd (today's rasterizer is
even-odd only; non-zero is the SVG/CSS default and the only
correct rule for self-intersecting paths).  Pure fixed-point area
math — no AxlMath dependency.  Note our **text is already
analytic-AA'd** (stb_truetype v2 rasterizes glyph coverage
exactly); G14 brings path fills up to the quality our glyphs
already have.  Behavior-changing upgrade to the G1/G3 core →
lands test-protected (bucket C) with golden-image diffs and a
documented quality bump.  **DECIDED (AD1, spike 2026-05-29): borrow
FreeType `ftgrays.c` in `STANDALONE_` mode** (3 freestanding symbols
→ axl; direct span-callback output; FTL attribution).  Implementation
shape: vendor `ftgrays.c` + `ftimage.h` + `ftgrays.h` under
`deps/freetype/` with a ~6-line standalone shim (route
`malloc`/`free`/`memset` to axl; provide `FALL_THROUGH`, the few
`FT_*` typedefs, `FT_QNEW_ARRAY`/`FT_FREE`); marshal `AxlGfxPath`
edges → `FT_Outline` (26.6 fixed point, all-ON tags, contour array,
y-flip); render with `FT_RASTER_FLAG_DIRECT` + a span callback that
modulates alpha by coverage and forwards to `fill_rect_i` / gradient;
add non-zero **and** even-odd via `FT_OUTLINE_EVEN_ODD_FILL`; delete
the 4×4 supersample outright (no compat mode).  ~2–4 days.

Phase G15 *(SHIPPED — v1)* — **gamma-correct (linear-light)
compositing**.  Module-global opt-in flag
`axl_gfx_set/get/reset_gamma_correct` (off by default — plain sRGB, zero
cost, matching Cairo/Blend2D).  When on, every buffer-target
alpha/coverage composite decodes src+dst sRGB→linear, source-over in
linear, re-encodes — removing the dark-fringe on anti-aliased text/path
edges and correcting translucent-overlay brightness.  Public sRGB
transfer helpers `axl_gfx_srgb_to_linear` / `axl_gfx_linear_to_srgb`
(uses `axl_pow`, M2); internal 256-entry decode + 4096-entry encode
LUTs built lazily on enable.  One gamma-aware `composite()` wrapper sits
under all the buffer compositing sites (text AA, path AA, fills,
transparency, transformed blit), so the fix is universal; the SIMD
source-over kernel is bypassed while gamma is on.  **v1 scope:** the
source-over composite is linearized for all modes; the separable PDF
blend *functions* and gradient color *interpolation* stay sRGB
(follow-on G15b: linear gradient ramps via the same transfer helpers).
`axl_gfx_blend_ex` stays a pure sRGB primitive.  **G15b *(SHIPPED)*:**
gradient color *ramps* now also interpolate in linear light under the
flag (`lerp` decodes the stop colors via the transfer helpers, lerps,
re-encodes; alpha stays plain coverage) — the gradient-midpoint fix from
the original G15 description.  Remaining sRGB-only path: the separable
PDF blend *functions*.

Phase G16 — **LCD subpixel text AA** (RGB-stripe).  **Lowest
priority of any G-phase — keep on the list, build last (if
ever).**  Distinct from G7's subpixel *positioning*: rasterize
glyph coverage at 3× horizontal resolution and map to R/G/B
subpixels through a filter for sharper text on LCD panels.  GOP
does not report panel subpixel order, so this is **opt-in with a
configurable order (RGB/BGR), off by default** — wrong order tints
edges, and rotated / unknown firmware panels make it risky.  Kept
only to round out the text-quality story; a firmware UI gets far
more from G14 + grayscale AA.  ~1 week.

### Rendering backend & acceleration (Phase G17+)

How pixels reach the panel, and how fast.  Today: draw into a
system-RAM `AxlGfxBuffer`, then `axl_gfx_buffer_present` blits it
to video via GOP `Blt(EfiBltBufferToVideo)`.  That already gives
us a clean double-buffer (we composite in cached RAM and never
read back from VRAM — the right discipline).  The GPU story and
the direct-framebuffer story are discussed at length in
"Acceleration strategy" below; the committed phases are:

Phase G17 — **direct-framebuffer present path**. ✅ **SHIPPED.**
Writes the back-buffer straight to `FrameBufferBase` (exposed via
`buffer_get_info`), honoring `PixelFormat` and `PixelsPerScanLine`
stride.  The pure pixel pack is the public `axl_gfx_pack_pixel(px,
order)` over `AxlGfxPixelOrder` (`AXL_GFX_PIXEL_BGRA` →
identity/memcpy since `AxlGfxPixel` is stored BGRA; `AXL_GFX_PIXEL_
RGBA` → red/blue swap).  **Never reads VRAM** — write-only row
copies; compositing stays in the cached RAM buffer.  Falls back to
GOP `Blt` for `PixelBitMask` / `PixelBltOnly` **and when
`FrameBufferBase == 0`** (observed: AARCH64 `virtio-gpu-pci` under
OVMF reports a zero base, so the Blt fallback is load-bearing, not
just a `PixelBltOnly` corner).  **Non-temporal streaming stores: DONE
on x86** — the direct-FB write uses `MOVNTDQ` (`_mm_stream_si128`, with
ordinary stores for the unaligned prefix) + `MOVNTI` (`_mm_stream_si32`)
for the tail, then one `SFENCE` after the present to flush the
weakly-ordered stores before the scanout / `axl_gfx_capture` reads.
This fills the FB's write-combine buffers in full bursts without
polluting the data cache with pixels we never read back.  (The earlier
note that NT stores were "blocked by gcc" was wrong — only the
Clang-only `__builtin_nontemporal_store` is unavailable; gcc has the
SSE2 `_mm_stream_*` intrinsics at baseline.)  AArch64 still uses
ordinary stores; a `stnp` non-temporal path there is a future
refinement.  Correctness validated by the present round-trip (the
SFENCE is load-bearing for the in-guest read-back); the perf benefit is
hardware-specific (QEMU's RAM-backed FB can't measure it).

Phase G18 — **dirty-rectangle present**. ✅ **SHIPPED.**
`axl_gfx_buffer_present_rect(buf, dst_x, dst_y, src_x, src_y, w, h)`
pushes a sub-region; per-buffer damage tracking unions dirty rects
into a bbox on the buffer (`axl_gfx_buffer_add_damage`), flushes
just that bbox (`axl_gfx_buffer_present_damage`, which clears after),
plus `clear_damage` / `get_damage`.  All three present variants share
one clamped path (`present()` = `present_rect(whole buffer)`;
`present_damage` = `present_rect(damage bbox)` then clear), so G17's
direct-FB write applies uniformly.  Composes with G9.  Verified end-
to-end on a real linear-FB GOP by `test/integration/test-gfx-
present-qemu.sh` (present a known pattern → read back in-guest via
`axl_gfx_capture` → assert pixels), both arches; pure logic
unit-tested in `axl-test-gfx.c`.

Phase G19 — **MP-Services parallel rasterization**.  The one
acceleration lever genuinely available pre-OS that the peer
libraries can't use the same way: AXL already has an MP Services
backend (`axl-backend-native-mp.c`).  Partition the scanline
rasterizer (G14) and/or compute-bound effects (G6 blur, G15 gamma)
across application processors via `StartupAllAPs`.  Blend2D
multithreads on an OS scheduler; we'd multithread on bare-metal
APs.  Real care needed (AP teardown before `ExitBootServices`,
false sharing on row boundaries, no synchronization primitives
beyond what MP Services gives).  **Gated on Spike G19 below** —
confirm a real speedup on a compute-bound workload before
committing the implementation.  ~1 week post-spike.

### Acceleration strategy — GPU vs CPU vs direct framebuffer

Recorded decision so we don't re-litigate it every perf
conversation.

**Direct framebuffer access — yes, do it (G17).**  GOP hands us a
linear framebuffer: `FrameBufferBase`, `PixelsPerScanLine` stride,
and a `PixelFormat` (almost always 32-bpp BGR or RGB; occasionally
a bitmask; rarely `PixelBltOnly` with no linear FB at all).  We
already expose these via `buffer_get_info`.  Writing rows straight
to that base can beat GOP `Blt` (which carries per-call overhead
and, in most firmware, is just a CPU copy anyway).  The
non-negotiable rules: the framebuffer is typically **write-
combining or uncached MMIO in VRAM — never read it back** (an
uncached read-modify-write per pixel is orders of magnitude slower
than RAM); composite everything in the cached RAM back-buffer and
push opaque pixels out; use sequential, ideally **non-temporal
streaming stores** so we fill write-combine buffers efficiently
without polluting cache.  Keep GOP `Blt` as the portable fallback
(and the only option under `PixelBltOnly`).

**GPU acceleration — hard non-goal for a real driver.**  Pre-OS
there is no GPU driver and we don't want to be one:

- GOP/firmware already did mode-setting; we inherit a configured
  scanout.  There is no command submission path exposed to us.
- A bare-metal GPU driver is vendor- *and generation*-specific
  (Intel i915-class, AMD amdgpu, NVIDIA): command rings, firmware
  blobs (Intel GuC/HuC, AMD PSP/SMU microcode), GTT/VRAM memory
  management, fence/IRQ handling.  That is an enormous, fragile,
  unmaintainable surface for a general library — and it would have
  to be re-done per GPU family.  Categorically not worth it.
- The only firmware-provided "acceleration" is GOP `Blt`, which
  *some* firmware backs with the chip's 2D blitter but most
  implement as a software copy — and we can't tell which.  We
  already use it; we can't rely on it being hardware.

**Therefore acceleration is CPU-side**, and that's where the wins
actually are for a 2D firmware UI:

- **SIMD** — the post-2020 cap-off is already chosen (build still
  defaults to `-march=x86-64`); wire SSE2/AVX2 + NEON into the
  G14 rasterizer span fills and the G17 present blit.
- **Less work** — G14 analytic coverage (vs 16× supersampling).
- **Less bandwidth** — G18 dirty-rectangle present.
- **More cores** — G19 MP-Services parallel rasterization, the one
  lever genuinely unique to our environment.

**Decision (confirmed): no GPU.**  Writing or vendoring a
pre-OS GPU driver is a permanent non-goal — not "deferred,"
not "revisit later."  All rendering is and stays CPU-side; the
acceleration roadmap is SIMD + G14 + G18 + G19.  Do not reopen
this without a fundamentally new constraint (e.g. a future
firmware exposing a standard, vendor-neutral 2D acceleration
protocol — which does not exist today).

### AxlMath follow-ups (Phase M1+)

> **Status (2026-05-31): the entire M-series M1–M10 is SHIPPED.** All of
> the inverse trig, pow/exp/log, the transform type (M3, consolidated to
> `AxlTransform`), lerp + easing, clamp/min/max, bit math, saturated
> arithmetic, geometry helpers, wrap, and the extra constants are in
> `<axl/axl-math.h>` and unit-tested. Entries below are kept as the
> design record.

Phase M1 *(SHIPPED)* — **`axl_atan2(y, x)`** + companions (`axl_atan`,
`axl_asin`, `axl_acos`).  Single most-asked-for math function we
don't have — vector-angle math (mouse direction, animation
rotation toward target, arc-from-endpoints).  Taylor / rational
approximation.  ~3 days.

Phase M2 *(SHIPPED)* — **`axl_pow` / `axl_exp` / `axl_log`**.  Un-stubs
stb_image's gamma path (currently a stub in `axl-pixmap.c`),
required for sRGB ↔ linear conversions used by G5 quality
gradients, foundation for any easing curve more sophisticated
than cubic.  ~3 days.

Phase M3 *(SHIPPED)* — **`AxlVec2` + transform type**.  Vec2 with
`add` / `sub` / `dot` / `length` / `normalize` (+ `lerp` / `distance` /
`perp` / `cross` / `rotate` / `angle` / `reflect` / `project`).  The 3×3
transform began as `AxlMat3` and was **consolidated on 2026-05-31 into
the single projective `AxlTransform`** (`axl_transform_identity` /
`translate` / `scale` / `rotate` / `shear` / `multiply` (cairo a-first) /
`invert` / `map_point` / `map_vector` / `determinant` / `perspective` /
`quad_to_quad` / `map_rect` / `map_quad` / `classify`).  Substrate for
G4's transform stack and the perspective-correct gfx primitives — this is
the one transform type the whole library shares (no GFX-side Vec2/Mat3
duplication).

Phase M4 *(SHIPPED)* — **`axl_lerp(a, b, t)` + easing palette**
(`axl_ease_in_cubic` / `axl_ease_in_out_cubic` /
`axl_ease_out_quint` / `axl_ease_in_out_sine` / etc.).
Animation primitive used by AGT widget transitions, CSS
`transition` / `animation`.  Trivially small (~5 LOC each).  ~2
days.

Phase M5 *(SHIPPED)* — **`axl_clamp` / `axl_min` / `axl_max` / `axl_remap`
/ `axl_step` / `axl_smoothstep`**.  Ubiquitous; we already use
inline equivalents in several places.  ~1 day.

Phase M6 *(SHIPPED)* — **bit math** — `axl_clz` / `axl_ctz` /
`axl_popcount` / `axl_log2i` / `axl_round_up_pow2`.  Buffer
sizing, hash math, allocator bookkeeping.  Thin wrappers around
`__builtin_*` (which are inline-able, no libgcc call).  ~2 days.

Phase M7 *(SHIPPED)* — **saturated arithmetic** — `axl_sat_add_u8` /
`axl_sat_sub_u8` / `axl_sat_mul_u16`.  Alpha blending without
overflow; useful any time the existing `(uint32_t)(a) * (b) /
255` pattern needs to be terse.  ~2 days.

Phase M8 *(SHIPPED)* — **geometry helpers** — `axl_point_in_rect` /
`axl_rect_intersect` / `axl_rect_union` /
`axl_segment_intersect` / `axl_distance_point_to_segment` /
`axl_circle_circle_intersect`.  Every UI codebase reimplements
these; centralize.  ~3 days.

Phase M9 *(SHIPPED)* — **`axl_wrap(x, n)`** — value in `[0, n)` regardless
of sign.  Circular indices, angle normalization (modulo 2π).
~1 day.

Phase M10 *(SHIPPED)* — **math constants beyond π** — `AXL_MATH_E` /
`AXL_MATH_SQRT_2` / `AXL_MATH_LOG_2` / `AXL_MATH_GOLDEN` / `AXL_MATH_DEG_TO_RAD` /
`AXL_MATH_RAD_TO_DEG`.  ~1 day.

### Dependency graph at a glance

Hard prereqs (don't build the bottom without the top):

- **G4** (transforms) ⇐ **M3** (Vec2/`AxlTransform`)
- **G5** quality gradients ⇐ **M2** (pow/exp/log) — soft, ship
  without first if needed
- **G6** shadow distance falloff ⇐ **M2** (pow) — same: soft
- **G9** display list ⇐ **G4** (recorded transforms compose) —
  hard if you want them in the same trace
- **G15** gamma-correct compositing ⇐ **M2** (pow/exp/log) — hard
- **G18** dirty-rect present ⇐ **G9** (display list) — soft;
  exact dirty regions need the trace, conservative boxes don't
- **G8** strokes / **G10** path clip both inherit **G14**'s edge
  quality — sequence G14 first if you care about AA on either

Everything else is free-standing.  M1 (atan2), M4 (easing), M5
(clamp/min/max), M6 (bit math), M7 (sat arith), M8 (geometry),
M9 (wrap), M10 (constants) can land in any order independent of
the rest.

## Spike A1 — Lexbor viability (1-2h)

**Branch:** `spike/lexbor-uefi`. Throwaway per [[spike-solutions]].

**Question:** Does Lexbor's HTML5 parser compile against axl-cc and
run inside a UEFI app? Memory allocator hookable to AxlMem? Any
hidden POSIX deps?

**Done when:** A 50-LOC UEFI program parses an embedded HTML string
into a Lexbor DOM tree and walks it printing `<tag> "text"` for each
node. Prints to console via `axl_print`.

**Kill criteria:** Lexbor needs `pthread_*`, `mmap`, dlopen, or fork.
(Read its README first to short-circuit.)

**Fallback if Lexbor doesn't fit:** Try NetSurf libhubbub + libdom
(BSD, plain C, designed for freestanding targets — but older + less
ergonomic).

## Spike C1 — QuickJS viability (1-2h)

**Branch:** `spike/quickjs-uefi`. Throwaway.

**Question:** Does QuickJS compile against axl-cc, run with AxlMem
as its allocator, and execute non-trivial ES code? Specifically the
ES2020 features that modern toolkits rely on (async/await, classes,
modules, destructuring).

**Done when:** A 100-LOC UEFI program runs:

```js
const sum = (...xs) => xs.reduce((a,b)=>a+b, 0);
print(sum(1,2,3,4,5));  // 15
```

via `JS_NewRuntime` + `JS_NewContext` + `JS_Eval`, with `print` bound
to `axl_print`. Plus an `async`/`await` test to confirm the event
loop integration story.

**Kill criteria:** QuickJS requires `setjmp`/`longjmp` in a way axl-cc
can't satisfy; or its GC needs `mmap`/`mprotect` for stack scanning.
(QuickJS is known-portable, so kills are unlikely — but verify.)

## Spike G19 — MP-Services parallel rasterization (time-boxed)

**Branch:** `spike/mp-rasterize`. Throwaway per [[spike-solutions]].
Gates Phase G19 — confirm the speedup is real before building it in.

**Question:** Does partitioning a *compute-bound* rendering
workload across application processors via the MP Services backend
(`StartupAllAPs`) give a worthwhile wall-clock speedup in UEFI, and
is the result correct + the AP teardown clean?

**Why compute-bound specifically:** plain blits and solid fills are
**memory-bandwidth-bound** — all cores share one memory controller,
so MP parallelism buys little or nothing there.  The spike must
target work that is arithmetic-heavy per byte (a full-screen **G6
blur**, or analytic rasterization of a **complex path** with many
edges), or it will measure the wrong thing and falsely kill.

**Done when:** a representative compute-bound frame, split N ways
across APs, runs **≥2× faster on ≥4 cores** than the single-core
path, produces **byte-identical output** to single-core, and the
APs are quiesced cleanly (no hang, safe well before any
`ExitBootServices`). Measure with the real-µs log timestamps, not
estimates.

**Kill criteria:** `StartupAllAPs` unavailable or serialized by the
target firmware; speedup `< 1.5×` even on the compute-bound
workload (bandwidth- or sync-overhead-bound, not compute-bound —
parallelism won't pay); or AP synchronization/teardown proves
unsafe. A kill here means G19 stays shelved and acceleration leans
entirely on SIMD + G14 + G18.

## Phase A2 — HTML/CSS prototype (multi-week, GREEN-LIT BY A1)

**Branch:** `feature/html-css-renderer` (not a spike — real PR
discipline, TDD bucket A per [[tdd-mandatory]]).

Scope (in order — each step independently shippable):

1. Parse HTML → DOM (Lexbor wraps).
2. Parse inline CSS → style rules (Lexbor's CSS parser or libcss).
3. Cascade + inherit → computed styles per node.
4. Box-tree layout — initial subset: block flow + inline flow + flex
   (skip grid, skip floats, skip positioned). Document the subset
   precisely; consumers need to know what works.
5. Paint — walk the box tree, emit AxlGfx draw calls. Uses Phase
   G1 (text), G2 (`background-image`, `<img>`), optionally G3 for
   `border-radius` etc.
6. Hit-testing — map (x, y) back to DOM node for input dispatch.

**Explicit non-goals for A2:**
- No CSS animations / transitions (defer).
- No JS integration — that's A3.
- No layout reflow on data change — paint once, throw away on
  re-render.
- No subpixel positioning, no kerning beyond what stb_truetype gives.
- No SVG, no MathML, no `<canvas>`, no shadow DOM, no Web Components.

**Exit criterion:** Renders a non-trivial real-world page (something
like a static React docs page with the JS stripped) at recognizable
fidelity. "Recognizable" = a human looking at the UEFI render and a
Chrome render side-by-side can identify them as the same page.

**Effort:** 2-3 months focused.

## Phase C2 — QuickJS-bound axl runtime (multi-week, GREEN-LIT BY C1)

**Branch:** `feature/quickjs-bindings` (not a spike).

Scope:

1. Bind AxlGfx (rect/line/text/blit/buffer/clip) into JS as a
   `gfx` global.
2. Bind AxlInput as a JS event source — JS callbacks fire on mouse
   / key / touch via axl-loop integration.
3. Bind AxlNet (TCP, HTTP client) — gives JS code `fetch`-shaped
   API for talking to remote services.
4. Bind AxlFs — read/write to UEFI volumes from JS.
5. Implement enough of the ES module loader to import multiple .js
   files from disk.
6. Integrate QuickJS' event loop with axl-loop (axl-loop owns the
   process loop; QuickJS' job queue drains on each tick).

**Explicit non-goals for C2:**
- No browser globals — no `window`, no `document`, no `localStorage`.
  Those are A3's job.
- No Web Worker, no SharedArrayBuffer.
- No JIT (QuickJS is interpreter-only; UEFI lacks W^X anyway).

**Exit criterion:** A JS program of 200+ LOC runs to completion
exercising gfx + input + net + fs bindings. Round-trip a HTTP request
from JS and render the response into the framebuffer.

**Effort:** 1-2 months focused.

## Phase A3 — A+C convergence (the actual goal)

**Branch:** `feature/dom-js-bindings` (after A2 and C2 land
independently).

Wire QuickJS's value system to the DOM tree produced by Phase A2.
That means:

1. Each DOM node gets a JS wrapper (a JSValue with an opaque pointer
   back to the C node).
2. Mutating the JS wrapper mutates the C DOM; the renderer re-walks
   on dirty.
3. Bind `document`, `window`, `addEventListener`, the small set of
   DOM methods that toolkits actually use (createElement, appendChild,
   removeChild, setAttribute, querySelector).
4. Plug `fetch` from C2's AxlNet binding into a `window.fetch`
   global.
5. Validate against a real JS toolkit. Top candidate: **Preact** (5
   KB, no JSX compiler needed if used with `htm`, designed for
   constrained environments). lit-html is the alternative. Solid
   requires a compiler step. React is the biggest stretch.

**Exit criterion:** A Preact app renders to the UEFI framebuffer,
responds to mouse/keyboard input, calls fetch, re-renders on state
change. This is the headline feature.

**Effort:** 2-3 months on top of A2 + C2.

## Path B — WASM (deferred, NOT planned)

Documented for completeness. Path A+C subsumes most of the use case
(modern dynamism + JS-ecosystem reach). Revisit if:

- A real consumer needs Rust/Zig/Go in pre-boot UEFI.
- Sandbox isolation between guest code and AxlGfx becomes a
  security requirement (WASM provides isolation cheaply; QuickJS
  doesn't).
- Performance of QuickJS in C2 hits a wall we can't tune around.

If revisited: WAMR (Apache 2.0, designed for embedded) is the target
runtime. wasm3 is the fallback if WAMR is too big.

## Decision gate

After A1 and C1 spikes land (≤1 week wall-clock total), pause and
review:

- Both green → commit to G1+G2 and start A2+C2 in parallel.
- A1 fails, C1 green → ship just Path C. Useful but downgraded scope
  (scripted UI without HTML).
- A1 green, C1 fails → ship A2 as a static-HTML viewer (kiosk-shape).
  Reconsider B for dynamism.
- Both fail → revisit Path B (WAMR) seriously.

No commitments until after the spikes. The whole point of running
them first is to gate the multi-month investments on cheap empirical
answers.

## Tracking

Add to ROADMAP under its own section once Spike A1 lands. Until then,
this doc IS the plan; ROADMAP just points at it.

## Open questions (not blocking)

- Font licensing — what TTF ships embedded by default? DejaVu Sans
  (public domain), Liberation Sans (OFL), or wait and require
  consumers to provide their own?
- HTTPS — Phase A3's `fetch` needs TLS. AxlNet has TLS (optional
  build flag). Decide whether Rich-UI builds force TLS on.
- Memory budget — UEFI apps in pre-boot typically run with single-
  digit MB available before fragmentation bites. Lexbor + QuickJS +
  a non-trivial DOM may not fit. Spike A1 and C1 should each report
  peak heap use after a 1000-element parse / a 10k-line script eval.
