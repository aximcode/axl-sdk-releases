/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx.c
    Basic graphics output — GOP framebuffer operations.

    Locates the UEFI Graphics Output Protocol on first use and wraps
    its Blt() function for fill, blit, and capture operations.
    Falls back gracefully if GOP is not available.
**/

#include "../backend/axl-backend.h"
#include "axl-gfx-internal.h"
#include <axl/axl-font.h>
#include <axl/axl-log.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-gfx.h>

AXL_LOG_DOMAIN("gfx");

/* Built-in font defined in src/gfx/fonts/font-edk2-laffstd.c. */
extern const AxlFont axl_font_edk2_laffstd;

/* Forward declaration: defined below in the GOP section. */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_get(void);

// ===================================================================
// Off-screen pixel buffer (double-buffering)
// ===================================================================

struct AxlGfxBuffer {
    uint32_t      w;
    uint32_t      h;
    AxlGfxPixel  *pixels;  /* w * h elements, row-major */
};

/* Active draw target.  NULL = screen (GOP).  Per substrate discipline,
 * graphics-driver-level state — same precedent as the clip stack. */
static AxlGfxBuffer *target_buf = NULL;

AxlGfxBuffer *
axl_gfx_buffer_new(
    uint32_t  w,
    uint32_t  h
    )
{
    if (w == 0 || h == 0) {
        return NULL;
    }
    AxlGfxBuffer *b = axl_malloc(sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->pixels = axl_malloc((size_t)w * h * sizeof(*b->pixels));
    if (b->pixels == NULL) {
        axl_free(b);
        return NULL;
    }
    b->w = w;
    b->h = h;
    return b;
}

void
axl_gfx_buffer_free(
    AxlGfxBuffer  *buf
    )
{
    if (buf == NULL) {
        return;
    }
    /* Defensive: if caller forgot to reset target, drop it so we don't
       leave a dangling pointer in target_buf after free. */
    if (target_buf == buf) {
        target_buf = NULL;
    }
    axl_free(buf->pixels);
    axl_free(buf);
}

int
axl_gfx_buffer_get_info(
    const AxlGfxBuffer  *buf,
    uint32_t            *out_w,
    uint32_t            *out_h
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    if (out_w != NULL) *out_w = buf->w;
    if (out_h != NULL) *out_h = buf->h;
    return AXL_OK;
}

int
axl_gfx_buffer_clear(
    AxlGfxBuffer  *buf,
    AxlGfxPixel    color
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    size_t n = (size_t)buf->w * buf->h;
    for (size_t i = 0; i < n; i++) {
        buf->pixels[i] = color;
    }
    return AXL_OK;
}

AxlGfxPixel *
axl_gfx_buffer_pixels(
    AxlGfxBuffer  *buf
    )
{
    return (buf != NULL) ? buf->pixels : NULL;
}

void
axl_gfx_target_buffer(
    AxlGfxBuffer  *buf
    )
{
    target_buf = buf;
}

AxlGfxBuffer *
axl_gfx_get_current_target(void)
{
    return target_buf;
}

int
axl_gfx_buffer_present(
    const AxlGfxBuffer  *buf,
    uint32_t             dst_x,
    uint32_t             dst_y
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || buf == NULL) {
        return AXL_ERR;
    }
    /* Almost certainly a caller mistake: presenting while a buffer
       target is active means subsequent draws will still go to the
       old target, not the screen.  Common cause: forgot to call
       axl_gfx_target_buffer(NULL) before present. */
    if (target_buf != NULL) {
        axl_debug("buffer_present called while target_buf != NULL — "
                  "did you forget axl_gfx_target_buffer(NULL)?");
    }
    EFI_STATUS status = g->Blt(
        g, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)buf->pixels,
        EfiBltBufferToVideo,
        0, 0,                                          /* source origin */
        dst_x, dst_y,                                  /* destination */
        buf->w, buf->h,                                /* size */
        buf->w * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) /* row stride */
        );
    return (status == 0) ? AXL_OK : AXL_ERR;
}

AxlGfxPixel
axl_gfx_blend(
    AxlGfxPixel  dst,
    AxlGfxPixel  src
    )
{
    /* Source-over compositing in 8-bit integer math with +127 rounding.
     * out.rgb = (src.rgb * a + dst.rgb * (255 - a) + 127) / 255
     * Result alpha is always 0xFF (we treat the destination as opaque
     * — appropriate for framebuffers and back-buffers; alpha-on-alpha
     * compositing would require a different result-alpha formula). */
    uint32_t a  = src.alpha;
    uint32_t ia = 255u - a;
    AxlGfxPixel out;
    out.blue  = (uint8_t)((src.blue  * a + dst.blue  * ia + 127u) / 255u);
    out.green = (uint8_t)((src.green * a + dst.green * ia + 127u) / 255u);
    out.red   = (uint8_t)((src.red   * a + dst.red   * ia + 127u) / 255u);
    out.alpha = 0xFF;
    return out;
}

/* CPU-side fill into the active target buffer.  Caller has already
 * clipped the rect against target bounds + active clip. */
static void
buffer_fill_pixels(
    AxlGfxBuffer  *b,
    uint32_t       x,
    uint32_t       y,
    uint32_t       w,
    uint32_t       h,
    AxlGfxPixel    color
    )
{
    for (uint32_t row = 0; row < h; row++) {
        AxlGfxPixel *line = &b->pixels[(y + row) * b->w + x];
        for (uint32_t col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

/* CPU-side alpha-blend fill into the active target buffer.  Each
 * destination pixel is replaced with axl_gfx_blend(existing, color). */
static void
buffer_blend_pixels(
    AxlGfxBuffer  *b,
    uint32_t       x,
    uint32_t       y,
    uint32_t       w,
    uint32_t       h,
    AxlGfxPixel    color
    )
{
    for (uint32_t row = 0; row < h; row++) {
        AxlGfxPixel *line = &b->pixels[(y + row) * b->w + x];
        for (uint32_t col = 0; col < w; col++) {
            line[col] = axl_gfx_blend(line[col], color);
        }
    }
}

/* CPU-side blit (memcpy rows) into the active target buffer. */
static void
buffer_blit_pixels(
    AxlGfxBuffer       *b,
    const AxlGfxPixel  *src,
    uint32_t            src_dx,
    uint32_t            src_dy,
    uint32_t            src_stride,   /* in pixels */
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h
    )
{
    for (uint32_t row = 0; row < h; row++) {
        AxlGfxPixel       *dst_line = &b->pixels[(y + row) * b->w + x];
        const AxlGfxPixel *src_line = &src[(src_dy + row) * src_stride + src_dx];
        for (uint32_t col = 0; col < w; col++) {
            dst_line[col] = src_line[col];
        }
    }
}

// ===================================================================
// Clip stack — module-global graphics-driver state.
// Per substrate discipline (docs/AGT-Design.md §"Substrate discipline
// rules"): clipping is graphics-API state (analogous to GL scissor,
// Cairo clip, GDI clip region), NOT widget state — paradigm-agnostic
// because every retained-mode AND immediate-mode toolkit needs it.
// ===================================================================

static AxlGfxClip clip_stack[AXL_GFX_CLIP_STACK_MAX];
static int        clip_depth = 0;

/// Compute axis-aligned intersection of two rects.  Empty result has
/// w == 0 or h == 0.
static AxlGfxClip
clip_intersect(
    AxlGfxClip  a,
    AxlGfxClip  b
    )
{
    AxlGfxClip out;
    int64_t a_right  = (int64_t)a.x + (int64_t)a.w;
    int64_t a_bottom = (int64_t)a.y + (int64_t)a.h;
    int64_t b_right  = (int64_t)b.x + (int64_t)b.w;
    int64_t b_bottom = (int64_t)b.y + (int64_t)b.h;

    out.x = (a.x > b.x) ? a.x : b.x;
    out.y = (a.y > b.y) ? a.y : b.y;
    int64_t right  = (a_right  < b_right)  ? a_right  : b_right;
    int64_t bottom = (a_bottom < b_bottom) ? a_bottom : b_bottom;
    out.w = (right  > out.x) ? (uint32_t)(right  - out.x) : 0;
    out.h = (bottom > out.y) ? (uint32_t)(bottom - out.y) : 0;
    return out;
}

int
axl_gfx_push_clip(
    AxlGfxClip  rect
    )
{
    if (clip_depth >= AXL_GFX_CLIP_STACK_MAX) {
        return AXL_ERR;
    }
    if (clip_depth == 0) {
        clip_stack[0] = rect;
    } else {
        clip_stack[clip_depth] = clip_intersect(clip_stack[clip_depth - 1], rect);
    }
    clip_depth++;
    return AXL_OK;
}

int
axl_gfx_pop_clip(void)
{
    if (clip_depth == 0) {
        return AXL_ERR;
    }
    clip_depth--;
    return AXL_OK;
}

int
axl_gfx_get_clip(
    AxlGfxClip  *out
    )
{
    if (out == NULL || clip_depth == 0) {
        return AXL_ERR;
    }
    *out = clip_stack[clip_depth - 1];
    return AXL_OK;
}

void
axl_gfx_reset_clip(void)
{
    clip_depth = 0;
}

// ===================================================================
// Transform stack — Phase G4.  Active matrix maps local space to
// world (target) space.  Composition matches HTML canvas semantics:
// translate/scale/rotate/skew right-multiply onto the current top.
// push/pop save and restore the entire active matrix.
// ===================================================================

/* Initialized to identity at translation-unit load — no lazy init
 * branch on every public-API entry. */
static AxlMat3  transform_current = { .m = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0,
}};
static AxlMat3  transform_stack[AXL_GFX_TRANSFORM_STACK_MAX];
static int      transform_depth = 0;

void
axl_gfx_translate(
    double  tx,
    double  ty
    )
{
    transform_current = axl_mat3_mul(transform_current,
                                     axl_mat3_translate(tx, ty));
}

void
axl_gfx_scale(
    double  sx,
    double  sy
    )
{
    transform_current = axl_mat3_mul(transform_current,
                                     axl_mat3_scale(sx, sy));
}

void
axl_gfx_rotate(
    double  radians
    )
{
    transform_current = axl_mat3_mul(transform_current,
                                     axl_mat3_rotate(radians));
}

void
axl_gfx_skew(
    double  sx,
    double  sy
    )
{
    transform_current = axl_mat3_mul(transform_current,
                                     axl_mat3_skew(sx, sy));
}

int
axl_gfx_push_transform(void)
{
    if (transform_depth >= AXL_GFX_TRANSFORM_STACK_MAX) {
        return AXL_ERR;
    }
    transform_stack[transform_depth++] = transform_current;
    return AXL_OK;
}

int
axl_gfx_pop_transform(void)
{
    if (transform_depth == 0) {
        return AXL_ERR;
    }
    transform_current = transform_stack[--transform_depth];
    return AXL_OK;
}

AxlMat3
axl_gfx_get_transform(void)
{
    return transform_current;
}

void
axl_gfx_reset_transform(void)
{
    transform_current = axl_mat3_identity();
    transform_depth   = 0;
}

/* Internal helpers exposed via axl-gfx-internal.h for the other
 * gfx-module source files (axl-gfx-path.c et al). */

AxlMat3
axl_gfx_internal_current_transform(void)
{
    return transform_current;
}

/// Clamp a draw rect to the active clip (if any).  On exit, x/y/w/h
/// describe the visible portion; *dx_out / *dy_out are how many pixels
/// were trimmed off the left / top (for adjusting a source buffer
/// pointer in blit/draw_text).  Returns false if fully clipped.
static bool
clip_clamp_rect(
    uint32_t *x,    uint32_t *y,
    uint32_t *w,    uint32_t *h,
    uint32_t *dx_out, uint32_t *dy_out
    )
{
    if (dx_out != NULL) *dx_out = 0;
    if (dy_out != NULL) *dy_out = 0;
    if (clip_depth == 0) {
        return *w > 0 && *h > 0;
    }
    AxlGfxClip c = clip_stack[clip_depth - 1];
    if (c.w == 0 || c.h == 0) {
        return false;
    }
    /* Treat draw rect as (uint x, y, w, h) — origin always >= 0. */
    int64_t rx_l = (int64_t)*x;
    int64_t ry_t = (int64_t)*y;
    int64_t rx_r = rx_l + (int64_t)*w;
    int64_t ry_b = ry_t + (int64_t)*h;
    int64_t cx_l = (int64_t)c.x;
    int64_t cy_t = (int64_t)c.y;
    int64_t cx_r = cx_l + (int64_t)c.w;
    int64_t cy_b = cy_t + (int64_t)c.h;

    int64_t nx_l = (rx_l > cx_l) ? rx_l : cx_l;
    int64_t ny_t = (ry_t > cy_t) ? ry_t : cy_t;
    int64_t nx_r = (rx_r < cx_r) ? rx_r : cx_r;
    int64_t ny_b = (ry_b < cy_b) ? ry_b : cy_b;

    if (nx_r <= nx_l || ny_b <= ny_t) {
        return false;
    }
    if (dx_out != NULL) *dx_out = (uint32_t)(nx_l - rx_l);
    if (dy_out != NULL) *dy_out = (uint32_t)(ny_t - ry_t);
    *x = (uint32_t)nx_l;
    *y = (uint32_t)ny_t;
    *w = (uint32_t)(nx_r - nx_l);
    *h = (uint32_t)(ny_b - ny_t);
    return true;
}

// ===================================================================
// GOP protocol — lazy-init on first use
// ===================================================================

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
static bool gop_probed = false;

static EFI_GRAPHICS_OUTPUT_PROTOCOL *
gop_get(void)
{
    if (gop_probed) {
        return gop;
    }
    gop_probed = true;

    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status = axl_bs()->LocateProtocol(&guid, NULL, (void **)&gop);
    if (status != 0 || gop == NULL) {
        gop = NULL;
        axl_debug("GOP not available (headless or serial-only)");
        return NULL;
    }

    axl_debug("GOP: %ux%u, stride=%u, fb=0x%llx",
             gop->Mode->Info->HorizontalResolution,
             gop->Mode->Info->VerticalResolution,
             gop->Mode->Info->PixelsPerScanLine,
             (unsigned long long)gop->Mode->FrameBufferBase);
    return gop;
}

// ===================================================================
// Public API
// ===================================================================

bool
axl_gfx_available(void)
{
    return gop_get() != NULL;
}

int
axl_gfx_get_info(
    AxlGfxInfo  *info
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || info == NULL) {
        return AXL_ERR;
    }

    info->width       = g->Mode->Info->HorizontalResolution;
    info->height      = g->Mode->Info->VerticalResolution;
    info->stride      = g->Mode->Info->PixelsPerScanLine;
    info->framebuffer = g->Mode->FrameBufferBase;
    return AXL_OK;
}

int
axl_gfx_fill_rect(
    uint32_t     x,
    uint32_t     y,
    uint32_t     w,
    uint32_t     h,
    AxlGfxPixel  color
    )
{
    /* Buffer target: CPU fill or alpha-blend into pixel array.
       Bounds-clamp against buffer dimensions before the clip stack. */
    if (target_buf != NULL) {
        if (color.alpha == 0) {
            return AXL_OK;  /* fully transparent: no-op */
        }
        if (x >= target_buf->w || y >= target_buf->h) {
            return AXL_OK;
        }
        if (x + w > target_buf->w) w = target_buf->w - x;
        if (y + h > target_buf->h) h = target_buf->h - y;
        if (!clip_clamp_rect(&x, &y, &w, &h, NULL, NULL)) {
            return AXL_OK;
        }
        if (color.alpha == 0xFF) {
            buffer_fill_pixels(target_buf, x, y, w, h, color);
        } else {
            buffer_blend_pixels(target_buf, x, y, w, h, color);
        }
        return AXL_OK;
    }

    /* Screen target: GOP fast path. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL) {
        return AXL_ERR;
    }
    if (!clip_clamp_rect(&x, &y, &w, &h, NULL, NULL)) {
        return AXL_OK;
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel;
    pixel.Blue     = color.blue;
    pixel.Green    = color.green;
    pixel.Red      = color.red;
    pixel.Reserved = 0;

    EFI_STATUS status = g->Blt(
        g, &pixel, EfiBltVideoFill,
        0, 0,   /* source (ignored for fill) */
        x, y,   /* destination */
        w, h,   /* size */
        0       /* delta (ignored for fill) */
        );
    return (status == 0) ? AXL_OK : AXL_ERR;
}

int
axl_gfx_fill_rect_i(
    int32_t      x,
    int32_t      y,
    int32_t      w,
    int32_t      h,
    AxlGfxPixel  color
    )
{
    /* Non-positive dimensions: no-op (matches the unsigned variant's
     * lenience for w/h == 0). */
    if (w <= 0 || h <= 0) {
        return AXL_OK;
    }
    /* Clamp negative origins by shifting the rect right/down and
     * shrinking the dimensions accordingly.  If the entire rect is
     * off the top-left after clamping, w/h drops to <= 0 and the
     * second check returns no-op. */
    if (x < 0) {
        w += x;
        x  = 0;
    }
    if (y < 0) {
        h += y;
        y  = 0;
    }
    if (w <= 0 || h <= 0) {
        return AXL_OK;
    }
    return axl_gfx_fill_rect((uint32_t)x, (uint32_t)y,
                             (uint32_t)w, (uint32_t)h, color);
}

/* Plot a single pixel honoring active target + clip + source alpha.
 * Buffer target: direct pixel write (with blend on translucent src).
 * Screen target: routed through fill_rect(1,1) which already applies
 * clip + GOP Blt. */
static void
put_pixel(
    int32_t      x,
    int32_t      y,
    AxlGfxPixel  color
    )
{
    if (color.alpha == 0) {
        return;
    }
    if (target_buf != NULL) {
        if (x < 0 || y < 0 ||
            (uint32_t)x >= target_buf->w ||
            (uint32_t)y >= target_buf->h) {
            return;
        }
        /* Honor active clip in buffer-local coords. */
        if (clip_depth > 0) {
            AxlGfxClip c = clip_stack[clip_depth - 1];
            int64_t cx_r = (int64_t)c.x + (int64_t)c.w;
            int64_t cy_b = (int64_t)c.y + (int64_t)c.h;
            if (x < c.x || y < c.y || x >= cx_r || y >= cy_b) {
                return;
            }
        }
        size_t idx = (size_t)(uint32_t)y * target_buf->w + (uint32_t)x;
        if (color.alpha == 0xFF) {
            target_buf->pixels[idx] = color;
        } else {
            target_buf->pixels[idx] = axl_gfx_blend(target_buf->pixels[idx], color);
        }
        return;
    }
    /* Screen target: per-pixel fill_rect.  Negative coords no-op (GOP
     * coords are unsigned and fill_rect's clip handling clamps). */
    if (x < 0 || y < 0) {
        return;
    }
    axl_gfx_fill_rect((uint32_t)x, (uint32_t)y, 1, 1, color);
}

int
axl_gfx_draw_line(
    int32_t      x0,
    int32_t      y0,
    int32_t      x1,
    int32_t      y1,
    AxlGfxPixel  color
    )
{
    if (target_buf == NULL && gop_get() == NULL) {
        return AXL_ERR;
    }
    /* Standard Bresenham line algorithm.  Per-pixel emission via
     * put_pixel handles clipping, alpha, and target redirection. */
    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;
    while (1) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return AXL_OK;
}

int
axl_gfx_draw_rect(
    uint32_t     x,
    uint32_t     y,
    uint32_t     w,
    uint32_t     h,
    AxlGfxPixel  color
    )
{
    if (w == 0 || h == 0) {
        return AXL_OK;  /* degenerate — nothing to draw */
    }
    /* Four edges as 1-pixel-thick fill_rects.  fill_rect honors
     * active clip + alpha + target, so we get all the right behavior
     * for free.  For w==1 or h==1 the redundant edges still draw the
     * correct 1-wide column / row (slight over-fill of corners). */
    int rc;
    rc = axl_gfx_fill_rect(x,         y,         w, 1, color); if (rc) return rc;
    rc = axl_gfx_fill_rect(x,         y + h - 1, w, 1, color); if (rc) return rc;
    rc = axl_gfx_fill_rect(x,         y,         1, h, color); if (rc) return rc;
    rc = axl_gfx_fill_rect(x + w - 1, y,         1, h, color); if (rc) return rc;
    return AXL_OK;
}

int
axl_gfx_draw_polyline(
    const AxlGfxPoint  *points,
    size_t              count,
    AxlGfxPixel         color
    )
{
    if (points == NULL || count < 2) {
        return AXL_ERR;
    }
    for (size_t i = 1; i < count; i++) {
        int rc = axl_gfx_draw_line(
            points[i - 1].x, points[i - 1].y,
            points[i].x,     points[i].y,
            color);
        if (rc != AXL_OK) {
            return rc;
        }
    }
    return AXL_OK;
}

int
axl_gfx_blit(
    const AxlGfxPixel  *buffer,
    uint32_t            x,
    uint32_t            y,
    uint32_t            w,
    uint32_t            h
    )
{
    if (buffer == NULL) {
        return AXL_ERR;
    }
    /* Preserve original w as the source row stride before any
       clip-induced shrinkage. */
    uint32_t src_w = w;
    uint32_t src_dx, src_dy;

    /* Buffer target: CPU memcpy rows. */
    if (target_buf != NULL) {
        if (x >= target_buf->w || y >= target_buf->h) {
            return AXL_OK;
        }
        if (x + w > target_buf->w) w = target_buf->w - x;
        if (y + h > target_buf->h) h = target_buf->h - y;
        if (!clip_clamp_rect(&x, &y, &w, &h, &src_dx, &src_dy)) {
            return AXL_OK;
        }
        buffer_blit_pixels(target_buf, buffer, src_dx, src_dy, src_w,
                           x, y, w, h);
        return AXL_OK;
    }

    /* Screen target: GOP fast path. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL) {
        return AXL_ERR;
    }
    if (!clip_clamp_rect(&x, &y, &w, &h, &src_dx, &src_dy)) {
        return AXL_OK;
    }
    EFI_STATUS status = g->Blt(
        g, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)buffer,
        EfiBltBufferToVideo,
        src_dx, src_dy,
        x, y,
        w, h,
        src_w * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
        );
    return (status == 0) ? AXL_OK : AXL_ERR;
}

int
axl_gfx_capture(
    AxlGfxPixel  *buffer,
    uint32_t      x,
    uint32_t      y,
    uint32_t      w,
    uint32_t      h
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || buffer == NULL) {
        return AXL_ERR;
    }

    EFI_STATUS status = g->Blt(
        g, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)buffer,
        EfiBltVideoToBltBuffer,
        x, y,   /* source on screen */
        0, 0,   /* destination origin in buffer */
        w, h,   /* size */
        w * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL)  /* delta = row stride */
        );
    return (status == 0) ? AXL_OK : AXL_ERR;
}

// ===================================================================
// Default font + text rendering
// ===================================================================

const AxlFont *
axl_gfx_default_font(void)
{
    return &axl_font_edk2_laffstd;
}

uint32_t
axl_gfx_measure_text(
    const AxlFont  *font,
    const char     *text,
    uint32_t        scale
    )
{
    if (font == NULL || text == NULL || scale == 0) {
        return 0;
    }
    /* UTF-8 input is decoded per-codepoint; sum per-glyph advances so
       variable-width fonts measure correctly.  For monospace fonts
       axl_font_advance returns cell_width regardless of glyph presence,
       so the answer matches (codepoint count * cell_width). */
    uint32_t    total = 0;
    uint32_t    cp;
    size_t      n;
    for (const char *p = text; (n = axl_utf8_decode(p, &cp)) > 0; p += n) {
        total += axl_font_advance(font, cp);
    }
    return total * scale;
}

/* Composite UTF-8 text glyphs into a destination pixel array at
 * (origin_x, origin_y) using @font / @color / @scale.  Pixels outside
 * [bound_x_lo, bound_x_hi) × [bound_y_lo, bound_y_hi) are skipped.
 * Shared between the screen path (origin=0,0 / bounds=scratch buf
 * extent) and the buffer-target path (origin=x,y / bounds=clip-clamped
 * target buffer extent). */
static void
render_text_glyphs(
    AxlGfxPixel       *dst,
    uint32_t           dst_stride,    /* pixels per row in dst */
    int32_t            origin_x,
    int32_t            origin_y,
    int32_t            bound_x_lo,
    int32_t            bound_y_lo,
    int32_t            bound_x_hi,
    int32_t            bound_y_hi,
    const AxlFont     *font,
    const char        *text,
    AxlGfxPixel        fg,
    uint32_t           scale
    )
{
    uint32_t pen_x = 0;
    uint32_t cp;
    size_t   n;
    for (const char *p = text; (n = axl_utf8_decode(p, &cp)) > 0; p += n) {
        const AxlGlyph *glyph = axl_font_glyph(font, cp);

        if (glyph == NULL) {
            pen_x += axl_font_advance(font, cp) * scale;
            continue;
        }

        uint32_t stride = ((uint32_t)glyph->width + 7u) / 8u;
        int32_t  glyph_x = origin_x + (int32_t)pen_x +
                           (int32_t)glyph->x_offset * (int32_t)scale;
        int32_t  glyph_y = origin_y + (int32_t)glyph->y_offset * (int32_t)scale;

        for (uint32_t row = 0; row < glyph->height; row++) {
            for (uint32_t col = 0; col < glyph->width; col++) {
                uint8_t bits = glyph->bitmap[row * stride + (col >> 3)];
                if (!(bits & (0x80 >> (col & 7)))) {
                    continue;
                }
                for (uint32_t sy = 0; sy < scale; sy++) {
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        int32_t px = glyph_x + (int32_t)(col * scale + sx);
                        int32_t py = glyph_y + (int32_t)(row * scale + sy);
                        if (px < bound_x_lo || py < bound_y_lo ||
                            px >= bound_x_hi || py >= bound_y_hi) {
                            continue;
                        }
                        size_t idx = (size_t)(uint32_t)py * dst_stride + (uint32_t)px;
                        /* Honor source alpha: opaque → fast assign;
                           translucent → source-over composite against
                           the existing destination pixel.  Works for
                           both the screen path (captured background in
                           scratch) and the buffer path (live buffer
                           pixels). */
                        if (fg.alpha == 0xFF) {
                            dst[idx] = fg;
                        } else {
                            dst[idx] = axl_gfx_blend(dst[idx], fg);
                        }
                    }
                }
            }
        }
        pen_x += axl_font_advance(font, cp) * scale;
    }
}

int
axl_gfx_draw_text(
    const AxlFont  *font,
    uint32_t        x,
    uint32_t        y,
    const char     *text,
    AxlGfxPixel     color,
    uint32_t        scale
    )
{
    uint32_t  total_w;
    uint32_t  total_h;

    if (font == NULL || text == NULL || scale == 0) {
        return AXL_ERR;
    }
    if (text[0] == '\0') {
        return AXL_OK;
    }

    total_w = axl_gfx_measure_text(font, text, scale);
    total_h = font->cell_height * scale;
    if (total_w == 0) {
        return AXL_OK;
    }

    /* Preserve caller's alpha — render_text_glyphs blends against
       existing destination pixels when fg.alpha < 0xFF. */
    AxlGfxPixel fg = { color.blue, color.green, color.red, color.alpha };
    if (fg.alpha == 0) {
        return AXL_OK;  /* fully transparent text: no-op */
    }

    /* Buffer target: composite glyphs DIRECTLY into the target buffer's
       pixel array.  Existing pixels serve as background — no screen
       capture, no scratch alloc, no final blit. */
    if (target_buf != NULL) {
        /* Compute the on-target visible rectangle clamped to buffer
           bounds AND active clip.  Pixels outside this rect are skipped
           by render_text_glyphs's bound checks. */
        if (x >= target_buf->w || y >= target_buf->h) {
            return AXL_OK;
        }
        uint32_t tw = total_w, th = total_h;
        if (x + tw > target_buf->w) tw = target_buf->w - x;
        if (y + th > target_buf->h) th = target_buf->h - y;
        uint32_t cx = x, cy = y;
        if (!clip_clamp_rect(&cx, &cy, &tw, &th, NULL, NULL)) {
            return AXL_OK;
        }
        render_text_glyphs(
            target_buf->pixels, target_buf->w,
            /* origin = where the text starts in target coordinates */
            (int32_t)x, (int32_t)y,
            /* visible bounds = clip-clamped rect in target coordinates */
            (int32_t)cx, (int32_t)cy,
            (int32_t)(cx + tw), (int32_t)(cy + th),
            font, text, fg, scale);
        return AXL_OK;
    }

    /* Screen target: scratch-alloc, capture existing screen content,
       composite, then blit back via GOP. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL) {
        return AXL_ERR;
    }
    /* Clamp to screen bounds */
    {
        AxlGfxInfo scr;
        if (axl_gfx_get_info(&scr) == AXL_OK) {
            if (x >= scr.width || y >= scr.height) {
                return AXL_ERR;
            }
            if (x + total_w > scr.width) total_w = scr.width - x;
            if (y + total_h > scr.height) total_h = scr.height - y;
        }
    }

    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buf =
        axl_malloc((size_t)total_w * total_h * sizeof(*buf));
    if (buf == NULL) {
        return AXL_ERR;
    }

    /* Capture existing screen content as background */
    EFI_STATUS status = g->Blt(
        g, buf, EfiBltVideoToBltBuffer,
        x, y, 0, 0, total_w, total_h,
        total_w * sizeof(*buf)
        );
    if (status != 0) {
        size_t npixels = (size_t)total_w * total_h;
        for (size_t pi = 0; pi < npixels; pi++) {
            buf[pi].Blue = 0;
            buf[pi].Green = 0;
            buf[pi].Red = 0;
            buf[pi].Reserved = 0;
        }
    }

    /* Composite glyphs into scratch buf (origin 0,0 since buf
       represents the (x,y,total_w,total_h) screen region). */
    render_text_glyphs(
        (AxlGfxPixel *)buf, total_w,
        /* origin */ 0, 0,
        /* bounds (scratch-local) */ 0, 0,
        (int32_t)total_w, (int32_t)total_h,
        font, text, fg, scale);

    /* Blit the composited buffer to screen — apply active clip here
       so pixels outside the clip stay untouched. */
    uint32_t dst_x = x, dst_y = y, dst_w = total_w, dst_h = total_h;
    uint32_t src_dx, src_dy;
    if (!clip_clamp_rect(&dst_x, &dst_y, &dst_w, &dst_h, &src_dx, &src_dy)) {
        axl_free(buf);
        return AXL_OK;
    }
    status = g->Blt(
        g, buf, EfiBltBufferToVideo,
        src_dx, src_dy,
        dst_x, dst_y,
        dst_w, dst_h,
        total_w * sizeof(*buf)
        );

    axl_free(buf);
    return (status == 0) ? AXL_OK : AXL_ERR;
}

