/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-gfx-region.c — AxlGfxRegion: an exact set of non-overlapping
 * rectangles with union / subtract / intersect set algebra (the
 * pixman / X11 `miregion` role), for damage tracking, opaque-region
 * occlusion, and multi-rect input regions.
 *
 * Representation: rectangles in canonical form (non-overlapping, y-sorted
 * bands) stored in an AxlArray of AxlGfxClip. The three ops are a band
 * SWEEP: cut the y-axis at every band edge of both operands, combine the
 * two x-span sets per strip, and coalesce vertically-adjacent identical
 * bands. Coordinates are half-open: a rect (x,y,w,h) covers [x,x+w)×[y,y+h).
 *
 * OOM: a mutating op that cannot allocate degrades the region to its
 * conservative bounding-box superset (reusing the retained buffer), sets
 * `lossy`, and returns AXL_ERR. See axl-gfx-region.h for the full contract.
 */

#include <axl/axl-gfx-region.h>
#include <axl/axl-array.h>
#include <axl/axl-mem.h>

typedef enum { OP_UNION, OP_SUBTRACT, OP_INTERSECT } RegionOp;

typedef struct { int32_t x0, x1; } Span;   /* half-open [x0, x1) */

struct AxlGfxRegion {
    AxlArray *rects;   /* of AxlGfxClip, canonical banded */
    bool      lossy;
};

/* ---- rect helpers (edges in int64 to dodge int32 overflow at margins) ---- */

static int64_t clip_x1(AxlGfxClip c) { return (int64_t)c.x + c.w; }
static int64_t clip_y1(AxlGfxClip c) { return (int64_t)c.y + c.h; }
static bool    clip_empty(AxlGfxClip c) { return c.w == 0 || c.h == 0; }

static AxlGfxClip
clip_make(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    AxlGfxClip c = {0, 0, 0, 0};
    if (x1 <= x0 || y1 <= y0) {
        return c;
    }
    c.x = (int32_t)x0;
    c.y = (int32_t)y0;
    c.w = (uint32_t)(x1 - x0);
    c.h = (uint32_t)(y1 - y0);
    return c;
}

static AxlGfxClip
region_rect(const AxlGfxRegion *r, size_t i)
{
    AxlGfxClip *p = (AxlGfxClip *)axl_array_get(r->rects, i);
    return p ? *p : (AxlGfxClip){0, 0, 0, 0};
}

static size_t
region_n(const AxlGfxRegion *r)
{
    return axl_array_len(r->rects);
}

/* ---- span-vector helpers (over an AxlArray of Span) ---- */

static Span *
span_base(AxlArray *sv)
{
    return (axl_array_len(sv) > 0) ? (Span *)axl_array_get(sv, 0) : NULL;
}

static int
span_push(AxlArray *sv, int32_t x0, int32_t x1)
{
    if (x1 <= x0) {
        return AXL_OK;
    }
    Span s = {x0, x1};
    return axl_array_append(sv, &s);
}

/* sort by x0 (insertion — span lists per band are tiny) + coalesce in place */
static void
span_canon(AxlArray *sv)
{
    size_t n = axl_array_len(sv);
    if (n < 2) {
        return;
    }
    Span *a = span_base(sv);
    for (size_t i = 1; i < n; i++) {
        Span key = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1].x0 > key.x0) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
    size_t w = 0;
    for (size_t i = 1; i < n; i++) {
        if (a[i].x0 <= a[w].x1) {                 /* overlap or abut */
            if (a[i].x1 > a[w].x1) a[w].x1 = a[i].x1;
        } else {
            a[++w] = a[i];
        }
    }
    axl_array_set_size(sv, w + 1);
}

static bool
spans_equal(AxlArray *a, AxlArray *b)
{
    size_t n = axl_array_len(a);
    if (n != axl_array_len(b)) {
        return false;
    }
    Span *pa = span_base(a), *pb = span_base(b);
    for (size_t i = 0; i < n; i++) {
        if (pa[i].x0 != pb[i].x0 || pa[i].x1 != pb[i].x1) {
            return false;
        }
    }
    return true;
}

static int
spans_copy(AxlArray *dst, AxlArray *src)
{
    axl_array_clear(dst);
    size_t n = axl_array_len(src);
    Span *p = span_base(src);
    for (size_t i = 0; i < n; i++) {
        if (axl_array_append(dst, &p[i]) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

/* x-spans of the rects in @a covering the y-strip [yt,yb), canonicalised */
static int
spans_at(AxlArray *rects, int32_t yt, int32_t yb, AxlArray *out)
{
    axl_array_clear(out);
    size_t n = axl_array_len(rects);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip *c = (AxlGfxClip *)axl_array_get(rects, i);
        if (c->y <= yt && clip_y1(*c) >= yb) {
            if (span_push(out, c->x, (int32_t)clip_x1(*c)) != AXL_OK) {
                return AXL_ERR;
            }
        }
    }
    span_canon(out);
    return AXL_OK;
}

/* combine two canonical span lists per the op; @out is cleared first */
static int
combine(AxlArray *a, AxlArray *b, RegionOp op, AxlArray *out)
{
    axl_array_clear(out);
    size_t na = axl_array_len(a), nb = axl_array_len(b);
    Span *pa = span_base(a), *pb = span_base(b);

    if (op == OP_UNION) {
        for (size_t i = 0; i < na; i++) {
            if (span_push(out, pa[i].x0, pa[i].x1) != AXL_OK) return AXL_ERR;
        }
        for (size_t i = 0; i < nb; i++) {
            if (span_push(out, pb[i].x0, pb[i].x1) != AXL_OK) return AXL_ERR;
        }
        span_canon(out);
    } else if (op == OP_INTERSECT) {
        size_t i = 0, j = 0;
        while (i < na && j < nb) {
            int32_t lo = pa[i].x0 > pb[j].x0 ? pa[i].x0 : pb[j].x0;
            int32_t hi = pa[i].x1 < pb[j].x1 ? pa[i].x1 : pb[j].x1;
            if (span_push(out, lo, hi) != AXL_OK) return AXL_ERR;
            if (pa[i].x1 < pb[j].x1) i++; else j++;
        }
    } else { /* OP_SUBTRACT: a ∖ b */
        for (size_t i = 0; i < na; i++) {
            int32_t cur = pa[i].x0;
            for (size_t k = 0; k < nb && pb[k].x0 < pa[i].x1; k++) {
                if (pb[k].x1 <= cur) continue;
                if (pb[k].x0 > cur) {
                    if (span_push(out, cur, pb[k].x0) != AXL_OK) return AXL_ERR;
                }
                if (pb[k].x1 > cur) cur = pb[k].x1;
                if (cur >= pa[i].x1) break;
            }
            if (cur < pa[i].x1) {
                if (span_push(out, cur, pa[i].x1) != AXL_OK) return AXL_ERR;
            }
        }
    }
    return AXL_OK;
}

/* emit one band: a rect per span across [yt,yb) */
static int
flush_band(AxlArray *out, AxlArray *spans, int32_t yt, int32_t yb)
{
    size_t n = axl_array_len(spans);
    Span *p = span_base(spans);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip c = clip_make(p[i].x0, yt, p[i].x1, yb);
        if (axl_array_append(out, &c) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

/* the band sweep: out := A op B (out is cleared). AXL_ERR only on OOM. */
static int
region_op(AxlArray *A, AxlArray *B, RegionOp op, AxlArray *out)
{
    axl_array_clear(out);   /* defensive: callers pass a fresh array today */

    AxlArray *ys = axl_array_new(sizeof(int32_t));
    AxlArray *sa = axl_array_new(sizeof(Span));
    AxlArray *sb = axl_array_new(sizeof(Span));
    AxlArray *sc = axl_array_new(sizeof(Span));
    AxlArray *prev = axl_array_new(sizeof(Span));
    int rc = AXL_OK;
    if (ys == NULL || sa == NULL || sb == NULL || sc == NULL || prev == NULL) {
        rc = AXL_ERR;
        goto cleanup;
    }

    /* collect candidate band boundaries from both operands */
    AxlArray *src[2] = {A, B};
    for (int s = 0; s < 2; s++) {
        size_t n = axl_array_len(src[s]);
        for (size_t i = 0; i < n; i++) {
            AxlGfxClip *c = (AxlGfxClip *)axl_array_get(src[s], i);
            int32_t y0 = c->y, y1 = (int32_t)clip_y1(*c);
            if (axl_array_append(ys, &y0) != AXL_OK ||
                axl_array_append(ys, &y1) != AXL_OK) {
                rc = AXL_ERR;
                goto cleanup;
            }
        }
    }
    size_t yn = axl_array_len(ys);
    if (yn == 0) {
        goto cleanup;   /* both empty -> empty result */
    }
    /* sort the boundaries (insertion sort; counts are small) */
    {
        int32_t *yv = (int32_t *)axl_array_get(ys, 0);
        for (size_t i = 1; i < yn; i++) {
            int32_t key = yv[i];
            size_t j = i;
            while (j > 0 && yv[j - 1] > key) {
                yv[j] = yv[j - 1];
                j--;
            }
            yv[j] = key;
        }
    }

    bool have_prev = false;
    int32_t prev_yt = 0;
    int32_t *yv = (int32_t *)axl_array_get(ys, 0);
    for (size_t i = 0; i + 1 < yn; i++) {
        int32_t yt = yv[i], yb = yv[i + 1];
        if (yb <= yt) {
            continue;   /* duplicate boundary */
        }
        if (spans_at(A, yt, yb, sa) != AXL_OK ||
            spans_at(B, yt, yb, sb) != AXL_OK ||
            combine(sa, sb, op, sc) != AXL_OK) {
            rc = AXL_ERR;
            goto cleanup;
        }
        if (have_prev && spans_equal(sc, prev)) {
            continue;   /* same spans — extend the open band downward */
        }
        if (have_prev) {
            if (flush_band(out, prev, prev_yt, yt) != AXL_OK) {
                rc = AXL_ERR;
                goto cleanup;
            }
        }
        if (spans_copy(prev, sc) != AXL_OK) {
            rc = AXL_ERR;
            goto cleanup;
        }
        prev_yt = yt;
        have_prev = (axl_array_len(sc) > 0);
    }
    if (have_prev) {
        if (flush_band(out, prev, prev_yt, yv[yn - 1]) != AXL_OK) {
            rc = AXL_ERR;
        }
    }

cleanup:
    axl_array_free(ys);
    axl_array_free(sa);
    axl_array_free(sb);
    axl_array_free(sc);
    axl_array_free(prev);
    return rc;
}

/* ---- bounding box of an AxlArray of rects ---- */
static AxlGfxClip
rects_bounds(AxlArray *rects)
{
    size_t n = axl_array_len(rects);
    if (n == 0) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    AxlGfxClip *c0 = (AxlGfxClip *)axl_array_get(rects, 0);
    int64_t x0 = c0->x, y0 = c0->y, x1 = clip_x1(*c0), y1 = clip_y1(*c0);
    for (size_t i = 1; i < n; i++) {
        AxlGfxClip *c = (AxlGfxClip *)axl_array_get(rects, i);
        if (c->x < x0) x0 = c->x;
        if (c->y < y0) y0 = c->y;
        if (clip_x1(*c) > x1) x1 = clip_x1(*c);
        if (clip_y1(*c) > y1) y1 = clip_y1(*c);
    }
    return clip_make(x0, y0, x1, y1);
}

/* Degrade @r to a single bounding-box rect (reusing the retained buffer so
 * the lone append cannot re-allocate when @r already held rects) + lossy. */
static void
region_degrade(AxlGfxRegion *r, AxlGfxClip bbox)
{
    axl_array_clear(r->rects);
    if (!clip_empty(bbox)) {
        axl_array_append(r->rects, &bbox);
    }
    r->lossy = true;
}

/* Apply r := r op other. On OOM, degrade to the op's conservative bbox. */
static int
region_apply(AxlGfxRegion *r, const AxlGfxRegion *other, RegionOp op)
{
    if (r == NULL || other == NULL) {
        return AXL_ERR;
    }
    bool was_lossy = r->lossy || other->lossy;
    AxlArray *out = axl_array_new(sizeof(AxlGfxClip));
    if (out != NULL && region_op(r->rects, other->rects, op, out) == AXL_OK) {
        axl_array_free(r->rects);
        r->rects = out;
        r->lossy = was_lossy;
        return AXL_OK;
    }
    axl_array_free(out);
    /* OOM degrade: a conservative superset bbox per op. Guard the empty
     * operands so an absent rect set can't drag the origin into the box. */
    AxlGfxClip rb = rects_bounds(r->rects);          /* empty -> {0,0,0,0} */
    AxlGfxClip ob = rects_bounds(other->rects);
    bool re = clip_empty(rb), oe = clip_empty(ob);
    AxlGfxClip deg = {0, 0, 0, 0};
    if (op == OP_UNION) {                            /* superset of rb ∪ ob */
        if (re)      deg = ob;
        else if (oe) deg = rb;
        else deg = clip_make(rb.x < ob.x ? rb.x : ob.x,
                             rb.y < ob.y ? rb.y : ob.y,
                             clip_x1(rb) > clip_x1(ob) ? clip_x1(rb) : clip_x1(ob),
                             clip_y1(rb) > clip_y1(ob) ? clip_y1(rb) : clip_y1(ob));
    } else if (op == OP_INTERSECT) {                 /* superset of rb ∩ ob */
        if (!re && !oe)
            deg = clip_make(rb.x > ob.x ? rb.x : ob.x,
                            rb.y > ob.y ? rb.y : ob.y,
                            clip_x1(rb) < clip_x1(ob) ? clip_x1(rb) : clip_x1(ob),
                            clip_y1(rb) < clip_y1(ob) ? clip_y1(rb) : clip_y1(ob));
        /* else either operand empty -> intersection empty -> deg stays empty */
    } else {                                         /* subtract: result ⊆ r */
        deg = rb;
    }
    region_degrade(r, deg);
    return AXL_ERR;
}

/* Wrap a single rect in a temporary one-rect region for the _rect ops. */
static int
region_apply_rect(AxlGfxRegion *r, AxlGfxClip rect, RegionOp op)
{
    if (r == NULL) {
        return AXL_ERR;
    }
    AxlGfxRegion tmp = { axl_array_new(sizeof(AxlGfxClip)), false };
    if (tmp.rects == NULL) {
        region_degrade(r, rects_bounds(r->rects));
        return AXL_ERR;
    }
    if (!clip_empty(rect)) {
        axl_array_append(tmp.rects, &rect);
    }
    int rc = region_apply(r, &tmp, op);
    axl_array_free(tmp.rects);
    return rc;
}

/* =========================== public API =========================== */

AxlGfxRegion *
axl_gfx_region_new(void)
{
    AxlGfxRegion *r = axl_malloc(sizeof(AxlGfxRegion));
    if (r == NULL) {
        return NULL;
    }
    r->rects = axl_array_new(sizeof(AxlGfxClip));
    r->lossy = false;
    if (r->rects == NULL) {
        axl_free(r);
        return NULL;
    }
    return r;
}

void
axl_gfx_region_free(AxlGfxRegion *r)
{
    if (r == NULL) {
        return;
    }
    axl_array_free(r->rects);
    axl_free(r);
}

void
axl_gfx_region_clear(AxlGfxRegion *r)
{
    if (r == NULL) {
        return;
    }
    axl_array_clear(r->rects);
    r->lossy = false;
}

int
axl_gfx_region_copy(AxlGfxRegion *dst, const AxlGfxRegion *src)
{
    if (dst == NULL || src == NULL) {
        return AXL_ERR;
    }
    axl_array_clear(dst->rects);
    size_t n = region_n(src);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip c = region_rect(src, i);
        if (axl_array_append(dst->rects, &c) != AXL_OK) {
            region_degrade(dst, rects_bounds(src->rects));
            return AXL_ERR;
        }
    }
    dst->lossy = src->lossy;
    return AXL_OK;
}

int
axl_gfx_region_union(AxlGfxRegion *r, const AxlGfxRegion *other)
{
    return region_apply(r, other, OP_UNION);
}

int
axl_gfx_region_subtract(AxlGfxRegion *r, const AxlGfxRegion *other)
{
    return region_apply(r, other, OP_SUBTRACT);
}

int
axl_gfx_region_intersect(AxlGfxRegion *r, const AxlGfxRegion *other)
{
    return region_apply(r, other, OP_INTERSECT);
}

int
axl_gfx_region_union_rect(AxlGfxRegion *r, AxlGfxClip rect)
{
    return region_apply_rect(r, rect, OP_UNION);
}

int
axl_gfx_region_subtract_rect(AxlGfxRegion *r, AxlGfxClip rect)
{
    return region_apply_rect(r, rect, OP_SUBTRACT);
}

int
axl_gfx_region_intersect_rect(AxlGfxRegion *r, AxlGfxClip rect)
{
    return region_apply_rect(r, rect, OP_INTERSECT);
}

void
axl_gfx_region_translate(AxlGfxRegion *r, int32_t dx, int32_t dy)
{
    if (r == NULL) {
        return;
    }
    size_t n = region_n(r);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip *c = (AxlGfxClip *)axl_array_get(r->rects, i);
        c->x += dx;
        c->y += dy;
    }
}

bool
axl_gfx_region_is_empty(const AxlGfxRegion *r)
{
    return r == NULL || region_n(r) == 0;
}

bool
axl_gfx_region_is_lossy(const AxlGfxRegion *r)
{
    return r != NULL && r->lossy;
}

AxlGfxClip
axl_gfx_region_bounds(const AxlGfxRegion *r)
{
    if (r == NULL) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    return rects_bounds(r->rects);
}

bool
axl_gfx_region_contains_point(const AxlGfxRegion *r, int32_t x, int32_t y)
{
    if (r == NULL) {
        return false;
    }
    size_t n = region_n(r);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip c = region_rect(r, i);
        if (x >= c.x && y >= c.y && x < clip_x1(c) && y < clip_y1(c)) {
            return true;
        }
    }
    return false;
}

bool
axl_gfx_region_intersects_rect(const AxlGfxRegion *r, AxlGfxClip rect)
{
    if (r == NULL || clip_empty(rect)) {
        return false;
    }
    size_t n = region_n(r);
    for (size_t i = 0; i < n; i++) {
        AxlGfxClip c = region_rect(r, i);
        if (c.x < clip_x1(rect) && rect.x < clip_x1(c) &&
            c.y < clip_y1(rect) && rect.y < clip_y1(c)) {
            return true;
        }
    }
    return false;
}

bool
axl_gfx_region_equal(const AxlGfxRegion *a, const AxlGfxRegion *b)
{
    bool ea = axl_gfx_region_is_empty(a);
    bool eb = axl_gfx_region_is_empty(b);
    if (ea || eb) {
        return ea && eb;   /* all empties (incl. NULL) are equal */
    }
    size_t n = region_n(a);
    if (n != region_n(b)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {   /* canonical -> direct compare */
        AxlGfxClip ca = region_rect(a, i), cb = region_rect(b, i);
        if (ca.x != cb.x || ca.y != cb.y || ca.w != cb.w || ca.h != cb.h) {
            return false;
        }
    }
    return true;
}

size_t
axl_gfx_region_num_rects(const AxlGfxRegion *r)
{
    return r != NULL ? region_n(r) : 0;
}

AxlGfxClip
axl_gfx_region_get_rect(const AxlGfxRegion *r, size_t i)
{
    if (r == NULL || i >= region_n(r)) {
        return (AxlGfxClip){0, 0, 0, 0};
    }
    return region_rect(r, i);
}
