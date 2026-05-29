/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx.h
    Basic graphics output (GOP framebuffer operations).

    Provides a thin abstraction over the UEFI Graphics Output Protocol
    for framebuffer rendering: fill rectangles, blit pixel buffers, and
    capture screen regions.  Falls back gracefully on headless systems
    where GOP is not available.

    @code
    if (axl_gfx_available()) {
        AxlGfxInfo info;
        axl_gfx_get_info(&info);
        printf("Display: %ux%u\n", info.width, info.height);

        axl_gfx_fill_rect(100, 100, 200, 150, AXL_GFX_RED);
    }
    @endcode
**/

#ifndef AXL_GFX_H
#define AXL_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-font.h>
#include <axl/axl-macros.h>
#include <axl/axl-math.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Types
// ===================================================================

/// Framebuffer information (standard C types, no UEFI types).
typedef struct {
    uint32_t  width;       ///< horizontal resolution in pixels
    uint32_t  height;      ///< vertical resolution in pixels
    uint32_t  stride;      ///< pixels per scan line (>= width)
    uint64_t  framebuffer; ///< physical address (0 if BltOnly mode)
} AxlGfxInfo;

/// Pixel color in BGRA layout (matches GOP native BGRX pixel format
/// for the BGR bytes; the 4th byte is alpha for blending operations).
///
/// alpha = 0xFF means fully opaque; alpha = 0 means fully transparent.
/// When passed to drawing primitives:
///   - alpha == 0xFF: fast opaque draw (existing behavior; for screen
///     targets uses GOP Blt directly).
///   - alpha == 0: no-op (fully transparent).
///   - 0 < alpha < 0xFF: source-over blend against the destination's
///     existing pixels.  Only supported on buffer targets (alpha-aware
///     drawing on screen targets falls back to opaque, since GOP has
///     no blending hardware — render to a back-buffer first if you
///     want semi-transparent overlays on screen).
typedef struct {
    uint8_t  blue;
    uint8_t  green;
    uint8_t  red;
    uint8_t  alpha;     ///< 0xFF = opaque, 0 = transparent
} AxlGfxPixel;

/// Source-over alpha composite: blend a source pixel over a destination.
/// Result alpha is always 0xFF (destination is treated as opaque).
/// Math (8-bit integer): out.rgb = (src.rgb * a + dst.rgb * (255 - a) + 127) / 255
/// where a = src.alpha.
AxlGfxPixel
axl_gfx_blend(
    AxlGfxPixel  dst,    ///< destination pixel (existing)
    AxlGfxPixel  src     ///< source pixel (with alpha)
    );

// -------------------------------------------------------------------
// Convenience: RGB(A) literal macros + named color palette
// -------------------------------------------------------------------
// AxlGfxPixel storage is BGRA to match the GOP framebuffer byte order
// exactly (zero conversion at present time).  These macros let callers
// write colors in the more familiar RGB notation — e.g. CSS #FF6347
// for tomato becomes AXL_GFX_RGB(0xFF, 0x63, 0x47) — without paying
// any per-pixel byte-swap cost at runtime: the macros expand to a
// compound literal with the bytes already in BGRA order.

#define AXL_GFX_RGB(r, g, b)        ((AxlGfxPixel){(b), (g), (r), 0xFF})
#define AXL_GFX_RGBA(r, g, b, a)    ((AxlGfxPixel){(b), (g), (r), (a)})

// Common named colors — all opaque.  Add more as consumers ask for them.
#define AXL_GFX_BLACK      AXL_GFX_RGB(0x00, 0x00, 0x00)
#define AXL_GFX_WHITE      AXL_GFX_RGB(0xFF, 0xFF, 0xFF)
#define AXL_GFX_RED        AXL_GFX_RGB(0xFF, 0x00, 0x00)
#define AXL_GFX_GREEN      AXL_GFX_RGB(0x00, 0xFF, 0x00)
#define AXL_GFX_BLUE       AXL_GFX_RGB(0x00, 0x00, 0xFF)
#define AXL_GFX_YELLOW     AXL_GFX_RGB(0xFF, 0xFF, 0x00)
#define AXL_GFX_CYAN       AXL_GFX_RGB(0x00, 0xFF, 0xFF)
#define AXL_GFX_MAGENTA    AXL_GFX_RGB(0xFF, 0x00, 0xFF)
#define AXL_GFX_GRAY       AXL_GFX_RGB(0x80, 0x80, 0x80)
#define AXL_GFX_TRANSPARENT AXL_GFX_RGBA(0x00, 0x00, 0x00, 0x00)

// ===================================================================
// Availability
// ===================================================================

/// Check whether a graphics display is available.
///
/// @return true if GOP was found, false on headless/serial systems.
bool
axl_gfx_available(void);

/// Get framebuffer information.
///
/// @return AXL_OK on success, AXL_ERR if GOP not available.
int
axl_gfx_get_info(
    AxlGfxInfo  *info  ///< [out] receives display info
    );

// ===================================================================
// Off-screen buffers (double-buffered rendering)
// ===================================================================

/// Opaque off-screen pixel buffer.  Holds a w*h array of AxlGfxPixel
/// in row-major order plus dimensions.  Used as a render target for
/// flicker-free double-buffered rendering: widgets composite into the
/// buffer, then `axl_gfx_buffer_present` blits the full buffer to the
/// screen in one operation.
typedef struct AxlGfxBuffer AxlGfxBuffer;

/// Allocate an off-screen buffer.  Pixels are uninitialized — call
/// axl_gfx_buffer_clear before first use if you want a known background.
///
/// @return new buffer (caller frees with axl_gfx_buffer_free), or NULL
///         on allocation failure or invalid dimensions (w == 0 or h == 0).
AxlGfxBuffer *
axl_gfx_buffer_new(
    uint32_t  w,    ///< width in pixels (must be > 0)
    uint32_t  h     ///< height in pixels (must be > 0)
    );

/// Free an off-screen buffer.  Safe to call with NULL.
void
axl_gfx_buffer_free(
    AxlGfxBuffer  *buf    ///< buffer to free, or NULL
    );

/// Get buffer dimensions.
///
/// @return AXL_OK on success, AXL_ERR if @a buf is NULL.
int
axl_gfx_buffer_get_info(
    const AxlGfxBuffer  *buf,   ///< [in] buffer
    uint32_t            *out_w, ///< [out] width (NULL OK to skip)
    uint32_t            *out_h  ///< [out] height (NULL OK to skip)
    );

/// Fill the entire buffer with @a color.
///
/// @return AXL_OK on success, AXL_ERR if @a buf is NULL.
int
axl_gfx_buffer_clear(
    AxlGfxBuffer  *buf,    ///< buffer to clear
    AxlGfxPixel    color   ///< fill color
    );

/// Direct access to the buffer's pixel array (row-major, no padding).
/// Pointer is stable for the buffer's lifetime.  Length is w*h pixels.
///
/// @return pointer to pixel array, or NULL if @a buf is NULL.
AxlGfxPixel *
axl_gfx_buffer_pixels(
    AxlGfxBuffer  *buf
    );

/// Make subsequent drawing operations target @a buf instead of the
/// screen.  Pass NULL to switch back to screen rendering (the default).
///
/// While a buffer target is active, fill_rect / blit / draw_text write
/// into the buffer's pixel array using buffer-local coordinates.
/// Clipping (push_clip / pop_clip) is honored against the same
/// buffer-local coordinate system.
///
/// State persists until changed.  Pair every set with an eventual
/// reset to NULL to avoid leaking the target across unrelated frames.
void
axl_gfx_target_buffer(
    AxlGfxBuffer  *buf    ///< buffer to target, or NULL for screen
    );

/// Query the buffer currently set as the draw target.
///
/// Paradigm-agnostic primitive intended for callers that need to
/// nest target changes — capture the current target, switch to
/// their own buffer, then restore the captured value on exit.
/// Without this query, a caller swapping in NULL on exit would
/// stomp on an outer caller that expected its buffer to stay
/// active.
///
/// @return the currently-targeted buffer, or NULL if rendering
///         targets the screen (the default).
AxlGfxBuffer *
axl_gfx_get_current_target(void);

/// Present (blit) a buffer to the screen at (@a dst_x, @a dst_y).
/// Bypasses the clip stack and the current draw target — always blits
/// directly via GOP.  This is the "swap" step of double-buffering.
///
/// @return AXL_OK on success, AXL_ERR if GOP is not available or
///         @a buf is NULL.
int
axl_gfx_buffer_present(
    const AxlGfxBuffer  *buf,    ///< source buffer
    uint32_t             dst_x,  ///< screen x position
    uint32_t             dst_y   ///< screen y position
    );

// ===================================================================
// Clipping
// ===================================================================

/// Clipping rectangle.  Signed origins so off-screen / negative
/// positions are expressible (useful when widgets scroll partly out
/// of view).  Width/height are unsigned — an empty clip is encoded as
/// w == 0 or h == 0.
typedef struct {
    int32_t   x;       ///< left edge in pixels
    int32_t   y;       ///< top edge in pixels
    uint32_t  w;       ///< width in pixels (0 = empty)
    uint32_t  h;       ///< height in pixels (0 = empty)
} AxlGfxClip;

/// Maximum nested clip-stack depth.  Deep enough for any realistic
/// widget tree; small enough to live in static module state.
#define AXL_GFX_CLIP_STACK_MAX  16

/// Push a clipping rectangle onto the clip stack.
///
/// The new active clip is the **intersection** of @a rect with the
/// previous top of stack (or @a rect itself if the stack was empty).
/// Subsequent draw operations are clamped to the active clip; pixels
/// outside it are not written.  Empty intersections are valid (all
/// draws no-op until pop or reset).
///
/// Pair every push with a matching pop.  Stacking enables widget
/// trees where each level adds its own clip on top of its parent's.
///
/// **Coordinate space:** clip rectangles are interpreted in the active
/// draw target's coordinate system.  With no target set (screen
/// rendering), coordinates are screen pixels.  After
/// `axl_gfx_target_buffer(buf)`, coordinates are buffer-local.
/// Push clips AFTER setting the target you want them to apply to.
///
/// @return AXL_OK on success, AXL_ERR if the stack is full
///         (depth > AXL_GFX_CLIP_STACK_MAX).
int
axl_gfx_push_clip(
    AxlGfxClip  rect   ///< clip rectangle to intersect onto the stack
    );

/// Pop the top clip off the stack.
///
/// Restores the previous clip (or "no clipping" if the stack is now
/// empty).
///
/// @return AXL_OK on success, AXL_ERR if the stack was already empty.
int
axl_gfx_pop_clip(void);

/// Get the current effective clip rectangle.
///
/// @return AXL_OK with @a out filled if a clip is active; AXL_ERR if
///         no clip is on the stack (drawing is full-screen) OR if
///         @a out is NULL.
int
axl_gfx_get_clip(
    AxlGfxClip  *out   ///< [out] receives the active clip rect
    );

/// Reset the clip stack to empty (no clipping).
///
/// Useful as a recovery primitive when an error path made stack depth
/// unrecoverable, or at app teardown.  Widget toolkits should pair
/// push/pop rather than reset for normal operation.
void
axl_gfx_reset_clip(void);

// ===================================================================
// Transform stack — module-global graphics-driver state.
// Phase G4.  Composes affine 2D transforms.  Substrate for CSS
// `transform`, AGT hover-scale, animation rotation.
// ===================================================================

/// Maximum nested transform-stack depth (parallel to the clip stack).
#define AXL_GFX_TRANSFORM_STACK_MAX  16

/// Append a translation to the current transform.
///
/// Composition matches HTML canvas: after `axl_gfx_translate(10, 20)`
/// the local origin draws at world `(10, 20)`.  Subsequent translates
/// add: `translate(10, 0); translate(5, 0)` is equivalent to
/// `translate(15, 0)`.
///
/// Affects only the drawing primitives that honor the transform —
/// see "Transform-aware primitives" below.
void
axl_gfx_translate(
    double  tx,
    double  ty
    );

/// Append a non-uniform scale to the current transform.
///
/// `axl_gfx_scale(2, 2)` doubles everything drawn after the call.
/// `axl_gfx_scale(1, -1)` flips the y-axis.
void
axl_gfx_scale(
    double  sx,
    double  sy
    );

/// Append a rotation (in radians) to the current transform.
///
/// Positive radians rotate counter-clockwise in math y-up coords;
/// clockwise in screen y-down (the axl-gfx default).  See
/// `axl_mat3_rotate` for the underlying matrix.
void
axl_gfx_rotate(
    double  radians
    );

/// Append a 2D shear to the current transform.
///
/// `sx` shears x as a function of y; `sy` shears y as a function
/// of x.  See `axl_mat3_skew` for the convention.
void
axl_gfx_skew(
    double  sx,
    double  sy
    );

/// Save the current transform onto the stack.
///
/// Pair every push with a matching `axl_gfx_pop_transform`.  Up to
/// `AXL_GFX_TRANSFORM_STACK_MAX` levels of nesting.
///
/// @return AXL_OK on success, AXL_ERR if the stack is full.
int
axl_gfx_push_transform(void);

/// Restore the previously-saved transform.
///
/// @return AXL_OK on success, AXL_ERR if the stack was empty.
int
axl_gfx_pop_transform(void);

/// Get the current active transform as a 3×3 matrix.
///
/// Useful for converting hit-test coordinates from screen-space
/// back to local-space via `axl_mat3_transform_point` against the
/// inverse (when AxlMath ships matrix inverse — see the M-phase
/// roadmap).
AxlMat3
axl_gfx_get_transform(void);

/// Reset the transform to identity and clear the save stack.
///
/// Recovery primitive — pair push/pop for normal operation.
void
axl_gfx_reset_transform(void);

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
/// the active draw target via their fractional coverage (4x4
/// supersampled in the current implementation).  Honors the
/// active clip stack and draw target; alpha-blends on buffer
/// targets where supported.
///
/// @return AXL_OK on success.  AXL_ERR if @a p is NULL, the path
///         is empty, or the active target is the screen and GOP
///         is unavailable.
int
axl_gfx_fill_path(
    const AxlGfxPath  *p,      ///< [in] path to fill
    AxlGfxPixel        color   ///< fill color
    );

/// Stroke the outline of @a p with @a color and width @a w.
///
/// Width is honored as a 1-pixel-thick edge regardless of @a w
/// in the current implementation (proper thick-line rendering
/// awaits a future batch).  Honors clip + draw target.
///
/// @return AXL_OK on success.  AXL_ERR if @a p is NULL or the
///         active target is the screen and GOP is unavailable.
int
axl_gfx_stroke_path(
    const AxlGfxPath  *p,      ///< [in] path to stroke
    AxlGfxPixel        color,  ///< stroke color
    float              w       ///< stroke width in pixels (currently 1)
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_H */
