/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-effects.h
    AxlGfx raster effects (Phase G6+): blur and (later) drop shadows,
    blend modes.  Operate on off-screen `AxlGfxBuffer`s.  Pulled in via
    the <axl/axl-gfx.h> umbrella.

    @code
    AxlGfxBuffer *b = axl_gfx_buffer_new(w, h);
    // ... render into b ...
    axl_gfx_buffer_blur(b, 8);          // soften it
    axl_gfx_buffer_present(b, 0, 0);    // blit to screen
    @endcode
**/

#ifndef AXL_GFX_EFFECTS_H
#define AXL_GFX_EFFECTS_H

#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-gfx-surface.h>   /* AxlGfxBuffer */

#ifdef __cplusplus
extern "C" {
#endif

/// Blur an off-screen buffer in place (stack blur — a fast Gaussian
/// approximation that is O(1) per pixel regardless of @a radius, run
/// as a separable horizontal + vertical pass).
///
/// All four channels are blurred, **including alpha**, so this works
/// directly on a shadow/alpha mask as well as on color content.
/// Edges use clamp-to-edge sampling (no darkening at the border).
///
/// @a radius is the blur reach in pixels each way; the effective
/// kernel width is `2*radius + 1`. A @a radius of 0 is a no-op
/// (returns AXL_OK). Radii larger than the buffer are clamped to fit.
///
/// Operates purely on the buffer's pixel array — does NOT require GOP
/// and ignores the active draw target / clip stack.
///
/// @return AXL_OK on success (including the radius-0 no-op).  AXL_ERR
///         if @a buf is NULL or the temporary scratch allocation
///         fails.
int
axl_gfx_buffer_blur(
    AxlGfxBuffer  *buf,     ///< buffer to blur in place
    uint32_t       radius   ///< blur radius in pixels (0 = no-op)
    );

/// Draw a soft drop shadow of @a src into the active draw target.
///
/// The shadow's SHAPE comes from @a src's alpha channel (so it works
/// for any rendered content — a box, rounded rect, or anti-aliased
/// text). It is tinted with @a color (use a translucent color for a
/// subtle shadow), blurred by @a radius, and composited at
/// (@a x, @a y) — the position @a src's top-left would occupy, so to
/// offset the shadow pass `x + offset_x, y + offset_y`. The caller
/// then draws the real content on top.
///
/// The shadow softly extends up to @a radius pixels beyond @a src's
/// bounds. Honors the active draw target, clip stack, and alpha
/// blending (each shadow pixel composites source-over). @a radius 0
/// gives a hard (un-blurred) tinted silhouette.
///
/// @return AXL_OK on success.  AXL_ERR if @a src is NULL, a temporary
///         buffer allocation fails, or the active target is the screen
///         and GOP is unavailable.
int
axl_gfx_draw_shadow(
    const AxlGfxBuffer  *src,    ///< [in] shape whose alpha is the shadow mask
    int32_t              x,      ///< target x for src's top-left (bake offset in)
    int32_t              y,      ///< target y for src's top-left
    AxlGfxPixel          color,  ///< shadow tint (alpha scales the shadow)
    uint32_t             radius  ///< blur radius in pixels
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_GFX_EFFECTS_H */
