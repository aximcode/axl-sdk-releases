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

## Prerequisite — AxlGfx 2D upgrade

**Co-consumer: AGT** (sibling C++ widget toolkit). AGT Phase 2.6
(AgtDrawContext + RecordingDrawContext) and Phase 2.7a (label/
button/frame chain) are already shipped; the G1/G2/G3 primitives
below are additive consumers for AGT, not blockers. AGT consumes
them on its own schedule via additive virtuals on AgtDrawContext.
AGT input on the API shape is captured in
[agt/docs/handoffs/2026-05-28-agt-input-axl-gfx-2d-upgrade.md](../../agt/docs/handoffs/2026-05-28-agt-input-axl-gfx-2d-upgrade.md)
(2026-05-28) and reflected in the G1/G2/G3 surfaces below.

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

Phase G4 — **affine transforms + transform stack**.  Public:
`axl_gfx_translate` / `axl_gfx_scale` / `axl_gfx_rotate` /
`axl_gfx_skew` / `axl_gfx_push_transform` / `axl_gfx_pop_transform`
/ `axl_gfx_get_transform`.  Substrate for CSS `transform`, AGT
button hover-scale, animation rotation.  **Depends on M3** (Vec2
/ Mat3); don't start before that.  ~1 week.

Phase G5 — **gradients (linear + radial)**.  `AxlGfxGradient`
opaque object with `add_stop(t, color)` builder; new `_gradient`
variants of fill_rect / fill_path / fill_rounded_rect.  Table
stakes for any UI built after ~2005 (button backgrounds, hero
panels, CSS `linear-gradient` / `radial-gradient`).  ~1 week.
Quality wins from **M2** (`axl_pow` for sRGB↔linear blending
mid-gradient) but doesn't strictly require it.

Phase G6 — **Gaussian blur + drop shadows**.  Stack-blur kernel
on `AxlGfxBuffer` (~150 LOC, O(N) per pixel regardless of
radius).  Shadows compose: render to back-buffer → blur → blit at
offset.  Enables CSS `box-shadow`, `text-shadow`, `filter: blur`.
~1 week.

Phase G7 — **glyph cache** keyed on `(font, codepoint, px_size
quantized, subpixel_offset)` → bitmap.  Currently `axl_ttf_draw`
re-rasterizes every glyph per call; real text-heavy UIs hit this
wall fast.  LRU eviction, hidden behind axl-truetype.c (no
public API change).  ~1 week.

Phase G8 — **stroke styling**.  Width > 1 (Minkowski-sum offset
of the stroke path), caps (butt / round / square), joins (miter /
round / bevel), dashed lines.  Already on the G3 follow-up
backlog.  ~1 week.

Phase G9 — **first-class display list / scene graph**.  Record
draw operations into an `AxlGfxDisplayList`; replay primitive +
optional serialization for trace/debug.  Would supersede AGT's
`RecordingDrawContext` (currently a test-only fixture).  Pairs
with G4 (recorded transforms compose cleanly).  ~2 weeks.

Phase G10 — **path-based clip**.  Extend the clip stack to accept
`AxlGfxPath` in addition to `AxlGfxClip` rect.  Combined with G4
transforms = arbitrary-shape masking.  Foundation for CSS
`clip-path`.  ~3 days (the rasterizer already handles paths;
this is plumbing).

Phase G11 — **multi-line text + word wrap**.  `axl_ttf_draw_box`
that takes a rect, wraps UTF-8 on whitespace + grapheme
boundaries.  Optional `axl_ttf_measure_box` for layout-before-
draw.  Every label heavier than a button needs this.  ~1 week.

Phase G12 — **pattern fill / tile blit**.  `axl_gfx_fill_pattern
(rect, AxlGfxBuffer *, repeat_mode)`.  CSS
`repeating-linear-gradient`, textured backgrounds, slice-9
buttons.  ~3 days.

Phase G13 — **blend modes** beyond source-over.  Extend
AxlGfxPixel composite path with a mode flag (`AXL_GFX_BLEND_
SOURCE_OVER` / `_MULTIPLY` / `_SCREEN` / `_DARKEN` / `_LIGHTEN` /
`_OVERLAY`).  Enables CSS `mix-blend-mode`.  ~3 days.

### AxlMath follow-ups (Phase M1+)

Phase M1 — **`axl_atan2(y, x)`** + companions (`axl_atan`,
`axl_asin`, `axl_acos`).  Single most-asked-for math function we
don't have — vector-angle math (mouse direction, animation
rotation toward target, arc-from-endpoints).  Taylor / rational
approximation.  ~3 days.

Phase M2 — **`axl_pow` / `axl_exp` / `axl_log`**.  Un-stubs
stb_image's gamma path (currently a stub in `axl-pixmap.c`),
required for sRGB ↔ linear conversions used by G5 quality
gradients, foundation for any easing curve more sophisticated
than cubic.  ~3 days.

Phase M3 — **`AxlVec2` + `AxlMat3` types** with
`add` / `sub` / `dot` / `length` / `normalize` /
`transform_point` / `mat3_mul` / `mat3_identity` /
`mat3_translate` / `mat3_scale` / `mat3_rotate`.  Substrate for
G4's transform stack — **do M3 before G4**, otherwise GFX
re-rolls its own Vec2/Mat3 and we're back to the duplication
problem AxlMath just solved.  ~1 week.

Phase M4 — **`axl_lerp(a, b, t)` + easing palette**
(`axl_ease_in_cubic` / `axl_ease_in_out_cubic` /
`axl_ease_out_quint` / `axl_ease_in_out_sine` / etc.).
Animation primitive used by AGT widget transitions, CSS
`transition` / `animation`.  Trivially small (~5 LOC each).  ~2
days.

Phase M5 — **`axl_clamp` / `axl_min` / `axl_max` / `axl_remap`
/ `axl_step` / `axl_smoothstep`**.  Ubiquitous; we already use
inline equivalents in several places.  ~1 day.

Phase M6 — **bit math** — `axl_clz` / `axl_ctz` /
`axl_popcount` / `axl_log2i` / `axl_round_up_pow2`.  Buffer
sizing, hash math, allocator bookkeeping.  Thin wrappers around
`__builtin_*` (which are inline-able, no libgcc call).  ~2 days.

Phase M7 — **saturated arithmetic** — `axl_sat_add_u8` /
`axl_sat_sub_u8` / `axl_sat_mul_u16`.  Alpha blending without
overflow; useful any time the existing `(uint32_t)(a) * (b) /
255` pattern needs to be terse.  ~2 days.

Phase M8 — **geometry helpers** — `axl_point_in_rect` /
`axl_rect_intersect` / `axl_rect_union` /
`axl_segment_intersect` / `axl_distance_point_to_segment` /
`axl_circle_circle_intersect`.  Every UI codebase reimplements
these; centralize.  ~3 days.

Phase M9 — **`axl_wrap(x, n)`** — value in `[0, n)` regardless
of sign.  Circular indices, angle normalization (modulo 2π).
~1 day.

Phase M10 — **math constants beyond π** — `AXL_MATH_E` /
`AXL_MATH_SQRT_2` / `AXL_MATH_LOG_2` / `AXL_MATH_GOLDEN` / `AXL_MATH_DEG_TO_RAD` /
`AXL_MATH_RAD_TO_DEG`.  ~1 day.

### Dependency graph at a glance

Hard prereqs (don't build the bottom without the top):

- **G4** (transforms) ⇐ **M3** (Vec2/Mat3)
- **G5** quality gradients ⇐ **M2** (pow/exp/log) — soft, ship
  without first if needed
- **G6** shadow distance falloff ⇐ **M2** (pow) — same: soft
- **G9** display list ⇐ **G4** (recorded transforms compose) —
  hard if you want them in the same trace

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
