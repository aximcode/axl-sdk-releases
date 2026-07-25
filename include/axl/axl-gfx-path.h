/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-path.h
    AxlGfx retained-mode paths (Phase G3): build with move_to/line_to/
    curve_to/arc/close, then fill (even-odd, anti-aliased), stroke, or
    fill_rounded_rect.  Pulled in via the <axl/axl-gfx.h> umbrella.
**/

#ifndef AXL_GFX_PATH_H
#define AXL_GFX_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-gfx-types.h>
#include <axl/axl-gfx-gradient.h>   /* gradient-fill variants below */

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Paths (G3 — retained-mode path objects + scanline fill)
// ===================================================================

/// Opaque retained path — sequence of line segments and subpaths.
///
/// Built incrementally with move_to / line_to / curve_to / arc /
/// close.  Curves and arcs are flattened to line segments at
/// insertion time so fill / stroke operations walk a uniform
/// segment list.  Filled or stroked with `axl_gfx_fill_path` /
/// `axl_gfx_stroke_path`; the path object is reusable across
/// many fills (caller frees with `axl_gfx_path_free`).
///
/// Retained shape (not immediate-mode) is deliberate per the
/// AGT recording-fixture design — capturing a path pointer keeps
/// trace entries cheap, where capturing a points array would
/// require deep-copy + UAF risk.
typedef struct AxlGfxPath AxlGfxPath;

/// Allocate an empty path.
///
/// @return new path (caller frees with `axl_gfx_path_free`), or
///         NULL on allocation failure.
AxlGfxPath *
axl_gfx_path_new(void);

/// Free a path allocated with `axl_gfx_path_new`.
///
/// Safe to call with NULL.
void
axl_gfx_path_free(
    AxlGfxPath  *p   ///< path to free, or NULL
    );

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlGfxPath, axl_gfx_path_free)
#endif

/// Clear all segments from @a p, retaining the allocation for reuse.
///
/// After reset the path is empty (no subpaths, no current pen
/// position).  Subsequent `axl_gfx_path_move_to` starts a new
/// subpath as if @a p were freshly allocated.  Cheaper than
/// free + new for paths that are rebuilt every frame.
void
axl_gfx_path_reset(
    AxlGfxPath  *p   ///< path to reset
    );

/// Start a new subpath at (@a x, @a y).
///
/// Moves the current pen position without emitting a line segment.
/// Multiple subpaths within one path are filled together using the
/// even-odd fill rule (subpath intersections invert).
void
axl_gfx_path_move_to(
    AxlGfxPath  *p,   ///< path
    float        x,   ///< pen x
    float        y    ///< pen y
    );

/// Add a line segment from the current pen position to (@a x, @a y).
///
/// If no subpath is open (no prior `move_to` since last reset / new),
/// the line implicitly starts a subpath at (@a x, @a y) (i.e., the
/// initial point becomes both start and current — no segment yet).
void
axl_gfx_path_line_to(
    AxlGfxPath  *p,   ///< path
    float        x,   ///< end x
    float        y    ///< end y
    );

/// Add a cubic Bezier curve from the current pen to (@a x, @a y).
///
/// Control points (@a c1x, @a c1y) and (@a c2x, @a c2y) shape the
/// curve.  The implementation flattens to line segments using
/// recursive de Casteljau subdivision; segment density scales with
/// the curve's deviation from straight.  After insertion the pen
/// is at (@a x, @a y).
void
axl_gfx_path_curve_to(
    AxlGfxPath  *p,    ///< path
    float        c1x,  ///< first control point x
    float        c1y,  ///< first control point y
    float        c2x,  ///< second control point x
    float        c2y,  ///< second control point y
    float        x,    ///< end x
    float        y     ///< end y
    );

/// Add a circular arc to the path.
///
/// Center (@a cx, @a cy), radius @a r, sweeping counterclockwise
/// from @a start_rad to @a end_rad.  If there's already an open
/// subpath, the implementation emits a `line_to` to the arc's
/// starting point (start_rad on the circle) before tracing the arc.
/// After insertion the pen is at the arc's end point.
///
/// Arc segment density is chosen so the chord-to-arc deviation is
/// sub-pixel at typical UI scales.
void
axl_gfx_path_arc(
    AxlGfxPath  *p,          ///< path
    float        cx,         ///< center x
    float        cy,         ///< center y
    float        r,          ///< radius
    float        start_rad,  ///< start angle (radians)
    float        end_rad     ///< end angle (radians, > start_rad)
    );

/// Close the current subpath by adding a line from the current pen
/// to the subpath's starting point.
///
/// No-op if no subpath is open.  Calling `axl_gfx_path_move_to`
/// after close starts a new subpath.
void
axl_gfx_path_close(
    AxlGfxPath  *p   ///< path
    );

/// Fill the area enclosed by @a p with @a color.
///
/// Uses even-odd fill rule for nested subpaths.  Edges are anti-
/// aliased: pixels straddling the path boundary are blended into
/// the active draw target via their exact fractional coverage
/// (computed by an analytic scanline rasterizer).  Honors the
/// active clip stack and draw target; alpha-blends on buffer
/// targets where supported.  Open subpaths are implicitly closed
/// for filling.
///
/// @return AXL_OK on success.  AXL_ERR if @a p is NULL, the path
///         is empty, or the active target is the screen and GOP
///         is unavailable.
int
axl_gfx_fill_path(
    const AxlGfxPath  *p,      ///< [in] path to fill
    AxlGfxPixel        color   ///< fill color
    );

/// Fill @a p with a gradient instead of a solid color.
///
/// Identical rasterization to `axl_gfx_fill_path` (even-odd,
/// anti-aliased, honors clip + draw target), except each covered
/// pixel takes its color from @a g sampled at that pixel
/// (`axl_gfx_gradient_sample`) modulated by edge coverage. A gradient
/// with no stops paints nothing.
///
/// @return AXL_OK on success.  AXL_ERR if @a p is NULL / empty, @a g
///         is NULL, or the active target is the screen and GOP is
///         unavailable.
int
axl_gfx_fill_path_gradient(
    const AxlGfxPath      *p,   ///< [in] path to fill
    const AxlGfxGradient  *g    ///< [in] gradient to sample
    );

/// Line-cap style for the open ends of a stroked subpath.
typedef enum {
    AXL_GFX_CAP_BUTT   = 0,  ///< flush square end exactly at the endpoint (default)
    AXL_GFX_CAP_ROUND  = 1,  ///< semicircular end, radius = width/2
    AXL_GFX_CAP_SQUARE = 2,  ///< square end projecting width/2 past the endpoint
} AxlGfxLineCap;

/// Line-join style for the corners between stroked segments.
typedef enum {
    AXL_GFX_JOIN_MITER = 0,  ///< sharp projected corner, clamped by miter_limit (default)
    AXL_GFX_JOIN_ROUND = 1,  ///< rounded corner, radius = width/2
    AXL_GFX_JOIN_BEVEL = 2,  ///< flat chamfered corner
} AxlGfxLineJoin;

/// Stroke styling for `axl_gfx_stroke_path_ex`.  A zero-initialized
/// value is a valid CSS-style default (butt caps, miter joins) once
/// @a width is set.
typedef struct {
    float           width;        ///< stroke width in pixels (<= 0: no-op)
    AxlGfxLineCap   cap;          ///< end-cap style (open subpaths + dash ends)
    AxlGfxLineJoin  join;         ///< corner style between segments
    float           miter_limit;  ///< max miterLength/strokeWidth = 1/sin(θ/2) (SVG/Canvas); <= 0 → default 10
    const float    *dashes;       ///< on/off dash lengths in pixels, alternating on,off,…; NULL = solid
    size_t          n_dashes;     ///< number of entries in @a dashes (0 = solid; odd repeats to even, SVG-style)
    float           dash_offset;  ///< phase: distance into the pattern at each subpath start
} AxlGfxStrokeStyle;

/// Stroke the outline of @a p with @a color and explicit @a style.
///
/// The stroke is anti-aliased (shares the path fill rasterizer) and
/// honors width, caps (butt / round / square), and joins (miter with
/// @a miter_limit / round / bevel).  Miters that exceed the limit fall
/// back to a bevel.  Closed subpaths are joined all round (no caps);
/// open subpaths are capped at both ends.
///
/// When @a style->dashes is non-NULL with @a n_dashes > 0, each
/// subpath is split into on/off intervals by the dash pattern (each
/// "on" interval is stroked as its own capped open piece, honoring the
/// @a cap style at its ends).  @a dash_offset shifts the pattern start.
/// An empty / all-zero / negative pattern (or one with no element long
/// enough to advance) strokes solid.  On a closed subpath the dashes at
/// the start/end seam are not merged (each is capped) — a minor
/// fidelity gap vs SVG; the FreeType backend handles it.
///
/// Honors the active clip stack and draw target.  @a style->width <= 0
/// is a no-op success.
///
/// @return AXL_OK on success (including the no-op width <= 0 case).
///         AXL_ERR if @a p or @a style is NULL, on allocation failure,
///         or the active target is the screen and GOP is unavailable.
int
axl_gfx_stroke_path_ex(
    const AxlGfxPath         *p,      ///< [in] path to stroke
    AxlGfxPixel               color,  ///< stroke color
    const AxlGfxStrokeStyle  *style   ///< [in] cap / join / width / miter
    );

/// Stroke @a p with @a color and width @a w — convenience wrapper
/// over `axl_gfx_stroke_path_ex` with the default style (butt caps,
/// miter joins, miter limit 10).
///
/// @return AXL_OK on success (including the no-op @a w <= 0 case).
///         AXL_ERR if @a p is NULL, on allocation failure, or the
///         active target is the screen and GOP is unavailable.
int
axl_gfx_stroke_path(
    const AxlGfxPath  *p,      ///< [in] path to stroke
    AxlGfxPixel        color,  ///< stroke color
    float              w       ///< stroke width in pixels
    );

/// Fill a rectangle with rounded corners — immediate-mode helper.
///
/// Internally builds a transient path, fills it, and frees.  Saves
/// callers from path-lifecycle management for the case that
/// dominates widget rendering (button + panel backgrounds).
///
/// @a radius is clamped to `min(w, h) / 2`.  A radius of 0 produces
/// a plain rectangle (equivalent to `axl_gfx_fill_rect_i`).  Honors
/// clip + draw target.
///
/// @return AXL_OK on success.  AXL_ERR if @a w / @a h are
///         non-positive (no-op success path also returns AXL_OK
///         matching the unsigned-rect lenience) or the active
///         target is the screen and GOP is unavailable.
int
axl_gfx_fill_rounded_rect(
    int32_t      x,        ///< left edge (may be negative)
    int32_t      y,        ///< top edge (may be negative)
    int32_t      w,        ///< width in pixels (<= 0: no-op)
    int32_t      h,        ///< height in pixels (<= 0: no-op)
    float        radius,   ///< corner radius (clamped to min(w,h)/2)
    AxlGfxPixel  color     ///< fill color
    );

/// Fill a rounded rectangle with a gradient instead of a solid color.
///
/// Same geometry/clamping/AA as `axl_gfx_fill_rounded_rect`; each
/// pixel (straight bands and anti-aliased corners alike) takes its
/// color from @a g sampled at that pixel. A gradient with no stops
/// paints nothing.
///
/// @return AXL_OK on success (including the @a w/@a h <= 0 no-op).
///         AXL_ERR if @a g is NULL or the active target is the screen
///         and GOP is unavailable.
int
axl_gfx_fill_rounded_rect_gradient(
    int32_t                x,        ///< left edge (may be negative)
    int32_t                y,        ///< top edge (may be negative)
    int32_t                w,        ///< width in pixels (<= 0: no-op)
    int32_t                h,        ///< height in pixels (<= 0: no-op)
    float                  radius,   ///< corner radius (clamped to min(w,h)/2)
    const AxlGfxGradient  *g         ///< [in] gradient to sample
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_PATH_H */
