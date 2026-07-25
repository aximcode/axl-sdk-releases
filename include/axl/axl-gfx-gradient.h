/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-gradient.h
    AxlGfx gradients (Phase G5) — linear and radial color gradients
    for filling rectangles, paths, and rounded rects.  Table stakes
    for modern UI: button backgrounds, hero panels, CSS
    `linear-gradient` / `radial-gradient`.  Pulled in via the
    <axl/axl-gfx.h> umbrella.

    @code
    AxlGfxGradient *g = axl_gfx_gradient_linear_new(0, 0, 0, 100);
    axl_gfx_gradient_add_stop(g, 0.0f, AXL_GFX_RGB(0x4a, 0x90, 0xd9));
    axl_gfx_gradient_add_stop(g, 1.0f, AXL_GFX_RGB(0x1c, 0x3f, 0x6b));
    axl_gfx_fill_rect_gradient(10, 10, 200, 100, g);
    axl_gfx_gradient_free(g);
    @endcode
**/

#ifndef AXL_GFX_GRADIENT_H
#define AXL_GFX_GRADIENT_H

#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-gfx-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum color stops per gradient.  Deep enough for any realistic
/// UI gradient; small enough to live inline in the gradient object
/// with no per-stop allocation.
#define AXL_GFX_GRADIENT_MAX_STOPS  16

/// Opaque gradient object.  Holds the gradient geometry (the axis for
/// linear, the center + radius for radial) plus a color-stop list.
/// Built with `axl_gfx_gradient_linear_new` / `_radial_new` +
/// `axl_gfx_gradient_add_stop`; consumed by the `_gradient` fill
/// variants; freed with `axl_gfx_gradient_free`.  Reusable across
/// many fills.
///
/// Geometry coordinates are in the active draw target's coordinate
/// system — the same space the fill rectangle / path uses.
typedef struct AxlGfxGradient AxlGfxGradient;

/// Create a linear gradient whose color axis runs from (@a x0, @a y0)
/// to (@a x1, @a y1).
///
/// A filled pixel's color is chosen by projecting the pixel onto this
/// axis: the offset is 0 at (@a x0, @a y0), 1 at (@a x1, @a y1), and
/// is clamped to [0, 1] beyond the endpoints (so the end stops extend
/// flat).  A zero-length axis (both endpoints equal) paints the whole
/// region with the first stop's color.
///
/// Coordinates are floats in the active draw target's space.
///
/// @return new gradient (caller frees with `axl_gfx_gradient_free`),
///         or NULL on allocation failure.
AxlGfxGradient *
axl_gfx_gradient_linear_new(
    float  x0,    ///< axis start x
    float  y0,    ///< axis start y
    float  x1,    ///< axis end x
    float  y1     ///< axis end y
    );

/// Create a radial gradient centered at (@a cx, @a cy) with radius
/// @a radius.
///
/// A filled pixel's color is chosen by its distance from the center:
/// offset 0 at the center, 1 at @a radius, clamped to [0, 1] beyond
/// (the last stop extends flat outside the circle).  A non-positive
/// @a radius paints the whole region with the last stop's color.
///
/// @return new gradient (caller frees with `axl_gfx_gradient_free`),
///         or NULL on allocation failure.
AxlGfxGradient *
axl_gfx_gradient_radial_new(
    float  cx,      ///< center x
    float  cy,      ///< center y
    float  radius   ///< radius in pixels (offset 1 at this distance)
    );

/// Add a color stop at normalized offset @a t (clamped to [0, 1]).
///
/// Stops may be added in any order; the gradient keeps them sorted by
/// offset internally.  Between two adjacent stops the color is
/// linearly interpolated per channel (including alpha).  Before the
/// first stop the first stop's color applies; after the last stop the
/// last stop's color applies.
///
/// @return AXL_OK on success.  AXL_ERR if @a g is NULL or the stop
///         list is already full (`AXL_GFX_GRADIENT_MAX_STOPS`).
int
axl_gfx_gradient_add_stop(
    AxlGfxGradient  *g,      ///< gradient to add to
    float            t,      ///< offset in [0, 1] (clamped)
    AxlGfxPixel      color   ///< stop color (alpha interpolated too)
    );

/// Free a gradient created with `axl_gfx_gradient_*_new`.
///
/// Safe to call with NULL.
void
axl_gfx_gradient_free(
    AxlGfxGradient  *g   ///< gradient to free, or NULL
    );

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlGfxGradient, axl_gfx_gradient_free)
#endif

/// Sample @a g at the center of pixel (@a x, @a y).
///
/// Returns the interpolated color the fill helpers would paint at that
/// pixel (before coverage/AA is applied). The offset is computed the
/// same way as the fill variants — axis projection for linear,
/// distance/radius for radial, clamped to [0, 1]. Useful for custom
/// painting and as the per-pixel source for path / rounded-rect fills.
///
/// Honors `axl_gfx_set_gamma_correct`: when on, the color ramp between
/// stops is interpolated in **linear light** (perceptually even, no dark
/// dip between colors); alpha always interpolates plainly (it's
/// coverage, not a light value).
///
/// @return the sampled `AxlGfxPixel`; fully-transparent (alpha 0) if
///         @a g is NULL or has no stops.
AxlGfxPixel
axl_gfx_gradient_sample(
    const AxlGfxGradient  *g,   ///< [in] gradient to sample
    int32_t                x,   ///< pixel x
    int32_t                y    ///< pixel y
    );

/// Fill a rectangle with @a g, signed-coord variant.
///
/// Geometry mirrors `axl_gfx_fill_rect_i`: (@a x, @a y) may be
/// negative, `w <= 0` or `h <= 0` is a no-op success, and the visible
/// region is the intersection of the rect with the active draw target
/// and clip stack.  Each filled pixel samples @a g at its position;
/// stop colors with alpha < 0xFF source-over blend on buffer targets.
///
/// A gradient with no stops fills nothing (AXL_OK, no-op).
///
/// @return AXL_OK on success (including the no-op cases).  AXL_ERR if
///         @a g is NULL, or the active target is the screen and GOP
///         is unavailable.
int
axl_gfx_fill_rect_gradient(
    int32_t                x,   ///< left edge (may be negative)
    int32_t                y,   ///< top edge (may be negative)
    int32_t                w,   ///< width in pixels (<= 0: no-op)
    int32_t                h,   ///< height in pixels (<= 0: no-op)
    const AxlGfxGradient  *g    ///< [in] gradient to sample
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_GRADIENT_H */
