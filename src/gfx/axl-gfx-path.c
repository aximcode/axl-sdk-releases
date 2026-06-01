/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-path.c
    G3 — retained-mode path API + immediate-mode rounded-rect helper,
    plus path fill (G14) and width stroke (G8a).

    Math primitives (sqrt, sin, cos, floor/ceil-to-int, fabs) come
    from <axl/axl-math.h> — same constraints (libm-free, freestanding
    UEFI) lifted into a shared module.

    Implementation choices:
      - Curves and arcs are flattened to line segments at insertion
        time, so fill / stroke walk a uniform segment list.
      - Cubic Beziers subdivide via de Casteljau until each
        sub-curve's control-polygon deviation from straight is
        sub-pixel.
      - Arcs subdivide via the chord-vs-arc-midpoint deviation
        test, also recursive — no sin/cos in the inner loop.
        The arc's start point uses axl_sin / axl_cos.
      - Fill rasterization is delegated to the analytic ftgrays
        backend (axl-gfx-rasterize.c, G14) via axl_gfx_rasterize_fill;
        the fill path uses the even-odd rule (SVG `fill-rule:evenodd`
        / Cairo default).
      - Stroke (G8a) builds an offset-geometry outline (a quad per
        segment + a disc per vertex = round joins/caps) and fills it
        non-zero through the same rasterizer.
      - fill_rounded_rect bypasses the path machinery and rasterizes
        the 4 corners + 4 edges directly using signed-distance
        coverage — faster than walking a 16-segment per-corner
        approximation through the scanline rasterizer.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "axl-gfx-internal.h"
#include <axl/axl-gfx.h>
#include <axl/axl-macros.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>

// ===================================================================
// Path data structure
// ===================================================================

/* Vertices are AxlGfxVertex (axl-gfx-internal.h): is_move=true starts
 * a subpath; otherwise the vertex extends the current subpath via a
 * line segment from the previous point.  The same type is consumed by
 * the rasterizer (axl-gfx-rasterize.c). */

struct AxlGfxPath {
    AxlGfxVertex  *pts;
    size_t      n;
    size_t      cap;

    /* Pen state — referenced by line_to / curve_to / close.  Only
     * valid when n > 0 and a subpath is open. */
    float       pen_x, pen_y;

    /* Start-of-current-subpath coordinates — referenced by close. */
    float       subpath_x, subpath_y;
    bool        subpath_open;
};

static bool
path_grow_(
    AxlGfxPath  *p
    )
{
    if (p->n < p->cap) {
        return true;
    }
    size_t new_cap = p->cap == 0 ? 16 : p->cap * 2;
    AxlGfxVertex *new_pts = axl_realloc(p->pts, new_cap * sizeof *new_pts);
    if (!new_pts) {
        return false;
    }
    p->pts = new_pts;
    p->cap = new_cap;
    return true;
}

static void
path_push_(
    AxlGfxPath  *p,
    float        x,
    float        y,
    bool         is_move
    )
{
    if (!path_grow_(p)) {
        return;
    }
    p->pts[p->n].x       = x;
    p->pts[p->n].y       = y;
    p->pts[p->n].is_move = is_move;
    p->n++;
    p->pen_x = x;
    p->pen_y = y;
}

/* Push a point given in LOCAL coordinates — apply the active gfx
 * transform (G4) then store in world space.  All G4-aware path APIs
 * (move_to, line_to, arc samples) flow through this helper so the
 * stored point list is always in world coords — the rasterizer
 * never needs to know about transforms. */
static void
path_push_local_(
    AxlGfxPath  *p,
    float        x_local,
    float        y_local,
    bool         is_move
    )
{
    AxlTransform M = axl_gfx_internal_current_transform();
    AxlVec2 t = axl_transform_map_point(
                    M,
                    axl_vec2((double)x_local, (double)y_local));
    path_push_(p, (float)t.x, (float)t.y, is_move);
}

// ===================================================================
// Public API — lifecycle
// ===================================================================

AxlGfxPath *
axl_gfx_path_new(void)
{
    AxlGfxPath *p = axl_malloc(sizeof *p);
    if (!p) {
        return NULL;
    }
    p->pts          = NULL;
    p->n            = 0;
    p->cap          = 0;
    p->pen_x        = 0.0f;
    p->pen_y        = 0.0f;
    p->subpath_x    = 0.0f;
    p->subpath_y    = 0.0f;
    p->subpath_open = false;
    return p;
}

void
axl_gfx_path_free(
    AxlGfxPath  *p
    )
{
    if (!p) {
        return;
    }
    axl_free(p->pts);
    axl_free(p);
}

void
axl_gfx_path_reset(
    AxlGfxPath  *p
    )
{
    if (!p) {
        return;
    }
    p->n            = 0;
    p->pen_x        = 0.0f;
    p->pen_y        = 0.0f;
    p->subpath_x    = 0.0f;
    p->subpath_y    = 0.0f;
    p->subpath_open = false;
}

// ===================================================================
// Public API — path building
// ===================================================================

void
axl_gfx_path_move_to(
    AxlGfxPath  *p,
    float        x,
    float        y
    )
{
    if (!p) {
        return;
    }
    /* Transform inline so subpath_x/y stores the WORLD-space subpath
     * start.  If the active transform changes between move_to and
     * close, close still returns to the original (transformed) start
     * — matches HTML canvas semantics. */
    AxlTransform M = axl_gfx_internal_current_transform();
    AxlVec2 t = axl_transform_map_point(
                    M, axl_vec2((double)x, (double)y));
    path_push_(p, (float)t.x, (float)t.y, true);
    p->subpath_x    = (float)t.x;
    p->subpath_y    = (float)t.y;
    p->subpath_open = true;
}

void
axl_gfx_path_line_to(
    AxlGfxPath  *p,
    float        x,
    float        y
    )
{
    if (!p) {
        return;
    }
    if (!p->subpath_open) {
        /* Implicit move_to — start the subpath at (x, y) with no
         * segment yet.  move_to applies the transform. */
        axl_gfx_path_move_to(p, x, y);
        return;
    }
    path_push_local_(p, x, y, false);
}

/* Recursive de Casteljau cubic Bezier subdivision.  Stops when the
 * control polygon is flat enough (per-control deviation from the
 * end-to-end chord is sub-pixel) or max depth is reached. */
static void
subdivide_cubic_(
    AxlGfxPath  *p,
    float        x0, float y0,
    float        x1, float y1,
    float        x2, float y2,
    float        x3, float y3,
    int          depth
    )
{
    if (depth > 14) {
        path_push_(p, x3, y3, false);
        return;
    }
    /* Flatness test: distance from each control point to the
     * chord (x0, y0) → (x3, y3). */
    float dx = x3 - x0;
    float dy = y3 - y0;
    float d1 = (float)axl_fabs((double)((x1 - x0) * dy - (y1 - y0) * dx));
    float d2 = (float)axl_fabs((double)((x2 - x0) * dy - (y2 - y0) * dx));
    float len_sq = dx * dx + dy * dy;
    /* (d1 + d2)^2 < 0.25 * len_sq  ≈  combined deviation < 0.5 px. */
    float dev = d1 + d2;
    if (dev * dev < 0.25f * len_sq + 0.0001f) {
        path_push_(p, x3, y3, false);
        return;
    }

    float x01   = (x0 + x1) * 0.5f, y01   = (y0 + y1) * 0.5f;
    float x12   = (x1 + x2) * 0.5f, y12   = (y1 + y2) * 0.5f;
    float x23   = (x2 + x3) * 0.5f, y23   = (y2 + y3) * 0.5f;
    float x012  = (x01 + x12) * 0.5f, y012  = (y01 + y12) * 0.5f;
    float x123  = (x12 + x23) * 0.5f, y123  = (y12 + y23) * 0.5f;
    float x0123 = (x012 + x123) * 0.5f, y0123 = (y012 + y123) * 0.5f;

    subdivide_cubic_(p, x0, y0, x01, y01, x012, y012, x0123, y0123,
                     depth + 1);
    subdivide_cubic_(p, x0123, y0123, x123, y123, x23, y23, x3, y3,
                     depth + 1);
}

void
axl_gfx_path_curve_to(
    AxlGfxPath  *p,
    float        c1x, float c1y,
    float        c2x, float c2y,
    float        x,   float y
    )
{
    if (!p) {
        return;
    }
    if (!p->subpath_open) {
        /* No starting point — implicit move to the curve end. */
        axl_gfx_path_move_to(p, x, y);
        return;
    }
    /* Transform control points + endpoint inline so subdivide_cubic_
     * operates entirely in world space (pen_x/y is already world).
     * Affine transforms commute with de Casteljau subdivision, so
     * subdividing in world space gives the same curve as subdividing
     * in local space then transforming each sampled point. */
    AxlTransform M = axl_gfx_internal_current_transform();
    AxlVec2 tc1  = axl_transform_map_point(M, axl_vec2(c1x, c1y));
    AxlVec2 tc2  = axl_transform_map_point(M, axl_vec2(c2x, c2y));
    AxlVec2 tend = axl_transform_map_point(M, axl_vec2(x,   y));
    float x0 = p->pen_x, y0 = p->pen_y;
    subdivide_cubic_(p, x0, y0,
                     (float)tc1.x,  (float)tc1.y,
                     (float)tc2.x,  (float)tc2.y,
                     (float)tend.x, (float)tend.y, 0);
}

/* Recursive arc subdivision via chord-vs-arc-midpoint deviation.
 * Avoids per-step sin/cos by walking endpoints on the circle and
 * recursively halving the chord. */
static void
subdivide_arc_(
    AxlGfxPath  *p,
    double       cx, double cy, double r,
    double       x0, double y0,
    double       x1, double y1,
    int          depth
    )
{
    if (depth > 8) {
        path_push_local_(p, (float)x1, (float)y1, false);
        return;
    }
    double mx = (x0 + x1) * 0.5;
    double my = (y0 + y1) * 0.5;
    double dx = mx - cx;
    double dy = my - cy;
    double len = axl_sqrt(dx * dx + dy * dy);
    /* If the chord midpoint is within 0.25 px of the arc, accept
     * the chord as a line segment.  Note: deviation is measured in
     * LOCAL space; under non-uniform-scale or rotation the actual
     * world-space deviation differs, but the sample density is
     * still safe for UI-scale inputs. */
    double deviation = axl_fabs(r - len);
    if (deviation < 0.25 || len < 1e-9) {
        path_push_local_(p, (float)x1, (float)y1, false);
        return;
    }
    /* Project chord midpoint onto the circle to get the arc midpoint. */
    double scale = r / len;
    double amx = cx + dx * scale;
    double amy = cy + dy * scale;

    subdivide_arc_(p, cx, cy, r, x0, y0, amx, amy, depth + 1);
    subdivide_arc_(p, cx, cy, r, amx, amy, x1, y1, depth + 1);
}

void
axl_gfx_path_arc(
    AxlGfxPath  *p,
    float        cx,
    float        cy,
    float        r,
    float        start_rad,
    float        end_rad
    )
{
    if (!p || r <= 0.0f || end_rad <= start_rad) {
        return;
    }

    /* Pre-split into ≤ π/2 sub-arcs before recursive subdivision.
     * The chord-vs-midpoint deviation test in subdivide_arc_
     * collapses to zero for a 180° chord (which passes through
     * the circle center) — pre-splitting keeps every sub-arc
     * strictly inside one quadrant where the test is well-behaved. */
    double total = (double)end_rad - (double)start_rad;
    int n_segments = (int)axl_ceili(total / AXL_MATH_HALF_PI);
    if (n_segments < 1) {
        n_segments = 1;
    }

    double sx = (double)cx + (double)r * axl_cos((double)start_rad);
    double sy = (double)cy + (double)r * axl_sin((double)start_rad);

    if (!p->subpath_open) {
        axl_gfx_path_move_to(p, (float)sx, (float)sy);
    } else {
        axl_gfx_path_line_to(p, (float)sx, (float)sy);
    }

    double prev_x = sx, prev_y = sy;
    for (int i = 1; i <= n_segments; i++) {
        double seg_end_rad = (double)start_rad
                             + total * (double)i / (double)n_segments;
        double ex = (double)cx + (double)r * axl_cos(seg_end_rad);
        double ey = (double)cy + (double)r * axl_sin(seg_end_rad);
        subdivide_arc_(p, (double)cx, (double)cy, (double)r,
                       prev_x, prev_y, ex, ey, 0);
        prev_x = ex;
        prev_y = ey;
    }
}

void
axl_gfx_path_close(
    AxlGfxPath  *p
    )
{
    if (!p || !p->subpath_open) {
        return;
    }
    /* Add a line back to subpath start.  After close, no subpath is
     * open — next move_to starts a new one. */
    path_push_(p, p->subpath_x, p->subpath_y, false);
    p->subpath_open = false;
}

// ===================================================================
// Internal — fill via the analytic rasterizer (axl-gfx-rasterize.c)
// ===================================================================

/* Per-span sink for axl_gfx_rasterize_fill, shared by fill and stroke
 * (declared in axl-gfx-internal.h).  Modulates the source alpha by
 * ftgrays' 8-bit coverage and blits through axl_gfx_fill_rect_i, which
 * applies the active clip stack, the current draw target, and alpha
 * blending. */
void
axl_gfx_internal_fill_span(
    int32_t  y,
    int32_t  x,
    int32_t  len,
    uint8_t  coverage,
    void    *user
    )
{
    const AxlGfxFillSink *fs = (const AxlGfxFillSink *)user;
    if (coverage == 0) {
        return;
    }
    if (fs->grad != NULL) {
        /* Gradient varies per pixel — emit one pixel at a time. */
        for (int32_t i = 0; i < len; i++) {
            AxlGfxPixel out = axl_gfx_gradient_sample(fs->grad, x + i, y);
            out.alpha = (uint8_t)(((uint32_t)out.alpha * coverage + 127u) / 255u);
            if (out.alpha != 0) {
                axl_gfx_fill_rect_i(x + i, y, 1, 1, out);
            }
        }
    } else {
        /* Solid: coverage is uniform across the span, so one rect
         * blit covers the whole run. */
        AxlGfxPixel out = fs->color;
        out.alpha = (uint8_t)(((uint32_t)fs->color.alpha * coverage + 127u) / 255u);
        if (out.alpha != 0) {
            axl_gfx_fill_rect_i(x, y, len, 1, out);
        }
    }
}

/* World-space vertex accessor — lets the stroker (axl-gfx-stroke.c)
 * read path geometry without the AxlGfxPath struct definition. */
const AxlGfxVertex *
axl_gfx_internal_path_verts(
    const AxlGfxPath  *p,
    size_t            *out_n
    )
{
    if (!p) {
        *out_n = 0;
        return NULL;
    }
    *out_n = p->n;
    return p->pts;
}

/* Shared fill entry. @a grad != NULL selects per-pixel gradient
 * sampling; otherwise the solid @a color is used.  Uses the even-odd
 * fill rule — the historical behavior of this public API.  Contours
 * are implicitly closed by the rasterizer. */
static int
fill_path_paint_(
    const AxlGfxPath      *p,
    AxlGfxPixel            color,
    const AxlGfxGradient  *grad
    )
{
    if (!p || p->n < 3) {
        return AXL_ERR;
    }
    AxlGfxFillSink fs = { color, grad };
    return axl_gfx_rasterize_fill(p->pts, p->n, /*even_odd=*/true,
                                  axl_gfx_internal_fill_span, &fs);
}

int
axl_gfx_fill_path(
    const AxlGfxPath  *p,
    AxlGfxPixel        color
    )
{
    return fill_path_paint_(p, color, NULL);
}

int
axl_gfx_fill_path_gradient(
    const AxlGfxPath      *p,
    const AxlGfxGradient  *g
    )
{
    if (g == NULL) {
        return AXL_ERR;
    }
    AxlGfxPixel unused = { 0, 0, 0, 0 };
    return fill_path_paint_(p, unused, g);
}

// ===================================================================
// Rounded rect — direct SDF coverage (does NOT use the path API)
// ===================================================================

/* Signed-distance coverage for a circular corner.
 *
 * (@a dx, @a dy) is the pixel center's offset from the corner's
 * curvature center.  @a r is the corner radius.
 *
 * Returns coverage in [0, 1] — 1 fully inside the rounded rect's
 * interior, 0 fully outside, fractional in the 1-pixel-wide AA
 * band around the curve. */
static float
corner_coverage_(
    double  dx,
    double  dy,
    double  r
    )
{
    double dist = axl_sqrt(dx * dx + dy * dy);
    if (dist <= r - 0.5) {
        return 1.0f;
    }
    if (dist >= r + 0.5) {
        return 0.0f;
    }
    /* Linear AA band — distance from r maps to coverage. */
    return (float)(r + 0.5 - dist);
}

/* Shared rounded-rect fill. @a grad != NULL selects per-pixel
   gradient sampling (straight bands via fill_rect_gradient, corners
   via axl_gfx_gradient_sample); otherwise solid @a color. */
static int
fill_rounded_rect_paint_(
    int32_t                x,
    int32_t                y,
    int32_t                w,
    int32_t                h,
    float                  radius,
    AxlGfxPixel            color,
    const AxlGfxGradient  *grad
    )
{
    if (w <= 0 || h <= 0) {
        return AXL_OK;
    }
    if (radius < 0.0f) {
        radius = 0.0f;
    }
    /* Clamp radius to half the smaller dim. */
    float max_r = (float)(w < h ? w : h) * 0.5f;
    if (radius > max_r) {
        radius = max_r;
    }

    /* Zero-radius case: defer to a plain rect fill. */
    if (radius < 0.5f) {
        return grad != NULL
               ? axl_gfx_fill_rect_gradient(x, y, w, h, grad)
               : axl_gfx_fill_rect_i(x, y, w, h, color);
    }

    int32_t r_int = (int32_t)axl_ceili((double)radius);

    /* The three straight bands: solid fill, or per-pixel gradient. */
    if (grad != NULL) {
        axl_gfx_fill_rect_gradient(x, y + r_int, w, h - 2 * r_int, grad);
        axl_gfx_fill_rect_gradient(x + r_int, y, w - 2 * r_int, r_int, grad);
        axl_gfx_fill_rect_gradient(x + r_int, y + h - r_int,
                                   w - 2 * r_int, r_int, grad);
    } else {
        axl_gfx_fill_rect_i(x, y + r_int, w, h - 2 * r_int, color);
        axl_gfx_fill_rect_i(x + r_int, y, w - 2 * r_int, r_int, color);
        axl_gfx_fill_rect_i(x + r_int, y + h - r_int,
                            w - 2 * r_int, r_int, color);
    }

    /* 4 corners — TL, TR, BL, BR.  For each, the curvature center
     * lies inside the rect at (corner_x + ±radius, corner_y + ±radius). */
    struct {
        int32_t  px_x0, px_y0;     /* pixel-box origin */
        double   center_x, center_y;
    } corners[4] = {
        { x,                  y,
          (double)x + (double)radius,
          (double)y + (double)radius },                    /* TL */
        { x + w - r_int,      y,
          (double)x + (double)w - (double)radius - 1.0,
          (double)y + (double)radius },                    /* TR */
        { x,                  y + h - r_int,
          (double)x + (double)radius,
          (double)y + (double)h - (double)radius - 1.0 },  /* BL */
        { x + w - r_int,      y + h - r_int,
          (double)x + (double)w - (double)radius - 1.0,
          (double)y + (double)h - (double)radius - 1.0 },  /* BR */
    };

    for (int c = 0; c < 4; c++) {
        for (int dy = 0; dy < r_int; dy++) {
            for (int dx = 0; dx < r_int; dx++) {
                int32_t px = corners[c].px_x0 + dx;
                int32_t py = corners[c].px_y0 + dy;
                double  ddx = (double)px + 0.5 - corners[c].center_x;
                double  ddy = (double)py + 0.5 - corners[c].center_y;
                float cov = corner_coverage_(ddx, ddy, (double)radius);
                if (cov == 0.0f) {
                    continue;
                }
                AxlGfxPixel out = grad != NULL
                                  ? axl_gfx_gradient_sample(grad, px, py)
                                  : color;
                uint32_t a = (uint32_t)out.alpha * (uint32_t)(cov * 255.0f + 0.5f);
                out.alpha = (uint8_t)((a + 127u) / 255u);
                if (out.alpha != 0) {
                    axl_gfx_fill_rect_i(px, py, 1, 1, out);
                }
            }
        }
    }

    return AXL_OK;
}

int
axl_gfx_fill_rounded_rect(
    int32_t      x,
    int32_t      y,
    int32_t      w,
    int32_t      h,
    float        radius,
    AxlGfxPixel  color
    )
{
    return fill_rounded_rect_paint_(x, y, w, h, radius, color, NULL);
}

int
axl_gfx_fill_rounded_rect_gradient(
    int32_t                x,
    int32_t                y,
    int32_t                w,
    int32_t                h,
    float                  radius,
    const AxlGfxGradient  *g
    )
{
    if (g == NULL) {
        return AXL_ERR;
    }
    AxlGfxPixel unused = { 0, 0, 0, 0 };
    return fill_rounded_rect_paint_(x, y, w, h, radius, unused, g);
}
