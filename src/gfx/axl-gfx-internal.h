/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-internal.h
    Intra-module private helpers shared between axl-gfx.c and the
    other gfx-module source files (axl-gfx-path.c etc.).

    NOT exported — kept inside src/gfx/.  Consumers go through the
    public <axl/axl-gfx.h> surface.
**/

#ifndef AXL_GFX_INTERNAL_H
#define AXL_GFX_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-math.h>
#include <axl/axl-gfx-types.h>
#include <uefi/axl-uefi.h>   /* EFI_GRAPHICS_PIXEL_FORMAT */

/* Opaque public types referenced below (full defs in the public
 * headers / axl-gfx-path.c). */
typedef struct AxlGfxPath      AxlGfxPath;
typedef struct AxlGfxGradient  AxlGfxGradient;

/// Get the current top of the gfx-module transform stack.  Returns
/// identity if the stack is empty.  Called by path / line primitives
/// to transform incoming vertices.
AxlTransform
axl_gfx_internal_current_transform(void);

/// Map a GOP pixel format to the public AxlGfxPixelFormat.  Returns
/// false for a value outside the spec's four formats (malformed
/// firmware).  Defined in axl-gfx-output.c (the rasterizer-free GOP
/// inventory TU) and shared with the active-GOP accessors in axl-gfx.c.
bool
axl_gfx_internal_map_pixel_format(
    EFI_GRAPHICS_PIXEL_FORMAT  in,
    AxlGfxPixelFormat         *out
    );

// ===================================================================
// Analytic path rasterizer (G14 — FreeType ftgrays backend)
// ===================================================================

/// A single path vertex in world (pixel) space.  @a is_move starts a
/// new contour; otherwise the vertex extends the current contour with
/// a line segment.  Layout shared between axl-gfx-path.c (which builds
/// the list) and axl-gfx-rasterize.c (which rasterizes it).
typedef struct {
    float  x, y;
    bool   is_move;
} AxlGfxVertex;

/// Per-span sink invoked by `axl_gfx_rasterize_fill` for each
/// rasterized horizontal run: fill @a len pixels starting at
/// (@a x, @a y) at 8-bit @a coverage (0 = transparent, 255 = opaque).
typedef void (*AxlGfxSpanSink)(
    int32_t  y,
    int32_t  x,
    int32_t  len,
    uint8_t  coverage,
    void    *user
    );

/// Analytic anti-aliased fill of the closed contours described by
/// @a verts (count @a n), emitting exact-coverage spans to @a sink.
/// Contours are implicitly closed.  @a even_odd selects the even-odd
/// fill rule; false selects non-zero winding.  Backed by FreeType's
/// ftgrays rasterizer.
///
/// @return AXL_OK on success, AXL_ERR on bad args or rasterizer
///         failure (e.g. allocation failure on a very complex path).
int
axl_gfx_rasterize_fill(
    const AxlGfxVertex  *verts,
    size_t               n,
    bool                 even_odd,
    AxlGfxSpanSink       sink,
    void                *user
    );

// ===================================================================
// Shared fill output — used by both fill (axl-gfx-path.c) and stroke
// (axl-gfx-stroke.c)
// ===================================================================

/// Span-sink context: solid @a color, or per-pixel @a grad sampling
/// when grad != NULL.  Passed as the @a user of `axl_gfx_internal_fill
/// _span`.
typedef struct {
    AxlGfxPixel            color;
    const AxlGfxGradient  *grad;
} AxlGfxFillSink;

/// `AxlGfxSpanSink` that modulates the source alpha (solid @a color or
/// per-pixel @a grad sample) by the 8-bit coverage and blits via
/// `axl_gfx_fill_rect_i` (which applies the clip stack, draw target,
/// and alpha blend).  @a user must point to an `AxlGfxFillSink`.
void
axl_gfx_internal_fill_span(
    int32_t  y,
    int32_t  x,
    int32_t  len,
    uint8_t  coverage,
    void    *user
    );

/// Return a path's world-space vertex array, writing the vertex count
/// to @a out_n.  Lets the stroker read path geometry without the
/// `AxlGfxPath` struct definition.  Returns NULL (with *out_n == 0)
/// for a NULL or empty path.
const AxlGfxVertex *
axl_gfx_internal_path_verts(
    const AxlGfxPath  *p,
    size_t            *out_n
    );

#endif /* AXL_GFX_INTERNAL_H */
