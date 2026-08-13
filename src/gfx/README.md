Basic graphics output over the UEFI GOP (Graphics Output Protocol).
Fill rectangles, blit pixel buffers, draw lines and outlines, render
text from a pluggable bitmap font, composite through a clip stack,
and double-buffer via off-screen pixel buffers. Falls back gracefully
on headless systems where GOP is not available.

Headers:

- `<axl/axl-gfx.h>` — framebuffer ops, off-screen buffers, clipping,
  drawing primitives, paths, rounded rects, text rendering
- `<axl/axl-font.h>` — bitmap font primitives (`AxlFont`, `AxlGlyph`)
  consumed by `axl-gfx` and authored by tools / generators
- `<axl/axl-truetype.h>` — vector text via stb_truetype. See the
  `AxlTtf module <truetype.html>`_ page for the full surface.
- `<axl/axl-pixmap.h>` — PNG / JPEG / GIF / BMP decoders via
  stb_image; returns blittable `AxlGfxBuffer`. See the
  `AxlPixmap module <pixmap.html>`_ page.

This module is the **graphics substrate** for higher-level toolkits.
Per [docs/AGT-Design.md](../../docs/AGT-Design.md) §"Substrate
discipline rules", `axl-gfx` is pure C and paradigm-agnostic — it
draws pixels, not widgets. Hierarchy, layout, theming, and event
dispatch live in the toolkit on top (AGT, or a future GTK-shape peer).

## Overview

Not all UEFI systems have a display. Serial-only servers and headless
BMCs typically lack GOP. Always check `axl_gfx_available()` before
drawing:

```c
#include <axl.h>

if (!axl_gfx_available()) {
    axl_printf("No display — running headless\n");
    return;
}

AxlGfxInfo info;
axl_gfx_get_info(&info);
axl_printf("Display: %ux%u\n", info.width, info.height);

axl_gfx_fill_rect(0, 0, info.width, info.height, AXL_GFX_BLACK);

const AxlFont *font = axl_gfx_default_font();
axl_gfx_draw_text(font, 20, 20, "Hello from AXL!", AXL_GFX_WHITE, 2);
```

### Pixel Format

Pixels use BGRA layout (`AxlGfxPixel`): blue, green, red, alpha.
The BGR ordering matches the native GOP pixel format on most hardware,
avoiding conversion overhead. The 4th byte is `alpha` — 0xFF means
fully opaque (default for solid colors), 0 means fully transparent.
Alpha-blending is honored on buffer targets; screen-target draws
treat all pixels as opaque (GOP has no blending hardware — render
to a back-buffer first if you want semi-transparent overlays).

For ergonomic RGB-style construction without paying any per-pixel
byte-swap cost, use the convenience macros:

```c
AxlGfxPixel tomato      = AXL_GFX_RGB(0xFF, 0x63, 0x47);     // CSS #FF6347
AxlGfxPixel translucent = AXL_GFX_RGBA(0xFF, 0x00, 0x00, 0x80);  // 50% red
axl_gfx_fill_rect(10, 10, 100, 100, AXL_GFX_RED);             // named palette
```

These expand to BGRA compound literals at compile time — zero
runtime cost. Named colors available: `AXL_GFX_BLACK`,
`AXL_GFX_WHITE`, `AXL_GFX_RED`, `AXL_GFX_GREEN`, `AXL_GFX_BLUE`,
`AXL_GFX_YELLOW`, `AXL_GFX_CYAN`, `AXL_GFX_MAGENTA`, `AXL_GFX_GRAY`,
`AXL_GFX_TRANSPARENT`.

### Alpha compositing

`axl_gfx_blend(dst, src)` returns the source-over composite of two
pixels using 8-bit integer math:

```
out.rgb = (src.rgb * a + dst.rgb * (255 - a) + 127) / 255    where a = src.alpha
out.alpha = 0xFF                                              (destination treated as opaque)
```

The drawing primitives (`fill_rect`, `draw_line`, `draw_text`, etc.)
apply this internally when both the target is a buffer and the source
alpha is non-zero and non-opaque. Screen targets degrade to opaque
draws — render to a back-buffer first for translucent overlays.

The constant-color source-over fill (a translucent `fill_rect` onto a
buffer) is **SIMD-accelerated** via `axl_cpu_simd_tier()` — an AVX2,
SSE2 (the x86-64 baseline, so every x86 build benefits), or NEON row
kernel, bit-identical to the scalar formula above (the `/255` uses the
exact `(Y + (Y>>8) + 1) >> 8` identity, valid because the numerator is
≤ 255²+127). The separable PDF blend modes below stay on the scalar
path. See the ``AxlCpu <cpu.html>``_ module for the dispatch machinery.

### Gamma-correct (linear-light) compositing

By default compositing happens in gamma-encoded sRGB (free, matches
Cairo/Blend2D). `axl_gfx_set_gamma_correct(true)` switches buffer-target
alpha/coverage compositing to **linear light** — decode src+dst
sRGB→linear, blend, re-encode — which is the physically correct way to
mix light. It removes the "dark fringe" on anti-aliased text and path
edges (thin text keeps its weight) and corrects the brightness of
translucent overlays and fades. It's a module-global flag (save/restore
via `axl_gfx_get_gamma_correct`, off by default), costs two small lazily
-built LUTs, and applies uniformly under every compositing primitive.
The sRGB transfer functions are exposed as `axl_gfx_srgb_to_linear` /
`axl_gfx_linear_to_srgb` for consumers doing their own color math.
Gradient color ramps also honor the flag (interpolated in linear light
for even, dark-dip-free transitions). The remaining sRGB-only paths are
the separable PDF blend *functions* (multiply/screen/…).

### Blend modes

`axl_gfx_set_blend_mode(mode)` selects how subsequent draws composite —
the standard separable blend functions on top of source-over:

| Mode | Effect |
|------|--------|
| `AXL_GFX_BLEND_OVER` | normal source-over (default) |
| `AXL_GFX_BLEND_MULTIPLY` | darken: `Cb·Cs/255` |
| `AXL_GFX_BLEND_SCREEN` | lighten: `255−(255−Cb)(255−Cs)/255` |
| `AXL_GFX_BLEND_OVERLAY` | contrast (multiply or screen per backdrop) |
| `AXL_GFX_BLEND_DARKEN` / `_LIGHTEN` | per-channel min / max |
| `AXL_GFX_BLEND_ADD` | additive, clamped to 255 |

It is module-global graphics state (like the draw target and clip
stack); every compositing primitive on a **buffer target** honors it —
fills, gradients, lines, text (bitmap + vector), and
`axl_gfx_blit_transform` — *including opaque draws* (an opaque source under
`MULTIPLY` still multiplies). Save/restore with `axl_gfx_get_blend_mode`
and reset to `AXL_GFX_BLEND_OVER` for normal drawing. The raw-copy
`axl_gfx_blit` and screen (GOP) targets are exempt (no compositing).
`axl_gfx_blend_ex(dst, src, mode)` exposes the pure per-pixel math.

```c
axl_gfx_set_blend_mode(AXL_GFX_BLEND_MULTIPLY);
axl_gfx_fill_rect(0, 0, w, h, shadow);   // multiplies onto the backdrop
axl_gfx_set_blend_mode(AXL_GFX_BLEND_OVER);
```

## Drawing Primitives

| Primitive | Function | Notes |
|---|---|---|
| Fill | `axl_gfx_fill_rect` | Solid color, screen or buffer (`uint32_t` coords) |
| Fill (signed) | `axl_gfx_fill_rect_i` | Same, but `int32_t` coords — partly off-screen widgets without manual clamping |
| Rounded fill | `axl_gfx_fill_rounded_rect` | SDF-based corners with anti-aliasing; the headline widget primitive |
| Outline | `axl_gfx_draw_rect` | 1-pixel-wide rectangle border |
| Line | `axl_gfx_draw_line` | Bresenham, signed origins, inclusive endpoints |
| Polyline | `axl_gfx_draw_polyline` | Connected segments through `AxlGfxPoint[]` |
| Path fill | `axl_gfx_fill_path` | 4x4-supersampled even-odd rasterizer; see "Paths" below |
| Path stroke | `axl_gfx_stroke_path` / `_ex` | width-`w` anti-aliased stroke; caps butt/round/square, joins miter/round/bevel, dashes (`AxlGfxStrokeStyle`); offset geometry filled non-zero — see `axl-gfx-stroke.c` |
| Blit | `axl_gfx_blit` | Copy a pixel buffer onto the target |
| Blit sub-rect | `axl_gfx_blit_rect` | Copy a sub-rectangle of a wider source (`src_stride`, `src_x/y`) — e.g. one sprite-sheet cell — onto the target without copying it out first |
| Pattern fill | `axl_gfx_fill_pattern` | Tile an `AxlGfxBuffer` over a rect; `AxlGfxRepeat` BOTH/X/Y/NONE (CSS `background-repeat`), anchored at the origin, honors clip + blend + per-texel alpha |
| Color parse | `axl_gfx_color_parse` | CSS hex string → `AxlGfxPixel` (`#RGB`, `#RGBA`, `#RRGGBB`, `#RRGGBBAA`) |
| Capture | `axl_gfx_capture` | Read screen region into a buffer |
| Text | `axl_gfx_draw_text` | UTF-8, bitmap-font-driven, integer-scaled |
| Text (vector) | `axl_ttf_draw` | UTF-8, TTF/OTF, fractional `px_size`; see `AxlTtf <truetype.html>`_ |
| Text (boxed) | `axl_ttf_draw_box` / `axl_ttf_measure_box` | word-wrapped multi-line text in a rect; `\n` hard breaks, `AXL_TTF_ALIGN_*` alignment, box clip; `measure_box` for layout-before-draw |
| Display list | `axl_gfx_display_list_new` / `axl_gfx_dl_*` / `axl_gfx_display_list_replay` | record draw ops into a retained, replayable command buffer; introspect via `_count` / `_op_at`; see ``AxlGfxDisplayList <display-list.html>``_ |

Lines, outlines, signed-coord fills, and polylines accept signed
coordinates so partly off-screen geometry is expressible directly
without caller clamping. The clip stack (below) handles the actual
pixel rejection.

## Paths and Rounded Rects

`AxlGfxPath` is a retained-mode path object — sequence of subpaths
built from `move_to` / `line_to` / `curve_to` / `arc` / `close`.
Curves and arcs are flattened to line segments at insertion time so
fill / stroke walk a uniform list. Fill uses 4x4 supersampling and
the even-odd rule (subpath intersections invert — outer ring + inner
ring makes a donut, matching SVG `fill-rule:evenodd`).

```c
AxlGfxPath *p = axl_gfx_path_new();
axl_gfx_path_move_to(p, 10.0f, 10.0f);
axl_gfx_path_line_to(p, 50.0f, 10.0f);
axl_gfx_path_line_to(p, 30.0f, 40.0f);
axl_gfx_path_close(p);
axl_gfx_fill_path(p, AXL_GFX_RED);
axl_gfx_path_free(p);
```

`axl_gfx_fill_rounded_rect` is an immediate-mode helper — it rasterizes
the 4 corners + 3 plain-fill bands directly via signed-distance
coverage, bypassing the path API. Faster than building a per-call
path for the case that dominates widget rendering (button + panel
backgrounds).

```c
axl_gfx_fill_rounded_rect(x, y, w, h, /* radius */ 6.0f, AXL_GFX_BLUE);
```

Stroke is fixed at 1 px wide in the current implementation — wider
strokes await a future batch. Math primitives (`sqrt`, `sin`, `cos`,
`floor`, `ceil`, `fabs`) come from the
`AxlMath <math.html>`_ module so the path rasterizer stays
libm-free.

## Gradients

`AxlGfxGradient` is an opaque, reusable gradient object — linear
(color axis from one point to another) or radial (offset by distance
from a center). Add up to `AXL_GFX_GRADIENT_MAX_STOPS` color stops in
any order; the offset `t` is normalized to `[0, 1]` and clamped, so
the end stops extend flat beyond the axis / radius. Colors are
interpolated per channel (including alpha) between adjacent stops.

```c
AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 100);
axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RGB(0x4a, 0x90, 0xd9));
axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_RGB(0x1c, 0x3f, 0x6b));
axl_gfx_fill_rect_gradient(10, 10, 200, 100, g);   /* vertical fade */
axl_gfx_gradient_free(g);
```

Geometry is in the active draw target's coordinate space, and fills
honor the clip stack + target + alpha blending like every other
primitive. Sampling is per-pixel at the pixel center; interpolation
is linear in stored (sRGB) bytes — gamma-correct interpolation is a
future refinement. Path and rounded-rect gradient fills are planned
follow-ups; the current entry point is `axl_gfx_fill_rect_gradient`.

## Clipping

`axl-gfx` maintains a 16-deep clip stack. Pushing a clip rect
**intersects** it with the current top of stack — child clips never
expand outside their parent. All draw operations (fill, blit, line,
text) respect the active clip; pixels outside it are not written.

```c
axl_gfx_push_clip((AxlGfxClip){ .x = 100, .y = 50, .w = 400, .h = 300 });
axl_gfx_draw_text(font, 90, 60, "clipped at x=100", AXL_GFX_WHITE, 1);
axl_gfx_pop_clip();
```

Clip rectangles are interpreted in the active draw target's coordinate
system: screen pixels by default, buffer-local pixels after
`axl_gfx_target_buffer(buf)`. Push clips **after** setting the target
you want them to apply to.

`axl_gfx_reset_clip()` empties the stack (useful for error recovery
or app teardown); pair `push`/`pop` for normal widget-tree operation.

### Non-axis-aligned (quad) clip

`axl_gfx_push_clip_quad(const AxlGfxPointF q[4])` pushes a convex
**quadrilateral** clip — the rotated/sheared counterpart to
`axl_gfx_push_clip`. The four corners are in device/target space (any
winding); the new region is intersected with the current top of stack,
and `axl_gfx_pop_clip` pops it like any other. Every primitive honors
it (fills, paths, text, blits). `axl_gfx_get_clip` still reports the
axis-aligned bounding box. Use it to clip content inside a rotated
panel.

### Arbitrary-shape (path) clip

`axl_gfx_push_clip_path(const AxlGfxPath *path)` clips to the filled
interior of any path — concave, self-intersecting, or multi-contour
(holed) — the foundation for CSS `clip-path`. Unlike the convex quad
clip, it rasterizes the path (even-odd) to an 8-bit coverage **mask**
over its device bounding box; subsequent draws are confined to the
shape ∩ the previous clip. Hard-edged (a pixel is kept at ≥ 50%
coverage). The path is taken in its current device coordinates (CTM not
re-applied, as with `push_clip_quad`). Pop frees the mask. Every
primitive honors it; on a screen target it falls to a per-pixel path
(compose into a back-buffer for speed).

## Transform-Aware Rendering

For toolkits that own their own coordinate model (Qt/GTK-style), the
`AxlTransform` (AxlMath, 3×3 double, row-major, cairo layout —
`x' = m[0]·x + m[1]·y + m[2]`) is the currency for rotation / shear /
non-uniform scale. Build one with the value helpers — no matrix code or
trig at the call site:

```c
AxlTransform m = axl_transform_multiply(
    axl_transform_rotate(angle),       // rotate about the origin first…
    axl_transform_translate(cx, cy));  // …then place at (cx, cy)
```

(`axl_transform_identity` / `_translate` / `_scale` / `_rotate` /
`_shear` / `_multiply` / `_invert`; `multiply(a, b)` applies `a` first,
then `b` — cairo `cairo_matrix_multiply` order. Apply one with
`axl_transform_map_point` / `_map_vector` — combine `_invert` +
`_map_point` to map a device point back to local space for
**hit-testing**.)

Two primitives consume it; both compose on top of the active graphics
transform (`axl_gfx_translate` et al.), so the effective map is
`CTM × m` (just `m` under the default identity CTM):

| What | Function | Notes |
|------|----------|-------|
| Vector text | `axl_ttf_draw_transform(font, utf8, px_size, &m, color)` | glyph outlines filled through `m`; anti-aliased at any angle. `m = translate(x, y)` reproduces `axl_ttf_draw`. Keep `axl_ttf_draw` for upright text (cached, faster). |
| Image | `axl_gfx_blit_transform(src, &m)` | `m` maps source pixels into the target; bilinear-sampled (smooth under rotation/scale), source alpha honored. |
| Clip region | `axl_gfx_push_clip_rect_transformed(rect, &m)` | clips to a rect mapped through `m` — the convex-quad clip without packing corners by hand. |

Both drawing primitives are **perspective-correct for a projective `m`**
(e.g. one from `axl_transform_quad_to_quad`): the blit inverse-maps each
pixel through the full homography, and vector text flattens its glyph
curves in local space before projecting (since a projective map sends
lines to lines).  An affine `m` takes an exact, cheaper fast path.
Geometry is assumed to stay in front of the transform's horizon (the
same precondition as `axl_transform_map_rect`).

Both honor the clip stack (including `push_clip_quad`), draw target, and
alpha — they are paradigm-agnostic: AXL never holds the matrix, the
caller hands it in per call.

## Double-Buffering

Off-screen `AxlGfxBuffer` objects let widgets composite into a
back-buffer and then atomically present the result, eliminating tearing
and partial-redraw flicker.

```c
AxlGfxBuffer *buf = axl_gfx_buffer_new(800, 600);
axl_gfx_buffer_clear(buf, AXL_GFX_BLACK);

axl_gfx_target_buffer(buf);                      // draws now go to buf
    axl_gfx_fill_rect(10, 10, 100, 100, AXL_GFX_RED);
    axl_gfx_draw_text(font, 20, 20, "Hello", AXL_GFX_WHITE, 1);
axl_gfx_target_buffer(NULL);                     // back to screen

axl_gfx_buffer_present(buf, 0, 0);               // single blit to screen
axl_gfx_buffer_free(buf);
```

`axl_gfx_buffer_present` bypasses the clip stack and current target —
it's the unconditional "swap" step. While a buffer target is active,
the clip stack and alpha compositing apply against the buffer's pixel
array using buffer-local coordinates.

### Raw fills vs. drawn fills

Every *drawing* primitive composites source-over onto a destination
treated as opaque, so the result's alpha is forced to `0xFF`. That is
right for painting, but it means `axl_gfx_fill_rect` cannot lay down a
**translucent** value — a see-through veil written through it stops
being see-through. `axl_gfx_buffer_fill_rect` is the raw counterpart:
it overwrites, so the exact pixel value lands, alpha included.

```c
/* A 50%-black veil in a per-pixel-alpha surface buffer — stays 50%. */
axl_gfx_buffer_fill_rect(veil, 0, 0, w, h, AXL_GFX_RGBA(0, 0, 0, 0x80));
```

Like the rest of the `axl_gfx_buffer_*` family it takes the buffer
explicitly and honors **no ambient graphics state** — not the clip
stack, not the blend mode, not the gamma-correct flag — and behaves the
same whether or not the buffer is the current draw target. That split is
deliberate: the `axl_gfx_*` draw family is the one that reads ambient
state. Intersect with `axl_gfx_get_clip` yourself if you want the fill
clipped. The rect is clamped to the buffer, so a negative origin or an
oversized extent is safe. `axl_gfx_buffer_clear` is the full-extent
special case, implemented on top of it.

### Present pipeline (direct framebuffer + dirty rectangles)

`axl_gfx_buffer_present` writes the back-buffer straight to the GOP
linear framebuffer (`FrameBufferBase`) when one is available, honoring
the panel's `PixelFormat` and scan-line stride. Because `AxlGfxPixel`
is stored BGRA — matching the common GOP "BGR" format — each row is a
single `memcpy`; an "RGB" panel gets a per-pixel red/blue swap. The
present path is **write-only**: it never reads VRAM (uncached MMIO
reads are catastrophically slow), so all compositing must stay in the
RAM buffer. Panels with no linear framebuffer (`PixelBitMask`,
`PixelBltOnly`, or a zero `FrameBufferBase`) transparently fall back to
GOP `Blt`. The pure conversion is exposed as `axl_gfx_pack_pixel(px,
order)` for callers writing their own framebuffer code.

For incremental redraws — a moving cursor, one updated widget — present
only what changed instead of the whole buffer:

```c
// Push just a sub-region.
axl_gfx_buffer_present_rect(buf, dst_x, dst_y, src_x, src_y, w, h);

// Or accumulate damage as you draw, then flush the union in one call.
axl_gfx_buffer_clear_damage(buf);
axl_gfx_target_buffer(buf);
    axl_gfx_fill_rect(40, 40, 16, 16, AXL_GFX_RED);
axl_gfx_target_buffer(NULL);
axl_gfx_buffer_add_damage(buf, (AxlGfxClip){40, 40, 16, 16});
axl_gfx_buffer_present_damage(buf, screen_x, screen_y);   // flushes + clears
```

`axl_gfx_buffer_add_damage` unions each dirty rect (clamped to the
buffer) into a per-buffer bounding box; `axl_gfx_buffer_present_damage`
flushes exactly that box to the screen and clears it for the next
frame. `axl_gfx_buffer_get_damage` reports the current box (empty when
clean). Presenting only the dirty region cuts present bandwidth by
10–100× for typical UI updates.

## Cursor Overlay

GOP exposes no hardware-cursor API (the GPU usually has a cursor plane,
but UEFI surfaces only a framebuffer + Blt), so a sprite that tracks the
pointer must be composited in software. `<axl/axl-cursor.h>` (`AxlCursor`) owns the
one cursor on the screen and its position. It binds to the back-buffer
"scene" being scanned out and touches only the cursor region.

The cursor is composited **into the scene as its top layer**, never
presented as a separate GOP operation — the same approach real
software-cursor compositors use (wlroots' `wlr_output_render_software_cursors`
composites the cursor as the last layer before one commit; Qt's
direct-framebuffer `QFbCursor` composites over the repainted scene and
flushes the region once). A move folds the sprite in, presents the old∪new
region **atomically** (one present, so a small move never shows a
cursor-less gap), then unfolds to keep the scene byte-clean. This avoids the
flicker you get when the cursor is erased then redrawn as two separate GOP
writes — at low present rates (e.g. a throttled 10 Hz pointer poll) that gap
is visible.

```c
AxlGfxBuffer *scene = axl_gfx_buffer_new(w, h);   // your back-buffer
AxlCursor *cur = axl_cursor_new(scene);           // built-in arrow, hidden
axl_cursor_attach(cur, loop, on_input, &app);     // tracks the pointer
// when you re-present a new frame, bracket it so the cursor folds INTO the
// flush — scene+cursor reach the screen in one present, atomically:
axl_cursor_lift(cur);                             // fold sprite into scene
axl_gfx_buffer_present(scene, 0, 0);              // one present carries both
axl_cursor_drop(cur);                             // unfold (scene clean again)
```

`axl_cursor_set_image` swaps the sprite (a NULL sprite restores the
built-in arrow); `axl_cursor_move` / `axl_cursor_move_rel` reposition the
hotspot — drive a relative pointer (`EFI_SIMPLE_POINTER`) with
`_move_rel` so the cursor clamps at a screen edge yet recovers the
instant motion reverses. A NULL `scene` selects direct-to-screen
"save-under" mode for consumers without a back-buffer. The cursor never
routes input — hit-testing stays with the consumer, or with the
compositor's seat below.

## Compositor — Surfaces, Seat, and Cursor

`<axl/axl-compositor.h>` (`AxlCompositor`) is a local, in-process
compositor: per-surface back-buffers, a z-ordered surface **tree**,
multi-surface present on the single framebuffer, and a **seat** (pointer
routing, grabs, per-surface keyboard focus, the cursor overlay). It lets
a toolkit stop faking window stacking with "reparent a child so it paints
last" and gives a reusable windowing primitive for any app that wants
stacked surfaces + input without a full widget set. The full design —
why it is deliberately Wayland-shaped — is in the `AXL-Compositor-Design`
doc.

A **surface** is a CPU-backed rectangle the client draws into, with a
position, visibility, opacity, an optional input region, and a place in a
scene-graph tree: each surface has an optional parent, a child's position
is relative to its parent, and stacking is tree pre-order (a node, then
its children on top). Raising / moving / hiding / destroying a surface
acts on its whole subtree, and so does opacity — a surface composites at
its own value times every ancestor's (`axl_surface_effective_opacity`),
so one `axl_surface_set_opacity` fades a group together. Note that this
multiplies down per surface: each still blends over whatever is already
beneath it, so mid-fade a child blends over its own already-faded parent
rather than over the parent's backdrop. It is not an offscreen group
fade; the endpoints are the same either way.

```c
AxlCompositor *c = axl_compositor_new(1280, 800);
AxlSurface *win = axl_surface_new(axl_compositor_root(c), 400, 300);
axl_surface_move(win, 100, 80);
axl_gfx_target_buffer(axl_surface_buffer(win));    // draw into the surface
axl_gfx_fill_rect(0, 0, 400, 300, AXL_GFX_RGB(0x20, 0x40, 0x80));
axl_gfx_target_buffer(NULL);
axl_surface_damage(win, (AxlGfxClip){0, 0, 400, 300});
axl_compositor_present(c);                          // composite + flush damage
```

`axl_compositor_present` composites every visible surface into one output
buffer and flushes only the damaged region to the screen — it is the
single atomicity barrier (one synchronous client, so there is no
per-surface double-buffering to manage), and it keeps the cursor on top.

The **seat** routes raw `axl-input` events. A surface installs an
`AxlSurfaceListener` — the Wayland-`wl_pointer_listener`-shaped struct of
surface-local callbacks (`enter` / `leave` / `motion` / `button` /
`axis` / `key` / `focus_in` / `focus_out`) — via
`axl_surface_set_listener`; this is the one seam a C++ toolkit bridges to
virtual methods. `axl_compositor_pointer_event` hit-tests topmost-first
(honoring the optional per-surface input region) and delivers to the
surface under the pointer. `axl_compositor_pointer_grab` confines the
pointer to a surface subtree — the menu / popup mechanism, dismissed by a
press outside it — and `axl_compositor_set_keyboard_focus` +
`axl_compositor_key_event` route keys to the focused surface. The
compositor drives an `AxlCursor` as the topmost overlay, with per-surface
cursor shapes via `axl_compositor_set_cursor_image` (a surface requests
its shape from its `enter` callback). Wire real devices with
`axl_compositor_attach_pointer` / `axl_compositor_attach_keyboard` on a
caller-owned event loop.

## Effects: Blur

`axl_gfx_buffer_blur(buf, radius)` softens an off-screen buffer in
place with a separable triangular-kernel blur (the standard
single-pass Gaussian approximation), clamp-to-edge so borders don't
darken. All four channels are blurred, **including alpha**, so it
works on an alpha/shadow mask as well as on color content. `radius 0`
is a no-op; the kernel is normalized (total intensity is preserved).

Each axis pass runs as two running-sum **box** passes rather than a
direct tent convolution: the triangular kernel of half-width `radius`
factors exactly into two box blurs, and a box blur is a sliding window
(add the entering sample, subtract the leaving one). So each pass is
**O(w·h) — independent of the radius**, and a full-screen backdrop veil
at a large radius costs no more per pixel than a small one. The
intermediate between the two boxes is carried at full integer
precision, so the output is **bit-identical** to a direct
triangular-kernel convolution — validated against an independent tent
reference in `test/integration/gfx-simd-selftest.c`, which also asserts
the cost stays flat as the radius grows.

Three further things make the constant factor small, all bit-exact
(the same tent reference guards them):

- The per-pixel round-and-divide uses an exact **multiply-shift
  reciprocal** instead of a division. `radius` is a runtime value, so
  the compiler cannot strength-reduce `/ (radius+1)²` — and that was
  four int64 divisions per output pixel, about 60% of a full-screen
  blur.
- The second axis runs as a **transpose plus another row pass**, not a
  strided column walk. Striding down a column touches one useful pixel
  per cache line, so the pass streams the whole image through the cache
  once per column; the transpose is done in cache-sized tiles and costs
  far less than the misses it removes.
- The row pass is **SIMD-dispatched** (SSE4.1 / NEON) over the four
  BGRA channels, which are exactly one 128-bit register of int32 lanes.
  Note what is *not* parallel: a running sum is serial along the pass
  axis, so — unlike a direct convolution, whose outputs are independent
  — neighbouring pixels cannot be vectorised, and AVX2 would buy
  nothing without restructuring to run several rows in lockstep. The
  rounding step uses a 32-bit reciprocal so the high half of a widening
  multiply IS the quotient; where that cannot be exact for the radius,
  or the rung is absent, the scalar pass runs instead.

Together they cut a full-screen blur to about a quarter (1280x800,
radius 12: 20.9 ms → 5.6 ms, median of 5, x64 under KVM,
`test/integration/gfx-present-bench.c`).

```c
AxlGfxBuffer *b = axl_gfx_buffer_new(w, h);   /* zero-filled */
axl_gfx_target_buffer(b);
    /* ... render a shape ... */
axl_gfx_target_buffer(NULL);
/* blur reads the WHOLE buffer, not just what you drew — which is why
   axl_gfx_buffer_new zero-fills: the halo blends against transparent
   black rather than whatever the allocator last held. */
axl_gfx_buffer_blur(b, 8);            /* CSS filter: blur(8px) */
axl_gfx_buffer_present(b, 0, 0);
axl_gfx_buffer_free(b);
```

`axl_gfx_draw_shadow(src, x, y, color, radius)` builds a soft drop
shadow on top of the blur: the shape comes from `src`'s alpha channel
(so it works for boxes, rounded rects, or anti-aliased text), tinted
with `color`, blurred by `radius`, and composited into the active
target at (x, y) — pass `x + offset_x, y + offset_y` to offset the
shadow. Draw the real content on top afterward.

```c
// `card` is a buffer with the widget rendered into it (alpha = shape)
axl_gfx_draw_shadow(card, card_x + 2, card_y + 4,
                    AXL_GFX_RGBA(0, 0, 0, 0x60), 8);  // soft offset shadow
axl_gfx_buffer_present(card, card_x, card_y);          // content on top
```

See the `AxlGfx effects <gfx.html>`_ API reference.

## Text Rendering

Text APIs take a `const AxlFont *` and UTF-8 input. Invalid UTF-8
sequences become U+FFFD REPLACEMENT CHARACTER; codepoints missing
from the font render the font's fallback glyph (when one is defined),
or skip while still advancing the pen.

```c
const AxlFont *font = axl_gfx_default_font();
uint32_t w = axl_gfx_measure_text(font, "Hello, 世界!", 1);
axl_gfx_draw_text(font, 20, 20, "Hello, 世界!", AXL_GFX_WHITE, 1);
```

`axl_gfx_measure_text` does not require GOP — useful for layout
calculations on headless systems.

### Built-in fonts

axl-gfx ships two bitmap fonts. Both are codepoint-sorted with
`fallback_codepoint = '?'` so missing glyphs render visibly.

| Font | Coverage | License | Notes |
|---|---|---|---|
| `axl_font_edk2_laffstd` | ASCII (95 glyphs) | BSD-2-Clause-Patent | EDK2 8x16 narrow font. Returned by `axl_gfx_default_font()`. |
| `axl_font_unifont_16` | 391-glyph BMP subset | SIL OFL 1.1 | GNU Unifont 16.0.04. 8-wide ASCII + 16-wide CJK / box-drawing / symbols. |

Linker garbage collection drops the fonts that aren't referenced, so
consumers pay only for the fonts they actually draw with.

For *vector* text, `axl_ttf_default()` returns a shared, built-in
DejaVu Sans subset (ASCII + Latin-1 + common typographic
punctuation) so consumers don't have to bundle a TTF asset — see the
`AxlTtf <truetype.html>`_ page. It is likewise gc-dropped when
unreferenced.

```c
AxlTtf *f = axl_ttf_default();             /* shared; do not free */
axl_ttf_draw(f, 20, 40, "Café — 21°C", 16.0f, AXL_GFX_WHITE);
```

`axl_ttf_draw` rasterizes glyphs with horizontal subpixel positioning
(quarter-pixel) and caches the results per font, keyed on
`(codepoint, px_size, subpixel offset)` with LRU eviction. The cache
is entirely internal — no API change — so repeated text redraws (the
common UI case) skip the rasterizer after the first frame. Glyphs at
whole-pixel pen positions render identically to the un-cached path.

For paragraphs, `axl_ttf_draw_box` wraps UTF-8 into a rectangle:
greedy whitespace word-wrap, hard `\n` breaks, baselines advancing by
the `axl_ttf_metrics` line-height, and horizontal alignment via
`AXL_TTF_ALIGN_{LEFT,CENTER,RIGHT}`. A word wider than the box takes
its own line and overflows (clipped to the box, which is pushed onto
the clip stack for the draw). `axl_ttf_measure_box` runs the same
layout without drawing — reporting line count plus the block width and
height — so a widget can size itself before painting.

```c
axl_ttf_draw_box(f, 20, 40, 200, 120, paragraph, 16.0f,
                 AXL_GFX_WHITE, AXL_TTF_ALIGN_LEFT);
```

### Adding a font

Bitmap fonts are generated from BDF (Glyph Bitmap Distribution Format)
sources via `scripts/gen-bdf-font.py`. The script reads a BDF file
(plus optional codepoint subset list) and emits a C source file
defining a single `AxlFont` with its `AxlGlyph[]` table — drop the
output into `src/gfx/fonts/` and add a declaration to a header your
consumer includes.

`AxlFont` itself is plain `.rodata`: codepoint-sorted glyph array,
font-wide metrics (cell width/height, ascent, descent, line height),
and a flags bitmask (`AXL_FONT_MONOSPACE` / `AXL_FONT_VARIABLE`).
Lookups use binary search; see `axl-font.h`'s design note for why
binary search beats hash/radix at the sizes typical for bitmap fonts.

Toolkits typically wrap fonts in a named-lookup / fallback-chain layer
of their own. Substrate discipline keeps that out of `axl-gfx` — the
substrate provides glyph rasterization and metrics, not text layout.
