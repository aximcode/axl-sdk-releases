/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-surface.h
    AxlGfx drawing surface + module-global state: GOP availability,
    off-screen buffers (double-buffering), the clip stack, and the
    affine transform stack (Phase G4).  Pulled in via the
    <axl/axl-gfx.h> umbrella.
**/

#ifndef AXL_GFX_SURFACE_H
#define AXL_GFX_SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-math.h>
#include <axl/axl-gfx-types.h>

#ifdef __cplusplus
extern "C" {
#endif

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
// Display modes (GOP QueryMode / SetMode — boot-services only)
// ===================================================================

/// Number of display modes the GOP enumerates.
///
/// @return mode count, or 0 if headless / no GOP.
uint32_t
axl_gfx_mode_count(void);

/// Query the geometry of mode @a index without switching to it.
///
/// @return AXL_OK on success, AXL_ERR if no GOP, @a index out of range
///         (>= axl_gfx_mode_count()), @a out is NULL, or QueryMode failed.
int
axl_gfx_query_mode(
    uint32_t     index,  ///< mode number in [0, axl_gfx_mode_count())
    AxlGfxMode  *out     ///< [out] receives the mode geometry
    );

/// The currently-active mode index (its geometry matches axl_gfx_get_info()).
///
/// @return AXL_OK + *@a out_index set, or AXL_ERR if no GOP / @a out_index NULL.
int
axl_gfx_current_mode(
    uint32_t  *out_index  ///< [out] receives the active mode number
    );

/// Find the first enumerated mode matching @a width x @a height.
///
/// @return AXL_OK + *@a out_index set, or AXL_ERR if no GOP, no matching
///         mode, or @a out_index is NULL.
int
axl_gfx_find_mode(
    uint32_t   width,      ///< desired horizontal resolution
    uint32_t   height,     ///< desired vertical resolution
    uint32_t  *out_index   ///< [out] receives the matching mode number
    );

/// Find the largest enumerated mode — greatest pixel area, ties broken by
/// the wider mode.  The "use the full screen" pick (its `index` feeds
/// `axl_gfx_set_mode`).
///
/// @return AXL_OK + *@a out, or AXL_ERR if no GOP, no modes, or @a out NULL.
int
axl_gfx_max_mode(
    AxlGfxMode  *out   ///< [out] receives the largest mode
    );

/// Switch the display to mode @a index.
///
/// The framebuffer is reallocated — axl_gfx_get_info() reflects the new
/// geometry afterward and any cached FrameBufferBase is stale — and the
/// screen is cleared, so the caller MUST repaint.  Boot-services only
/// (the GOP is gone after ExitBootServices).
///
/// @return AXL_OK on success, AXL_ERR if no GOP, @a index out of range,
///         or SetMode failed.
int
axl_gfx_set_mode(
    uint32_t  index  ///< mode number in [0, axl_gfx_mode_count())
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

/// Present a sub-rectangle of @a buf to the screen (Phase G18).
///
/// The dirty-rectangle counterpart to `axl_gfx_buffer_present`: copies
/// only the `@a w × @a h` region whose top-left lies at (@a src_x,
/// @a src_y) in the buffer to screen position (@a dst_x, @a dst_y).
/// Presenting just the changed region cuts present bandwidth by 10–100×
/// for incremental redraws (a moving cursor, one updated widget).
/// `axl_gfx_buffer_present(buf, x, y)` is exactly this over the whole
/// buffer.
///
/// The source region is clamped to the buffer extent: a region that
/// starts at or past the buffer edge presents nothing (still AXL_OK).
/// Like `axl_gfx_buffer_present`, this bypasses the clip stack and the
/// active draw target — it always writes directly to the screen.
///
/// @return AXL_OK on success (including the fully-clamped no-op).
///         AXL_ERR if GOP is not available or @a buf is NULL.
int
axl_gfx_buffer_present_rect(
    const AxlGfxBuffer  *buf,    ///< source buffer
    uint32_t             dst_x,  ///< screen x of the region's top-left
    uint32_t             dst_y,  ///< screen y of the region's top-left
    uint32_t             src_x,  ///< buffer x of the region's top-left
    uint32_t             src_y,  ///< buffer y of the region's top-left
    uint32_t             w,      ///< region width in pixels
    uint32_t             h       ///< region height in pixels
    );

/// Blit a source buffer into the active draw target through a transform.
///
/// The transform-aware counterpart to `axl_gfx_blit` — for rotated,
/// sheared, or non-uniformly scaled images.  The transform @a m maps
/// **source pixel coordinates** (the rectangle `[0, src->w] ×
/// [0, src->h]`) into the draw target: e.g.
/// `axl_transform_translate(x, y)` places the image upright at
/// `(x, y)`, and `axl_transform_multiply(axl_transform_rotate(a),
/// axl_transform_translate(x,y))` rotates it about its top-left corner
/// (rotate first, then translate — cairo order).
///
/// Each covered destination pixel is inverse-mapped back into the
/// source and **bilinearly sampled** (smooth under rotation / scale).
/// Destination pixels whose sample falls outside the source rectangle
/// are left untouched.  The source alpha is honored: fully transparent
/// source texels contribute nothing; partial alpha blends source-over
/// on buffer targets (matching `axl_gfx_fill_rect`).
///
/// The active graphics transform (`axl_gfx_translate` et al.) composes
/// on top: the effective mapping is `CTM × m`.  Honors the active clip
/// stack (including `axl_gfx_push_clip_quad`) and draw target.  A
/// singular (zero-area) @a m draws nothing.
///
/// @return AXL_OK on success (including the nothing-drawn cases).
///         AXL_ERR if @a src or @a m is NULL, or the active target is
///         the screen and GOP is unavailable.
int
axl_gfx_blit_transform(
    const AxlGfxBuffer  *src,   ///< [in] source image
    const AxlTransform  *m      ///< [in] source-pixel → target transform
    );

// ===================================================================
// Pattern fill (Phase G12) — tiled/repeated buffer fill
// ===================================================================

/// How an `axl_gfx_fill_pattern` source tiles across the destination
/// rectangle.  Mirrors CSS `background-repeat`.
typedef enum {
    AXL_GFX_REPEAT_BOTH = 0,  ///< tile on both axes (CSS `repeat`)
    AXL_GFX_REPEAT_X,         ///< tile horizontally only (CSS `repeat-x`)
    AXL_GFX_REPEAT_Y,         ///< tile vertically only (CSS `repeat-y`)
    AXL_GFX_REPEAT_NONE,      ///< a single copy at the origin (CSS `no-repeat`)
} AxlGfxRepeat;

/// Fill the rectangle (@a x, @a y, @a w, @a h) by tiling @a pattern.
///
/// The substrate for textured backgrounds, CSS `repeating-linear-
/// gradient` (pre-render one period into a buffer, then repeat it), and
/// nine-slice button art.  The pattern is **anchored at the rect's
/// top-left** (@a x, @a y) — texel (0,0) lands there — and repeats per
/// @a repeat: `BOTH` tiles in x and y; `X`/`Y` tile one axis and draw a
/// single band on the other (rows/columns past the pattern extent are
/// left untouched); `NONE` draws one copy at the origin.
///
/// Signed origins (like `axl_gfx_fill_rect_i`) so a pattern can start
/// off the top-left edge.  Honors the active **clip** stack and draw
/// **target**, and composites each texel with the active **blend mode**
/// and the texel's alpha — fully-transparent texels (alpha == 0) leave
/// the destination untouched, so patterns with holes show through.
/// A zero-size fill (@a w <= 0 or @a h <= 0) is a documented no-op.
///
/// @return AXL_OK on success (including the no-op).  AXL_ERR if
///         @a pattern is NULL or has zero dimensions, or the active
///         target is the screen and GOP is unavailable.
int
axl_gfx_fill_pattern(
    int32_t              x,        ///< destination left (signed)
    int32_t              y,        ///< destination top (signed)
    int32_t              w,        ///< destination width (<= 0 = no-op)
    int32_t              h,        ///< destination height (<= 0 = no-op)
    const AxlGfxBuffer  *pattern,  ///< [in] tile source (anchored at x,y)
    AxlGfxRepeat         repeat    ///< tiling mode
    );

// ===================================================================
// Blend mode — module-global compositing state
// ===================================================================

/// Set the active blend mode for subsequent drawing.
///
/// Graphics-driver state (like the draw target and clip stack), default
/// `AXL_GFX_BLEND_OVER` (normal source-over).  Every primitive that
/// composites onto a **buffer target** — fills (rect / rounded / path /
/// gradient), lines, text (bitmap + vector), and `axl_gfx_blit_transform`
/// — honors it, including otherwise-opaque draws (an opaque source under
/// `AXL_GFX_BLEND_MULTIPLY` still multiplies).  The raw-copy
/// `axl_gfx_blit` is exempt (it overwrites, it does not composite).
/// Screen (GOP) targets cannot composite and ignore non-default modes,
/// the same limitation as translucent alpha.
///
/// Save/restore via `axl_gfx_get_blend_mode` when nesting; reset to
/// `AXL_GFX_BLEND_OVER` for normal drawing.
void
axl_gfx_set_blend_mode(
    AxlGfxBlendMode  mode   ///< blend function for subsequent draws
    );

/// Get the active blend mode (for save/restore around a nested draw).
AxlGfxBlendMode
axl_gfx_get_blend_mode(void);

/// Reset the blend mode to `AXL_GFX_BLEND_OVER` (the default).  The
/// recovery counterpart to `axl_gfx_reset_clip` / `axl_gfx_reset_
/// transform`, for a teardown / error path that must restore normal
/// compositing without tracking the prior mode.
void
axl_gfx_reset_blend_mode(void);

// ===================================================================
// Gamma-correct (linear-light) compositing (Phase G15)
// ===================================================================

/// Enable or disable gamma-correct (linear-light) compositing.
///
/// Module-global compositing state, like the blend mode — **off by
/// default** (plain sRGB compositing, matching Cairo/Blend2D defaults and
/// costing nothing).  When enabled, every alpha/coverage composite onto a
/// **buffer target** decodes the source and destination colors from sRGB
/// to linear light, blends in linear, and re-encodes — the physically
/// correct way to mix light.
///
/// What it fixes: the "dark fringe" on anti-aliased text and path edges
/// (partial-coverage pixels are no longer composited too dark, so thin
/// text keeps its weight and edges stay clean), and the brightness of
/// translucent overlays / fades / shadows.  Costs two small lookup tables
/// (built lazily on first use) plus a decode/encode per blended channel,
/// so it is opt-in for consumers that want the quality.
///
/// Scope: the **source-over alpha composite** is gamma-correct for all
/// modes, and gradient color ramps interpolate in linear light too (see
/// `axl_gfx_gradient_sample`).  Still sRGB-only: the separable PDF blend
/// *functions* (multiply/screen/…).  Opaque draws are unaffected (no
/// blending happens).  Screen (GOP) targets composite in sRGB regardless,
/// the same limitation as translucent alpha.
///
/// Save/restore via `axl_gfx_get_gamma_correct` when nesting.
void
axl_gfx_set_gamma_correct(
    bool  enable   ///< true = linear-light compositing; false = sRGB (default)
    );

/// Get the active gamma-correct compositing flag (for save/restore).
bool
axl_gfx_get_gamma_correct(void);

/// Reset gamma-correct compositing to off (the default) — the recovery
/// counterpart to `axl_gfx_reset_blend_mode`.
void
axl_gfx_reset_gamma_correct(void);

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

/// Push a convex-quadrilateral clip region onto the clip stack.
///
/// The lean counterpart to `axl_gfx_push_clip` for callers whose clip
/// is not axis-aligned — e.g. a widget toolkit that has rotated/sheared
/// its content and needs the clip rotated to match.  The four corners
/// @a q are in the **active draw target's coordinate space** (device /
/// buffer-local pixels, the same space as `AxlGfxClip` and the rest of
/// the clip stack) — pass the four already-transformed corners; the
/// active transform (`axl_gfx_translate` et al.) is **not** re-applied,
/// matching the device-space convention of `axl_gfx_push_clip`.
///
/// The new active clip is the **intersection** of the quad with the
/// previous top of stack (same nesting semantics as
/// `axl_gfx_push_clip`).  Subsequent draws are clipped to the quad: a
/// pixel is kept when its center lies inside all four edges.  Edges are
/// hard (no anti-aliasing on the clip boundary), matching the existing
/// rectangular clip.  Pair every push with a matching
/// `axl_gfx_pop_clip`.
///
/// The corners may be given in either winding order (clockwise or
/// counter-clockwise) and may be partly or fully outside the target.
/// The region MUST be convex — the four transformed corners of a
/// rectangle always are.  A degenerate (zero-area) quad clips
/// everything (empty region).  `axl_gfx_get_clip` continues to report
/// the **axis-aligned bounding box** of the active clip (a conservative
/// rect), since its output type is a rectangle.
///
/// @return AXL_OK on success, AXL_ERR if @a q is NULL or the stack is
///         full (depth > AXL_GFX_CLIP_STACK_MAX).
int
axl_gfx_push_clip_quad(
    const AxlGfxPointF  q[4]   ///< [in] four device-space corners (any winding)
    );

/// Push a clip = axis-aligned rect @a r mapped through transform @a m
/// (and the active CTM), as a convex quad.
///
/// The convenience form of `axl_gfx_push_clip_quad` for the common case
/// of clipping to a transformed rectangle: it maps the four corners of
/// @a r through `CTM × m` (the same effective mapping the transform-aware
/// drawing primitives use) and pushes the resulting device-space quad.
/// An affine @a m yields a parallelogram, a projective @a m a general
/// convex quad — both valid convex clip regions (a rect whose projective
/// image crosses the horizon is out of contract, as for
/// `axl_transform_map_rect`).
///
/// @return AXL_OK on success; AXL_ERR if @a m is NULL or the clip stack
///         is full.
int
axl_gfx_push_clip_rect_transformed(
    AxlRect             r,    ///< [in] rect in pre-transform (local) space
    const AxlTransform  *m    ///< [in] local → target transform
    );

typedef struct AxlGfxPath AxlGfxPath;  ///< fwd-decl (full def in axl-gfx-path.h)

/// Push a clip = the filled interior of @a path, intersected with the
/// current clip — arbitrary (concave, self-intersecting, multi-contour /
/// holed) masking, the foundation for CSS `clip-path`.
///
/// Unlike `axl_gfx_push_clip_quad` (convex only), this rasterizes @a path
/// to a coverage mask, so any shape works.  The path is taken in its
/// **current device-space coordinates** (built under whatever transform
/// was active; the CTM is not re-applied here, matching push_clip_quad),
/// and filled with the **even-odd** rule (same as `axl_gfx_fill_path`).
/// The clip is hard-edged: a pixel is kept when its center is >= 50%
/// covered (no anti-aliased clip boundary, matching the quad clip).
///
/// Subsequent draws are confined to the path ∩ the previous clip.  Pair
/// every push with `axl_gfx_pop_clip`.  `axl_gfx_get_clip` reports the
/// mask's axis-aligned bounding box (a conservative rect).
///
/// @return AXL_OK on success; AXL_ERR if @a path is NULL or has fewer
///         than 3 vertices (can't enclose area), the clip stack is full,
///         or the mask allocation fails.
int
axl_gfx_push_clip_path(
    const AxlGfxPath  *path   ///< [in] shape to clip to (device-space)
    );

/// Pop the top clip off the stack.
///
/// Restores the previous clip (or "no clipping" if the stack is now
/// empty).  Pops both rectangular (`axl_gfx_push_clip`) and quad
/// (`axl_gfx_push_clip_quad`) pushes — the stack is uniform.
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
// Damage tracking (Phase G18) — per-buffer dirty-region accumulator
// ===================================================================
//
// A buffer carries an optional damage bounding box: the union of the
// regions a frame's draws touched.  A widget toolkit unions in each
// dirty rect as it redraws, then flushes only that bbox to the screen,
// turning a full-screen present into a single small region copy.  The
// damage rect is in buffer-local pixel coordinates (the same space as
// the buffer's draw operations).  A fresh buffer starts un-damaged.

/// Union @a rect into @a buf's damage bounding box.
///
/// Accumulates the dirty region for the next `axl_gfx_buffer_present_
/// damage`.  The stored damage is the axis-aligned bbox of every rect
/// added since the last clear, clamped to the buffer extent.  Portions
/// of @a rect outside the buffer (including negative origins) are
/// clipped off; a fully-out-of-bounds or empty (`w == 0 || h == 0`)
/// rect contributes nothing.
///
/// @return AXL_OK on success, AXL_ERR if @a buf is NULL.
int
axl_gfx_buffer_add_damage(
    AxlGfxBuffer  *buf,    ///< buffer to mark dirty
    AxlGfxClip     rect    ///< dirty region in buffer-local coords
    );

/// Get @a buf's current damage bounding box.
///
/// @return AXL_OK with @a out filled if the buffer has accumulated
///         damage; AXL_ERR if @a buf or @a out is NULL, or the buffer
///         is currently un-damaged (nothing to present).
int
axl_gfx_buffer_get_damage(
    const AxlGfxBuffer  *buf,   ///< [in] buffer
    AxlGfxClip          *out    ///< [out] damage bbox (buffer-local)
    );

/// Clear @a buf's accumulated damage (mark it fully clean).
///
/// @return AXL_OK on success, AXL_ERR if @a buf is NULL.
int
axl_gfx_buffer_clear_damage(
    AxlGfxBuffer  *buf    ///< buffer to reset
    );

/// Present @a buf's accumulated damage region to the screen, then clear
/// it (the "swap" step for incremental redraw).
///
/// Flushes exactly the damage bbox via `axl_gfx_buffer_present_rect`,
/// mapping the buffer-local damage origin to screen position (@a dst_x +
/// damage.x, @a dst_y + damage.y), then resets the damage so the next
/// frame starts clean.  With no damage accumulated this is a no-op
/// (still AXL_OK) — nothing changed, nothing to present.
///
/// @return AXL_OK on success (including the no-damage no-op).  AXL_ERR
///         if GOP is not available or @a buf is NULL.
int
axl_gfx_buffer_present_damage(
    AxlGfxBuffer  *buf,    ///< source buffer (its damage is flushed + cleared)
    uint32_t       dst_x,  ///< screen x for the buffer's local origin
    uint32_t       dst_y   ///< screen y for the buffer's local origin
    );

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
/// `axl_transform_rotate` for the underlying matrix.
void
axl_gfx_rotate(
    double  radians
    );

/// Append a 2D shear to the current transform.
///
/// `sx` shears x as a function of y; `sy` shears y as a function
/// of x.  See `axl_transform_shear` for the convention.
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
/// back to local-space via `axl_transform_map_point` against the
/// inverse (when AxlMath ships matrix inverse — see the M-phase
/// roadmap).
AxlTransform
axl_gfx_get_transform(void);

/// Reset the transform to identity and clear the save stack.
///
/// Recovery primitive — pair push/pop for normal operation.
void
axl_gfx_reset_transform(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_SURFACE_H */
