/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-draw.h
    AxlGfx immediate-mode drawing primitives (fill/line/rect/blit/
    capture, AxlGfxPoint polylines) and bitmap-font text rendering.
    Pulled in via the <axl/axl-gfx.h> umbrella.
**/

#ifndef AXL_GFX_DRAW_H
#define AXL_GFX_DRAW_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-gfx-types.h>
#include <axl/axl-font.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Drawing
// ===================================================================
//
// Phase G4 — transform-aware primitives:
//   - Path API (`axl_gfx_path_*`, `axl_gfx_fill_path`,
//     `axl_gfx_stroke_path`) — full affine transform applied at
//     vertex insertion time.  Translate, scale, rotate, and skew
//     all work.
//
// NOT yet transform-aware (Phase G4 scope deferred to a follow-up):
//   - `axl_gfx_fill_rect`, `axl_gfx_fill_rect_i`, `axl_gfx_draw_rect`,
//     `axl_gfx_draw_line`, `axl_gfx_blit`, `axl_gfx_fill_rounded_rect`.
//   - These render in raw target coordinates regardless of the
//     active transform.  Consumers that need transformed rect /
//     line drawing should build a path (`axl_gfx_path_move_to` +
//     `_line_to` + `_close`) and fill / stroke it instead.
//

/// Fill a rectangle with a solid color.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_fill_rect(
    uint32_t     x,      ///< left edge
    uint32_t     y,      ///< top edge
    uint32_t     w,      ///< width in pixels
    uint32_t     h,      ///< height in pixels
    AxlGfxPixel  color   ///< fill color
    );

/// Fill a rectangle with a solid color, signed-coord variant.
///
/// Identical semantics to `axl_gfx_fill_rect` except (@a x, @a y)
/// may be negative — useful for widgets that partly scroll off the
/// top or left edge.  The visible portion is the intersection of
/// the requested rect with (a) the active draw target's bounds and
/// (b) the active clip stack.  Width / height are signed too so
/// `w < 0` or `h < 0` is a no-op (returns AXL_OK) — matches the
/// zero-dim lenience of the unsigned variant.
///
/// Equivalent to: clamp negatives in caller code, then call the
/// unsigned `axl_gfx_fill_rect` — but with the boundary arithmetic
/// inside the library, callers don't have to repeat the four-line
/// dance every time a widget might scroll partly off-screen.
///
/// @return AXL_OK on success (including the no-op cases), AXL_ERR
///         if GOP not available on a screen target.
int
axl_gfx_fill_rect_i(
    int32_t      x,      ///< left edge (may be negative)
    int32_t      y,      ///< top edge (may be negative)
    int32_t      w,      ///< width in pixels (<=0 = no-op)
    int32_t      h,      ///< height in pixels (<=0 = no-op)
    AxlGfxPixel  color   ///< fill color
    );

/// Blit a pixel buffer to the screen.
///
/// @a buffer must contain at least @a w * @a h pixels in row-major
/// order with AxlGfxPixel (BGRX) layout.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_blit(
    const AxlGfxPixel  *buffer,  ///< [in] source pixel buffer
    uint32_t            x,       ///< destination left edge
    uint32_t            y,       ///< destination top edge
    uint32_t            w,       ///< width in pixels
    uint32_t            h        ///< height in pixels
    );

/// Draw a 1-pixel-wide line from (@a x0, @a y0) to (@a x1, @a y1).
///
/// Uses Bresenham's line algorithm.  Signed origins so partly off-screen
/// lines are expressible.  Honors the active clip and (for buffer
/// targets) source alpha.  Endpoints are inclusive — both (x0,y0)
/// and (x1,y1) get written.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available (screen target only).
int
axl_gfx_draw_line(
    int32_t      x0,    ///< start x (signed; off-screen origins OK)
    int32_t      y0,    ///< start y
    int32_t      x1,    ///< end x (inclusive)
    int32_t      y1,    ///< end y (inclusive)
    AxlGfxPixel  color  ///< line color (alpha honored on buffer targets)
    );

/// Draw a 1-pixel-wide rectangle outline.
///
/// Equivalent to four 1-pixel-thick `axl_gfx_fill_rect` calls — top,
/// bottom, left, right edges.  Interior pixels are not touched.  For
/// w==1 or h==1, the four edges degenerate sensibly (still draws the
/// 1-wide column or row).  w==0 or h==0 is a documented no-op
/// (matches `axl_gfx_fill_rect`'s lenience).
///
/// @return AXL_OK on success (including the zero-dim no-op),
///         AXL_ERR if GOP not available on a screen target.
int
axl_gfx_draw_rect(
    uint32_t     x,     ///< left edge
    uint32_t     y,     ///< top edge
    uint32_t     w,     ///< width (0 = no-op)
    uint32_t     h,     ///< height (0 = no-op)
    AxlGfxPixel  color  ///< outline color
    );

/// 2D integer point for polyline / shape APIs.
typedef struct {
    int32_t  x;
    int32_t  y;
} AxlGfxPoint;

/// Draw connected 1-pixel-wide line segments through @a count points.
///
/// Equivalent to (count - 1) `axl_gfx_draw_line` calls between
/// consecutive points.  Points may have signed (off-screen) coordinates.
///
/// @return AXL_OK on success, AXL_ERR if @a points is NULL or
///         @a count < 2.
int
axl_gfx_draw_polyline(
    const AxlGfxPoint  *points,  ///< [in] array of point vertices
    size_t              count,   ///< number of points (>= 2)
    AxlGfxPixel         color    ///< line color
    );

/// Capture a screen region into a pixel buffer.
///
/// @a buffer must have space for at least @a w * @a h pixels.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_capture(
    AxlGfxPixel  *buffer,  ///< [out] destination pixel buffer
    uint32_t      x,       ///< source left edge
    uint32_t      y,       ///< source top edge
    uint32_t      w,       ///< width in pixels
    uint32_t      h        ///< height in pixels
    );

// ===================================================================
// Text rendering
// ===================================================================

/// Get the default built-in font.
///
/// Returns a pointer to the font axl-gfx ships with — currently the
/// EDK2 LaffStd 8x16 narrow font.  Stable across calls (returns the
/// same pointer; never NULL).  Suitable as the @a font argument to
/// the text APIs when the caller has no preference.
///
/// @return pointer to the default font (static, never NULL).
const AxlFont *
axl_gfx_default_font(void);

/// Compute the rendered pixel width of a text string.
///
/// @a text is UTF-8.  Each codepoint contributes its per-glyph advance
/// (or the font's cell_width for monospace / missing glyphs).  Invalid
/// UTF-8 bytes are treated as U+FFFD REPLACEMENT CHARACTER.  Does not
/// require GOP.
///
/// @return rendered width in pixels, or 0 if any argument is invalid
///         (@a font NULL, @a text NULL, or @a scale 0).
uint32_t
axl_gfx_measure_text(
    const AxlFont  *font,   ///< [in] font atlas to measure with
    const char     *text,   ///< [in] UTF-8 text to measure
    uint32_t        scale   ///< scale factor (1 = native, 2 = doubled, etc.)
    );

/// Draw a UTF-8 text string at the given position.
///
/// Decodes @a text as UTF-8 and renders each codepoint's glyph from
/// @a font.  Codepoints absent from the font render the font's
/// fallback glyph (if any) or skip while still advancing the pen.
/// Invalid UTF-8 sequences become U+FFFD REPLACEMENT CHARACTER.
/// Output is clamped to screen bounds.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available, @a font
///         or @a text is NULL, or @a scale is 0.
int
axl_gfx_draw_text(
    const AxlFont  *font,    ///< [in] font atlas
    uint32_t        x,       ///< left edge (pixels)
    uint32_t        y,       ///< top edge (pixels)
    const char     *text,    ///< UTF-8 text to render
    AxlGfxPixel     color,   ///< text foreground color
    uint32_t        scale    ///< scale factor (1 = native, 2 = doubled, etc.)
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_DRAW_H */
