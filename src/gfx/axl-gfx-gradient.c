/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-gradient.c
    AxlGfx gradients (Phase G5) — linear + radial.

    Per-pixel sampling: each filled pixel's center is mapped to a
    normalized offset t in [0, 1] (axis projection for linear,
    distance/radius for radial), then the color is interpolated
    between the bracketing stops.  Pixels are written through
    `axl_gfx_fill_rect_i(px, py, 1, 1, color)` — the same plotting
    primitive the path rasterizer uses — so clipping, the active draw
    target (screen or buffer), and source-over blending are all
    handled in one place.

    Interpolation is per-channel linear in the stored (sRGB) byte
    values, matching `axl_gfx_blend`.  Gamma-correct interpolation is
    a future refinement (would compose with axl_pow from AxlMath).
**/

#include <stdint.h>

#include <axl/axl-gfx-gradient.h>
#include <axl/axl-gfx-surface.h>   /* axl_gfx_available, get_current_target */
#include <axl/axl-gfx-draw.h>      /* axl_gfx_fill_rect_i */
#include <axl/axl-macros.h>
#include <axl/axl-math.h>          /* axl_sqrt */
#include <axl/axl-mem.h>

// ===================================================================
// Types
// ===================================================================

typedef enum {
    GRAD_LINEAR,
    GRAD_RADIAL
} GradKind;

typedef struct {
    float        t;       ///< offset in [0, 1]
    AxlGfxPixel  color;
} GradStop;

struct AxlGfxGradient {
    GradKind     kind;
    float        x0, y0;   ///< linear: axis start; radial: center
    float        x1, y1;   ///< linear: axis end (unused for radial)
    float        radius;   ///< radial: radius (unused for linear)
    GradStop     stops[AXL_GFX_GRADIENT_MAX_STOPS];
    int          n_stops;
};

// ===================================================================
// Static helpers
// ===================================================================

static uint8_t
lerp_u8_(
    uint8_t  a,
    uint8_t  b,
    float    t,
    bool     linear   /* interpolate in linear light (G15b)? */
    )
{
    if (linear) {
        /* Decode the endpoints to linear light, interpolate, re-encode —
           so the ramp's brightness is perceptually even (no dark dip
           between colors).  Only color channels use this; alpha is
           linear coverage and is always interpolated plainly. */
        float la = axl_gfx_srgb_to_linear(a);
        float lb = axl_gfx_srgb_to_linear(b);
        float lv = la + (lb - la) * t;
        if (lv < 0.0f) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;
        return axl_gfx_linear_to_srgb(lv);
    }
    float v = (float)a + ((float)b - (float)a) * t;
    if (v < 0.0f) {
        v = 0.0f;
    }
    if (v > 255.0f) {
        v = 255.0f;
    }
    return (uint8_t)(v + 0.5f);
}

/* Map a normalized offset to a color via the (sorted) stop list.
 * Caller guarantees n_stops >= 1. */
static AxlGfxPixel
sample_stops_(
    const AxlGfxGradient  *g,
    float                  t
    )
{
    if (t <= g->stops[0].t) {
        return g->stops[0].color;
    }
    if (t >= g->stops[g->n_stops - 1].t) {
        return g->stops[g->n_stops - 1].color;
    }
    for (int i = 0; i < g->n_stops - 1; i++) {
        float lo = g->stops[i].t;
        float hi = g->stops[i + 1].t;
        if (t >= lo && t <= hi) {
            float span = hi - lo;
            float local = (span > 0.0f) ? (t - lo) / span : 0.0f;
            AxlGfxPixel a = g->stops[i].color;
            AxlGfxPixel b = g->stops[i + 1].color;
            /* Color channels honor gamma-correct mode (linear ramp);
               alpha is coverage and always interpolates plainly. */
            bool linear = axl_gfx_get_gamma_correct();
            AxlGfxPixel out;
            out.blue  = lerp_u8_(a.blue,  b.blue,  local, linear);
            out.green = lerp_u8_(a.green, b.green, local, linear);
            out.red   = lerp_u8_(a.red,   b.red,   local, linear);
            out.alpha = lerp_u8_(a.alpha, b.alpha, local, false);
            return out;
        }
    }
    /* Unreachable given the bracket guarantees above. */
    return g->stops[g->n_stops - 1].color;
}

/* Normalized offset for the center of pixel (px, py). */
static float
offset_at_(
    const AxlGfxGradient  *g,
    int32_t                px,
    int32_t                py
    )
{
    float cx = (float)px + 0.5f;
    float cy = (float)py + 0.5f;
    float t;

    if (g->kind == GRAD_LINEAR) {
        float dx = g->x1 - g->x0;
        float dy = g->y1 - g->y0;
        float len2 = dx * dx + dy * dy;
        if (len2 <= 0.0f) {
            return 0.0f;   /* zero-length axis → first stop */
        }
        t = ((cx - g->x0) * dx + (cy - g->y0) * dy) / len2;
    } else {
        if (g->radius <= 0.0f) {
            return 1.0f;   /* degenerate radius → last stop */
        }
        float dx = cx - g->x0;
        float dy = cy - g->y0;
        t = (float)axl_sqrt((double)(dx * dx + dy * dy)) / g->radius;
    }

    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return t;
}

static AxlGfxGradient *
gradient_alloc_(
    GradKind  kind
    )
{
    AxlGfxGradient *g = axl_malloc(sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->kind    = kind;
    g->x0      = 0.0f;
    g->y0      = 0.0f;
    g->x1      = 0.0f;
    g->y1      = 0.0f;
    g->radius  = 0.0f;
    g->n_stops = 0;
    return g;
}

// ===================================================================
// Public API — construction
// ===================================================================

AxlGfxGradient *
axl_gfx_gradient_linear_new(
    float  x0,
    float  y0,
    float  x1,
    float  y1
    )
{
    AxlGfxGradient *g = gradient_alloc_(GRAD_LINEAR);
    if (g == NULL) {
        return NULL;
    }
    g->x0 = x0;
    g->y0 = y0;
    g->x1 = x1;
    g->y1 = y1;
    return g;
}

AxlGfxGradient *
axl_gfx_gradient_radial_new(
    float  cx,
    float  cy,
    float  radius
    )
{
    AxlGfxGradient *g = gradient_alloc_(GRAD_RADIAL);
    if (g == NULL) {
        return NULL;
    }
    g->x0     = cx;
    g->y0     = cy;
    g->radius = radius;
    return g;
}

int
axl_gfx_gradient_add_stop(
    AxlGfxGradient  *g,
    float            t,
    AxlGfxPixel      color
    )
{
    if (g == NULL) {
        return AXL_ERR;
    }
    if (g->n_stops >= AXL_GFX_GRADIENT_MAX_STOPS) {
        return AXL_ERR;
    }
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    /* Insertion sort by offset so sample_stops_ can assume sorted. */
    int i = g->n_stops;
    while (i > 0 && g->stops[i - 1].t > t) {
        g->stops[i] = g->stops[i - 1];
        i--;
    }
    g->stops[i].t     = t;
    g->stops[i].color = color;
    g->n_stops++;
    return AXL_OK;
}

void
axl_gfx_gradient_free(
    AxlGfxGradient  *g
    )
{
    if (g == NULL) {
        return;
    }
    axl_free(g);
}

// ===================================================================
// Public API — sampling
// ===================================================================

AxlGfxPixel
axl_gfx_gradient_sample(
    const AxlGfxGradient  *g,
    int32_t                x,
    int32_t                y
    )
{
    if (g == NULL || g->n_stops == 0) {
        AxlGfxPixel transparent = { 0, 0, 0, 0 };
        return transparent;
    }
    return sample_stops_(g, offset_at_(g, x, y));
}

// ===================================================================
// Public API — fill
// ===================================================================

int
axl_gfx_fill_rect_gradient(
    int32_t                x,
    int32_t                y,
    int32_t                w,
    int32_t                h,
    const AxlGfxGradient  *g
    )
{
    if (g == NULL) {
        return AXL_ERR;
    }
    /* Screen target with no GOP: nothing we can draw to. Mirrors the
     * fill_rect_i contract. A buffer target is always drawable. */
    if (axl_gfx_get_current_target() == NULL && !axl_gfx_available()) {
        return AXL_ERR;
    }
    if (w <= 0 || h <= 0 || g->n_stops == 0) {
        return AXL_OK;   /* documented no-op */
    }

    /* 64-bit bounds: the header allows negative origins and arbitrary
     * extents, so x + w / y + h could overflow int32. fill_rect_i
     * clips each pixel, so out-of-range coords are harmless. */
    int64_t x_end = (int64_t)x + (int64_t)w;
    int64_t y_end = (int64_t)y + (int64_t)h;
    for (int64_t py = y; py < y_end; py++) {
        for (int64_t px = x; px < x_end; px++) {
            AxlGfxPixel color =
                axl_gfx_gradient_sample(g, (int32_t)px, (int32_t)py);
            axl_gfx_fill_rect_i((int32_t)px, (int32_t)py, 1, 1, color);
        }
    }
    return AXL_OK;
}
