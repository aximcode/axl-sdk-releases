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
| Path stroke | `axl_gfx_stroke_path` | 1-px line segments along the path |
| Blit | `axl_gfx_blit` | Copy a pixel buffer onto the target |
| Capture | `axl_gfx_capture` | Read screen region into a buffer |
| Text | `axl_gfx_draw_text` | UTF-8, bitmap-font-driven, integer-scaled |
| Text (vector) | `axl_ttf_draw` | UTF-8, TTF/OTF, fractional `px_size`; see `AxlTtf <truetype.html>`_ |

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
