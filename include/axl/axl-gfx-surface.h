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

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_SURFACE_H */
