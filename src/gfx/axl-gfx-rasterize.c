/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-rasterize.c
    AxlGfx analytic path rasterizer (G14) — FreeType ftgrays backend.

    Wraps FreeType's `ftgrays.c` (the reference "FreeType-smooth"
    anti-aliased rasterizer) in `STANDALONE_` mode to compute exact
    fractional pixel coverage for filled paths, replacing the previous
    4x4 supersampled even-odd sampler.  ftgrays is vendored under
    `deps/freetype/` (FreeType License — FTL; see deps/freetype/FTL.TXT
    and THIRD_PARTY.md).

    Integration shape (decided by spike `spike/g14-rasterizer`,
    docs/spikes/2026-05-29-G14-rasterizer.md): the standalone ftgrays
    needs only `malloc`/`free`/`memset` externally — routed to axl via
    the shim below — with no setjmp, no libm, no qsort.  We marshal our
    `AxlGfxVertex` list into an `FT_Outline` (26.6 fixed point) and run
    the rasterizer in DIRECT mode, so it streams coverage spans to our
    callback instead of materializing a bitmap.
**/

#include <stddef.h>
#include <stdint.h>

#include "axl-gfx-internal.h"
#include <axl/axl-macros.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

// ===================================================================
// ftgrays STANDALONE_ shim
// ===================================================================
//
// Definitions the newer ftgrays.c expects that its STANDALONE_ branch
// does not self-provide.  `malloc`/`free` are routed to axl via the
// FT_QNEW_ARRAY / FT_FREE macros (ftgrays calls no allocator directly);
// `memset` resolves to our freestanding intrinsic (src/mem/
// axl-intrinsics.c) through <string.h> (include/compat/string.h).

#define FALL_THROUGH  ((void)0)

typedef unsigned long  FT_ULong;
typedef signed long    FT_Long;
typedef int            FT_Error;
typedef void          *FT_Memory;

/* Allocate `cnt` elements of `*ptr`; evaluate to non-zero (and set the
 * enclosing `error` to the overflow code) on failure, matching how
 * ftgrays uses `if ( FT_QNEW_ARRAY( ... ) )`.
 *
 * The `cnt * sizeof` is overflow-checked: ftgrays derives `cnt` (the
 * pool `estimate`) from clip-box spans, and a known upstream typo in
 * that estimate (see the AXL fix in ftgrays.c gray_raster_render) can
 * drive it negative → huge unsigned. Real FreeType's allocator is
 * size-checked and fails such a request gracefully; this shim must do
 * the same, or the wrapped `size_t` product yields a too-small buffer
 * and ftgrays writes past it. With both fixes a huge request fails
 * cleanly (Smooth_Err_Raster_Overflow) instead of corrupting memory. */
#define FT_QNEW_ARRAY(ptr, cnt)                                        \
    ( ( (size_t)(cnt) > SIZE_MAX / sizeof(*(ptr)) ||                   \
        ( (ptr) = axl_malloc((size_t)(cnt) * sizeof(*(ptr))) ) == NULL ) \
      ? ( error = Smooth_Err_Raster_Overflow, (ptr) = NULL, 1 )        \
      : 0 )
#define FT_FREE(ptr)  ( axl_free(ptr), (ptr) = NULL )

#define STANDALONE_
/* ftgrays is distributed as a single .c compiled in STANDALONE_ mode
 * by #including it here (FreeType's documented integration path), the
 * same vendoring approach as stb_truetype in axl-truetype.c. */
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../deps/freetype/ftgrays.c"

// ===================================================================
// Marshalling AxlGfxVertex list -> FT_Outline, direct-mode render
// ===================================================================

/* Vertex count at/below which the outline arrays are stack-allocated
 * (most UI paths); larger paths fall back to one heap allocation set.
 * Keeps the common fill path allocation-free, as the old sampler was. */
#define RASTER_STACK_VERTS  256u

typedef struct {
    AxlGfxSpanSink  sink;
    void           *user;
} RasterCtx;

/* FT direct-mode span callback: forward each FT_Span run to our sink. */
static void
ft_span_trampoline(
    int             y,
    int             count,
    const FT_Span  *spans,
    void           *user
    )
{
    const RasterCtx *ctx = (const RasterCtx *)user;
    for (int i = 0; i < count; i++) {
        ctx->sink((int32_t)y,
                  (int32_t)spans[i].x,
                  (int32_t)spans[i].len,
                  spans[i].coverage,
                  ctx->user);
    }
}

int
axl_gfx_rasterize_fill(
    const AxlGfxVertex  *verts,
    size_t               n,
    bool                 even_odd,
    AxlGfxSpanSink       sink,
    void                *user
    )
{
    if (!verts || n < 2 || !sink) {
        return AXL_ERR;
    }

    /* Contours: index 0 and every is_move vertex start one.  FT_Outline
     * counts are unsigned short, so cap at 65535 points. */
    size_t n_contours = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == 0 || verts[i].is_move) {
            n_contours++;
        }
    }
    if (n > 0xFFFFu || n_contours == 0) {
        return AXL_ERR;
    }

    FT_Vector       pt_stack[RASTER_STACK_VERTS];
    unsigned char   tag_stack[RASTER_STACK_VERTS];
    unsigned short  ct_stack[RASTER_STACK_VERTS];

    bool             heap     = n > RASTER_STACK_VERTS;
    FT_Vector       *points   = heap ? axl_malloc(n * sizeof *points)   : pt_stack;
    unsigned char   *tags     = heap ? axl_malloc(n * sizeof *tags)     : tag_stack;
    unsigned short  *contours = heap ? axl_malloc(n_contours * sizeof *contours)
                                     : ct_stack;
    if (!points || !tags || !contours) {
        if (heap) {
            axl_free(points);
            axl_free(tags);
            axl_free(contours);
        }
        return AXL_ERR;
    }

    /* World coords are fed y-down (our screen row convention); ftgrays
     * reports span y in the same space, so no vertical flip is needed.
     * Inside/outside under both even-odd and non-zero winding is
     * invariant under a global y-sign flip, so y-down is safe. */
    size_t c = 0;
    float  min_x = verts[0].x, max_x = verts[0].x;
    float  min_y = verts[0].y, max_y = verts[0].y;
    for (size_t i = 0; i < n; i++) {
        float vx = verts[i].x, vy = verts[i].y;
        points[i].x = (FT_Pos)axl_floori((double)vx * 64.0 + 0.5);
        points[i].y = (FT_Pos)axl_floori((double)vy * 64.0 + 0.5);
        tags[i]     = FT_CURVE_TAG_ON;
        if (i > 0 && verts[i].is_move) {
            contours[c++] = (unsigned short)(i - 1);
        }
        if (vx < min_x) min_x = vx;
        if (vx > max_x) max_x = vx;
        if (vy < min_y) min_y = vy;
        if (vy > max_y) max_y = vy;
    }
    contours[c] = (unsigned short)(n - 1);   /* close the last contour */

    FT_Outline outline;
    outline.n_contours = (unsigned short)n_contours;
    outline.n_points   = (unsigned short)n;
    outline.points     = points;
    outline.tags       = tags;
    outline.contours   = contours;
    outline.flags      = even_odd ? FT_OUTLINE_EVEN_ODD_FILL : FT_OUTLINE_NONE;

    RasterCtx ctx = { sink, user };

    FT_Raster_Params params;
    axl_memset(&params, 0, sizeof params);
    params.source     = &outline;
    /* AA direct-span render, clipped to our explicit clip_box.  The
     * CLIP flag is currently a no-op for this ftgrays (DIRECT mode
     * reads clip_box unconditionally) but is the documented contract
     * for "the clip_box I supply is authoritative" — set it so a
     * future ftgrays bump can't silently reinterpret an unflagged
     * clip_box.  Our box is the exact path bbox, so it clips nothing. */
    params.flags      = FT_RASTER_FLAG_AA
                      | FT_RASTER_FLAG_DIRECT
                      | FT_RASTER_FLAG_CLIP;
    params.gray_spans = ft_span_trampoline;
    params.user       = &ctx;
    /* clip_box is in integer pixels; spans are reported in the same
     * space.  FT_Span x/len are 16-bit, so geometry beyond ~±32767px
     * would truncate — far outside any real GOP surface. */
    params.clip_box.xMin = (FT_Pos)axl_floori((double)min_x);
    params.clip_box.yMin = (FT_Pos)axl_floori((double)min_y);
    params.clip_box.xMax = (FT_Pos)axl_ceili((double)max_x);
    params.clip_box.yMax = (FT_Pos)axl_ceili((double)max_y);

    FT_Raster raster = NULL;
    int rc = gray_raster_new(NULL, &raster);
    if (rc == 0) {
        rc = gray_raster_render(raster, &params);
    }

    if (heap) {
        axl_free(points);
        axl_free(tags);
        axl_free(contours);
    }

    return rc == 0 ? AXL_OK : AXL_ERR;
}
