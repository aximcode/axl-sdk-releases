/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-stroke.c
    AxlGfx path stroker (G8) — width + caps + joins.

    Stroking is a self-contained geometry transform: an AxlGfxPath is
    converted to a stroke *outline* and that outline is filled.  This
    is the same separation the established 2D libraries use (Cairo's
    cairo-path-stroke.c, Skia's SkStroker, Qt's QStroker, FreeType's
    FT_Stroker) — distinct from path construction + fill (axl-gfx-
    path.c) and from the rasterizer (axl-gfx-rasterize.c).

    The outline is built as a union of simple contours — one offset
    quad per segment, plus join geometry at interior vertices and cap
    geometry at open ends — filled with the **non-zero** winding rule
    (the reason G14 added it): overlapping contours accumulate winding,
    so the union fills seamlessly in one anti-aliased pass.  Each
    contour is normalized to a consistent winding, so any contour that
    overlaps the body just keeps it filled.  Joins fill only the OUTER
    (convex) side of a corner — the inner side is already covered by the
    two overlapping segment quads.

    Per AD5 this is the lean (AXL_FREETYPE=0) stroker; FT_Stroker backs
    the full tier.  G8c adds dashes.
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
// Stroke outline accumulator
// ===================================================================

/* The rasterizer caps an outline at 65535 vertices (FT_Outline counts
 * are 16-bit), so the buffer auto-flushes to a fresh fill pass at
 * whole-contour boundaries before it could overrun.  For very long
 * strokes with a non-opaque colour, a rare overlap straddling a flush
 * seam may double-blend slightly — acceptable vs. dropping the whole
 * stroke. */
#define STROKE_FLUSH_CAP  60000u

/* Growable AxlGfxVertex accumulator + the colour it flushes with. */
typedef struct {
    AxlGfxVertex  *v;
    size_t         n;
    size_t         cap;
    bool           oom;
    AxlGfxPixel    color;
    int            rc;     /* worst rasterize result so far */
} StrokeBuf;

/* Resolved stroke options (half-width + tessellation). */
typedef struct {
    double          r;
    AxlGfxLineCap   cap;
    AxlGfxLineJoin  join;
    double          miter_limit;
    int             segs;
} StrokeOpt;

static void
sb_reserve_(
    StrokeBuf  *sb,
    size_t      extra
    )
{
    if (sb->oom || sb->n + extra <= sb->cap) {
        return;
    }
    size_t nc = sb->cap ? sb->cap : 64;
    while (nc < sb->n + extra) {
        nc *= 2;
    }
    AxlGfxVertex *nv = axl_realloc(sb->v, nc * sizeof *nv);
    if (!nv) {
        sb->oom = true;
        return;
    }
    sb->v = nv;
    sb->cap = nc;
}

/* Rasterize the accumulated contours as one non-zero fill and reset
 * the buffer.  A sub-3-vertex buffer is a no-op.  Worst result latches
 * in sb->rc. */
static void
sb_flush_(
    StrokeBuf  *sb
    )
{
    if (sb->oom) {
        sb->rc = AXL_ERR;
        return;
    }
    if (sb->n >= 3) {
        AxlGfxFillSink fs = { sb->color, NULL };
        int rc = axl_gfx_rasterize_fill(sb->v, sb->n, /*even_odd=*/false,
                                        axl_gfx_internal_fill_span, &fs);
        if (rc != AXL_OK) {
            sb->rc = rc;
        }
    }
    sb->n = 0;
}

/* Append a closed contour of @a n points, normalized to a consistent
 * (positive-area) winding so the non-zero union treats every contour
 * additively.  Auto-flushes first if the buffer would overrun the
 * rasterizer's 16-bit vertex cap. */
static void
sb_emit_contour_(
    StrokeBuf           *sb,
    const AxlGfxVertex  *pts,
    size_t               n
    )
{
    if (n < 3) {
        return;
    }
    if (sb->n + n > STROKE_FLUSH_CAP) {
        sb_flush_(sb);
    }
    double area = 0.0;
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        area += (double)pts[i].x * (double)pts[j].y
              - (double)pts[j].x * (double)pts[i].y;
    }
    bool reverse = area < 0.0;
    sb_reserve_(sb, n);
    if (sb->oom) {
        return;
    }
    for (size_t k = 0; k < n; k++) {
        size_t i = reverse ? (n - 1 - k) : k;
        sb->v[sb->n].x       = pts[i].x;
        sb->v[sb->n].y       = pts[i].y;
        sb->v[sb->n].is_move = (k == 0);
        sb->n++;
    }
}

// ===================================================================
// Segment / join / cap geometry
// ===================================================================

/* Normalize (@a dx,@a dy) to unit length; false if degenerate. */
static bool
stroke_unit_(
    double   dx, double dy,
    double  *ux, double *uy
    )
{
    double len = axl_sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        return false;
    }
    *ux = dx / len;
    *uy = dy / len;
    return true;
}

/* Offset quad for segment (@a ax,@a ay)->(@a bx,@a by) at half-width
 * @a r.  Zero-length segments contribute nothing. */
static void
sb_emit_segment_(
    StrokeBuf  *sb,
    float       ax, float ay,
    float       bx, float by,
    double      r
    )
{
    double ux, uy;
    if (!stroke_unit_((double)bx - ax, (double)by - ay, &ux, &uy)) {
        return;
    }
    double px = -uy * r, py = ux * r;
    AxlGfxVertex q[4] = {
        { (float)((double)ax + px), (float)((double)ay + py), false },
        { (float)((double)bx + px), (float)((double)by + py), false },
        { (float)((double)bx - px), (float)((double)by - py), false },
        { (float)((double)ax - px), (float)((double)ay - py), false },
    };
    sb_emit_contour_(sb, q, 4);
}

/* Disc of radius @a r at (@a cx,@a cy), @a segs-gon. */
static void
sb_emit_disc_(
    StrokeBuf  *sb,
    float       cx, float cy,
    double      r,
    int         segs
    )
{
    AxlGfxVertex ring[64];
    if (segs > 64) {
        segs = 64;
    }
    for (int k = 0; k < segs; k++) {
        double a = AXL_MATH_TWO_PI * (double)k / (double)segs;
        ring[k].x       = (float)((double)cx + axl_cos(a) * r);
        ring[k].y       = (float)((double)cy + axl_sin(a) * r);
        ring[k].is_move = false;
    }
    sb_emit_contour_(sb, ring, (size_t)segs);
}

/* Join geometry at vertex (@a vx,@a vy) between incoming unit dir
 * (@a d1x,@a d1y) and outgoing unit dir (@a d2x,@a d2y).  Round → a
 * disc; bevel / miter fill the outer (convex) corner gap only — the
 * inner side is already covered by the two overlapping segment quads. */
static void
stroke_join_(
    StrokeBuf       *sb,
    float            vx, float vy,
    double           d1x, double d1y,
    double           d2x, double d2y,
    const StrokeOpt *o
    )
{
    if (o->join == AXL_GFX_JOIN_ROUND) {
        sb_emit_disc_(sb, vx, vy, o->r, o->segs);
        return;
    }
    double cross = d1x * d2y - d1y * d2x;
    double dot   = d1x * d2x + d1y * d2y;
    if (axl_fabs(cross) < 1e-6 && dot > 0.0) {
        return;   /* collinear — no corner to fill */
    }
    /* Fill ONLY the outer (convex) side of the turn — the inner side
     * is already covered by the two overlapping segment quads.  The
     * outer side is opposite the turn direction: s = -sign(cross).
     * Emitting the inner side too would spill geometry past the body
     * for short segments at sharp angles. */
    double s   = (cross >= 0.0) ? -1.0 : 1.0;
    double p1x = -d1y, p1y = d1x;
    double p2x = -d2y, p2y = d2x;
    double o1x = (double)vx + s * p1x * o->r;
    double o1y = (double)vy + s * p1y * o->r;
    double o2x = (double)vx + s * p2x * o->r;
    double o2y = (double)vy + s * p2y * o->r;

    bool bevel = (o->join == AXL_GFX_JOIN_BEVEL);
    if (!bevel) {
        /* Miter apex = intersection of the two outer offset edge lines.
         * ml is the apex-to-vertex distance; ml/r == miterLength/
         * strokeWidth == 1/sin(theta/2), the SVG/Canvas miter ratio. */
        double denom = cross;
        if (axl_fabs(denom) < 1e-9) {
            bevel = true;   /* parallel offsets */
        } else {
            double t = ((o2x - o1x) * d2y - (o2y - o1y) * d2x) / denom;
            double apx = o1x + t * d1x;
            double apy = o1y + t * d1y;
            double ml  = axl_sqrt((apx - vx) * (apx - vx)
                                + (apy - vy) * (apy - vy));
            if (ml <= o->miter_limit * o->r) {
                AxlGfxVertex quad[4] = {
                    { vx, vy, false },
                    { (float)o1x, (float)o1y, false },
                    { (float)apx, (float)apy, false },
                    { (float)o2x, (float)o2y, false },
                };
                sb_emit_contour_(sb, quad, 4);
            } else {
                bevel = true;   /* miter over the limit → bevel */
            }
        }
    }
    if (bevel) {
        AxlGfxVertex tri[3] = {
            { vx, vy, false },
            { (float)o1x, (float)o1y, false },
            { (float)o2x, (float)o2y, false },
        };
        sb_emit_contour_(sb, tri, 3);
    }
}

/* Cap geometry at open end (@a vx,@a vy) with outward unit dir
 * (@a ox,@a oy).  Butt → nothing; round → disc; square → a quad
 * projecting r past the endpoint. */
static void
stroke_cap_(
    StrokeBuf       *sb,
    float            vx, float vy,
    double           ox, double oy,
    const StrokeOpt *o
    )
{
    if (o->cap == AXL_GFX_CAP_BUTT) {
        return;
    }
    if (o->cap == AXL_GFX_CAP_ROUND) {
        sb_emit_disc_(sb, vx, vy, o->r, o->segs);
        return;
    }
    /* SQUARE — project a half-width quad past the endpoint. */
    double px = -oy * o->r, py = ox * o->r;
    double ex = (double)vx + ox * o->r, ey = (double)vy + oy * o->r;
    AxlGfxVertex quad[4] = {
        { (float)((double)vx + px), (float)((double)vy + py), false },
        { (float)(ex + px),         (float)(ey + py),         false },
        { (float)(ex - px),         (float)(ey - py),         false },
        { (float)((double)vx - px), (float)((double)vy - py), false },
    };
    sb_emit_contour_(sb, quad, 4);
}

/* Stroke one subpath @a v[0..cnt-1]. */
static void
stroke_subpath_(
    StrokeBuf           *sb,
    const AxlGfxVertex  *v,
    size_t               cnt,
    const StrokeOpt     *o
    )
{
    if (cnt == 0) {
        return;
    }
    if (cnt == 1) {
        /* Lone point: round cap → dot; butt/square have no direction. */
        if (o->cap == AXL_GFX_CAP_ROUND) {
            sb_emit_disc_(sb, v[0].x, v[0].y, o->r, o->segs);
        }
        return;
    }

    /* Closed if the last point coincides with the first
     * (axl_gfx_path_close appends the start point). */
    bool closed = cnt >= 3
               && axl_fabs((double)v[cnt - 1].x - v[0].x) < 1e-4
               && axl_fabs((double)v[cnt - 1].y - v[0].y) < 1e-4;

    for (size_t i = 0; i + 1 < cnt; i++) {
        sb_emit_segment_(sb, v[i].x, v[i].y, v[i + 1].x, v[i + 1].y, o->r);
    }

    if (closed) {
        size_t nv = cnt - 1;   /* drop the duplicate closing point */
        for (size_t i = 0; i < nv; i++) {
            size_t pv = (i + nv - 1) % nv;
            size_t nx = (i + 1) % nv;
            double d1x, d1y, d2x, d2y;
            if (stroke_unit_((double)v[i].x - v[pv].x,
                             (double)v[i].y - v[pv].y, &d1x, &d1y)
             && stroke_unit_((double)v[nx].x - v[i].x,
                             (double)v[nx].y - v[i].y, &d2x, &d2y)) {
                stroke_join_(sb, v[i].x, v[i].y, d1x, d1y, d2x, d2y, o);
            }
        }
    } else {
        for (size_t i = 1; i + 1 < cnt; i++) {
            double d1x, d1y, d2x, d2y;
            if (stroke_unit_((double)v[i].x - v[i - 1].x,
                             (double)v[i].y - v[i - 1].y, &d1x, &d1y)
             && stroke_unit_((double)v[i + 1].x - v[i].x,
                             (double)v[i + 1].y - v[i].y, &d2x, &d2y)) {
                stroke_join_(sb, v[i].x, v[i].y, d1x, d1y, d2x, d2y, o);
            }
        }
        double ux, uy;
        if (stroke_unit_((double)v[0].x - v[1].x,
                         (double)v[0].y - v[1].y, &ux, &uy)) {
            stroke_cap_(sb, v[0].x, v[0].y, ux, uy, o);   /* start cap */
        }
        if (stroke_unit_((double)v[cnt - 1].x - v[cnt - 2].x,
                         (double)v[cnt - 1].y - v[cnt - 2].y, &ux, &uy)) {
            stroke_cap_(sb, v[cnt - 1].x, v[cnt - 1].y, ux, uy, o);  /* end cap */
        }
    }
}

// ===================================================================
// Dash pre-pass (G8c)
// ===================================================================
//
// Dashing splits each subpath into on/off intervals by arc length,
// then strokes each "on" interval as its own open piece (so each dash
// gets the configured caps at its ends, and joins at any original
// vertices it spans).  The pattern alternates on,off,on,…; an
// odd-length pattern repeats to even (SVG semantics) via the element
// index parity.

/* Walking state over the alternating dash pattern. */
typedef struct {
    const float  *dashes;
    size_t        n;
    size_t        k;     /* global element index (parity = on/off) */
    double        rem;   /* length remaining in element k */
} DashState;

static bool
dash_on_(const DashState *d)
{
    return (d->k & 1u) == 0u;
}

static void
dash_advance_(DashState *d)
{
    if (d->n == 0u) {
        return;   /* unreachable: dash_init_ guarantees n >= 1 — guards
                   * the modulo against a static-analyzer divide-by-zero */
    }
    d->k++;
    d->rem = (double)d->dashes[d->k % d->n];
}

/* Initialize from @a offset.  Returns false for a degenerate pattern
 * (NULL / empty / negative / all-zero) — caller then strokes solid. */
static bool
dash_init_(
    DashState    *d,
    const float  *dashes,
    size_t        n,
    double        offset
    )
{
    if (!dashes || n == 0) {
        return false;
    }
    double total = 0.0, max_elem = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (dashes[i] < 0.0f) {
            return false;
        }
        total += (double)dashes[i];
        if ((double)dashes[i] > max_elem) {
            max_elem = (double)dashes[i];
        }
    }
    /* Require at least one element long enough to make arc-length
     * progress.  An all-near-zero pattern whose sum is still positive
     * would otherwise let the walk advance the dash index forever
     * without advancing along the segment (infinite loop). */
    if (max_elem <= 1e-9) {
        return false;
    }
    d->dashes = dashes;
    d->n      = n;
    d->k      = 0;
    d->rem    = (double)dashes[0];

    /* Normalize the phase into one on/off period (2*total if odd). */
    double period = (n & 1u) ? 2.0 * total : total;
    double off = offset;
    if (off != 0.0) {
        /* Guard axl_fmod's double->int64 reduction against an absurd
         * offset (out-of-range conversion is UB); such a phase is
         * meaningless, so fall back to phase 0. */
        double q = off / period;
        if (q >= -9.0e18 && q <= 9.0e18) {
            off = axl_fmod(off, period);
            if (off < 0.0) {
                off += period;
            }
        } else {
            off = 0.0;
        }
    }
    for (size_t guard = 0; off > 0.0 && guard < 4 * n + 4; guard++) {
        if (off < d->rem) {
            d->rem -= off;
            off = 0.0;
        } else {
            off -= d->rem;
            dash_advance_(d);   /* skips zero-length elements too */
        }
    }
    return true;
}

/* Growable point buffer for the current "on" dash piece. */
typedef struct {
    AxlGfxVertex  *v;
    size_t         n;
    size_t         cap;
    bool           oom;
} DashPiece;

static void
dp_add_(
    DashPiece  *dp,
    float       x, float y
    )
{
    if (dp->oom) {
        return;
    }
    /* Dedup the duplicate point at a segment boundary. */
    if (dp->n > 0 && dp->v[dp->n - 1].x == x && dp->v[dp->n - 1].y == y) {
        return;
    }
    if (dp->n >= dp->cap) {
        size_t nc = dp->cap ? dp->cap * 2 : 16;
        AxlGfxVertex *nv = axl_realloc(dp->v, nc * sizeof *nv);
        if (!nv) {
            dp->oom = true;
            return;
        }
        dp->v = nv;
        dp->cap = nc;
    }
    dp->v[dp->n].x       = x;
    dp->v[dp->n].y       = y;
    dp->v[dp->n].is_move = false;
    dp->n++;
}

/* Stroke one subpath @a v[0..cnt-1] under the dash pattern. */
static void
stroke_dashed_subpath_(
    StrokeBuf           *sb,
    const AxlGfxVertex  *v,
    size_t               cnt,
    const StrokeOpt     *o,
    const float         *dashes,
    size_t               nd,
    double               offset
    )
{
    DashState d;
    if (cnt < 2 || !dash_init_(&d, dashes, nd, offset)) {
        stroke_subpath_(sb, v, cnt, o);   /* degenerate → solid */
        return;
    }

    DashPiece dp = { NULL, 0, 0, false };
    bool in_on = false;

    for (size_t i = 0; i + 1 < cnt; i++) {
        double ux, uy;
        double dx = (double)v[i + 1].x - v[i].x;
        double dy = (double)v[i + 1].y - v[i].y;
        if (!stroke_unit_(dx, dy, &ux, &uy)) {
            continue;   /* zero-length segment */
        }
        double L = axl_sqrt(dx * dx + dy * dy);
        double pos = 0.0;
        size_t zero_adv = 0;   /* defensive: consecutive zero-length steps */
        while (pos < L - 1e-9) {
            double step = d.rem < (L - pos) ? d.rem : (L - pos);
            if (step <= 1e-9) {
                /* Zero-length element — skip it.  dash_init_ guarantees
                 * a usable element exists within n, so this never cycles
                 * the whole pattern; bail if it somehow does. */
                if (++zero_adv > d.n) {
                    break;
                }
                dash_advance_(&d);
                continue;
            }
            zero_adv = 0;
            if (dash_on_(&d)) {
                dp_add_(&dp, (float)((double)v[i].x + ux * pos),
                             (float)((double)v[i].y + uy * pos));
                dp_add_(&dp, (float)((double)v[i].x + ux * (pos + step)),
                             (float)((double)v[i].y + uy * (pos + step)));
                in_on = true;
            }
            pos     += step;
            d.rem   -= step;
            if (d.rem <= 1e-9) {
                if (in_on) {
                    stroke_subpath_(sb, dp.v, dp.n, o);   /* end this dash */
                    dp.n = 0;
                    in_on = false;
                }
                dash_advance_(&d);
            }
        }
    }
    if (in_on && dp.n >= 1) {
        stroke_subpath_(sb, dp.v, dp.n, o);   /* trailing dash */
    }
    if (dp.oom) {
        sb->oom = true;
    }
    axl_free(dp.v);
}

// ===================================================================
// Public API
// ===================================================================

int
axl_gfx_stroke_path_ex(
    const AxlGfxPath         *p,
    AxlGfxPixel               color,
    const AxlGfxStrokeStyle  *style
    )
{
    if (!p || !style) {
        return AXL_ERR;
    }
    size_t n = 0;
    const AxlGfxVertex *pts = axl_gfx_internal_path_verts(p, &n);
    if (style->width <= 0.0f || n < 1) {
        return AXL_OK;   /* nothing to stroke — no-op success */
    }

    StrokeOpt o;
    o.r           = (double)style->width * 0.5;
    o.cap         = style->cap;
    o.join        = style->join;
    o.miter_limit = style->miter_limit > 0.0f ? (double)style->miter_limit : 10.0;
    /* Disc tessellation scaled to radius: round caps/joins read as
     * round without over-tessellating tiny strokes. */
    o.segs = (int)(o.r * 1.5) + 8;
    if (o.segs < 12) {
        o.segs = 12;
    }
    if (o.segs > 64) {
        o.segs = 64;
    }

    bool dashed = style->dashes != NULL && style->n_dashes > 0;

    StrokeBuf sb = { NULL, 0, 0, false, color, AXL_OK };
    size_t i = 0;
    while (i < n) {
        size_t s = i;
        size_t e = i + 1;
        while (e < n && !pts[e].is_move) {
            e++;
        }
        if (dashed) {
            stroke_dashed_subpath_(&sb, &pts[s], e - s, &o,
                                   style->dashes, style->n_dashes,
                                   (double)style->dash_offset);
        } else {
            stroke_subpath_(&sb, &pts[s], e - s, &o);
        }
        i = e;
    }
    sb_flush_(&sb);

    int rc = sb.oom ? AXL_ERR : sb.rc;
    axl_free(sb.v);
    return rc;
}

int
axl_gfx_stroke_path(
    const AxlGfxPath  *p,
    AxlGfxPixel        color,
    float              w
    )
{
    AxlGfxStrokeStyle style = {
        .width       = w,
        .cap         = AXL_GFX_CAP_BUTT,
        .join        = AXL_GFX_JOIN_MITER,
        .miter_limit = 10.0f,
    };
    return axl_gfx_stroke_path_ex(p, color, &style);
}
