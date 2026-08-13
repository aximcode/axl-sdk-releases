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
#include <axl/axl-cpu.h>
#include <axl/axl-edid.h>
#include <axl/axl-font.h>
#include <axl/axl-log.h>
#include <axl/axl-math.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-gfx.h>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

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
    AxlGfxClip    damage;  /* accumulated dirty bbox; w==0||h==0 == clean */
};

/* Active draw target.  NULL = screen (GOP).  Per substrate discipline,
 * graphics-driver-level state — same precedent as the clip stack. */
static AxlGfxBuffer *target_buf = NULL;

static AxlGfxBuffer *
buffer_new_common(
    uint32_t  w,
    uint32_t  h,
    bool      zeroed
    )
{
    if (w == 0 || h == 0) {
        return NULL;
    }
    AxlGfxBuffer *b = axl_malloc(sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    /* w * h cannot overflow (two uint32_t into a 64-bit size_t on both
     * targets), but the * sizeof(AxlGfxPixel) could -- and the original
     * `axl_malloc(w * h * sizeof(px))` wrapped it silently, yielding a
     * short allocation. Check it on both paths. */
    const size_t n = (size_t)w * h;
    if (n > SIZE_MAX / sizeof(*b->pixels)) {
        axl_free(b);
        return NULL;
    }
    b->pixels = zeroed ? axl_calloc(n, sizeof(*b->pixels))
                       : axl_malloc(n * sizeof(*b->pixels));
    if (b->pixels == NULL) {
        axl_free(b);
        return NULL;
    }
    b->w = w;
    b->h = h;
    b->damage = (AxlGfxClip){0, 0, 0, 0};  /* clean */
    return b;
}

AxlGfxBuffer *
axl_gfx_buffer_new(
    uint32_t  w,
    uint32_t  h
    )
{
    /* Zeroed: axl_gfx_buffer_blur and the compositor read the WHOLE buffer,
     * so undefined bytes get smeared into a shape's halo and presented --
     * leaking heap to the GOP and corrupting the visual. The documented blur
     * example in src/gfx/README.md hits exactly that. */
    return buffer_new_common(w, h, true);
}

AxlGfxBuffer *
axl_gfx_internal_buffer_new_uninit(
    uint32_t  w,
    uint32_t  h
    )
{
    return buffer_new_common(w, h, false);
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
axl_gfx_buffer_fill_rect(
    AxlGfxBuffer  *buf,
    int32_t        x,
    int32_t        y,
    uint32_t       w,
    uint32_t       h,
    AxlGfxPixel    color
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    if (w == 0 || h == 0) {
        return AXL_OK;                      /* empty extent: nothing to write */
    }
    /* Clamp to the buffer in int64 so a huge extent or a very negative
       origin cannot wrap on the way in. */
    int64_t x0 = x, y0 = y;
    int64_t x1 = (int64_t)x + (int64_t)w;   /* half-open */
    int64_t y1 = (int64_t)y + (int64_t)h;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (int64_t)buf->w) { x1 = (int64_t)buf->w; }
    if (y1 > (int64_t)buf->h) { y1 = (int64_t)buf->h; }
    if (x1 <= x0 || y1 <= y0) {
        return AXL_OK;                      /* fully outside: nothing to write */
    }
    /* Raw stores — no clip, no blend mode, no gamma: the whole point is
       that the caller's exact pixel value lands, alpha included. */
    for (int64_t row = y0; row < y1; row++) {
        AxlGfxPixel *p = &buf->pixels[(size_t)row * buf->w + (size_t)x0];
        for (int64_t col = x0; col < x1; col++) {
            *p++ = color;
        }
    }
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
    return axl_gfx_buffer_fill_rect(buf, 0, 0, buf->w, buf->h, color);
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

uint32_t
axl_gfx_pack_pixel(
    AxlGfxPixel       px,
    AxlGfxPixelOrder  order
    )
{
    uint32_t a = px.alpha, r = px.red, g = px.green, b = px.blue;
    /* AxlGfxPixel is stored BGRA, so BGRA is the identity packing (the
       returned word reconstructs the in-memory bit pattern); RGBA swaps
       the red and blue bytes. */
    if (order == AXL_GFX_PIXEL_RGBA) {
        return (a << 24) | (b << 16) | (g << 8) | r;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Map a GOP pixel format to an AxlGfxPixelOrder for direct-framebuffer
   writes.  Returns false for PixelBitMask / PixelBltOnly, which have no
   fixed 32-bit byte order — those fall back to GOP Blt. */
static bool
fb_order_from_gop(
    EFI_GRAPHICS_PIXEL_FORMAT  fmt,
    AxlGfxPixelOrder          *order
    )
{
    switch (fmt) {
    case PixelBlueGreenRedReserved8BitPerColor:
        *order = AXL_GFX_PIXEL_BGRA;
        return true;
    case PixelRedGreenBlueReserved8BitPerColor:
        *order = AXL_GFX_PIXEL_RGBA;
        return true;
    default:
        return false;
    }
}

/* Direct-framebuffer present of a buffer sub-region.  Writes the
   w*h source region at (src_x, src_y) straight to FrameBufferBase at
   (dst_x, dst_y), honoring the GOP scan-line stride.  WRITE-ONLY: VRAM
   is never read back (it is uncached/slow to read).

   The GOP framebuffer is typically write-combining / uncached MMIO, so
   on x86 the rows are written with NON-TEMPORAL streaming stores
   (MOVNTDQ / MOVNTI): they fill the write-combine buffers in full bursts
   without polluting the data cache with pixels we'll never reuse.  A
   single SFENCE after the present flushes the weakly-ordered streaming
   stores before the scanout (or axl_gfx_capture) reads the framebuffer.
   AArch64 uses ordinary stores (a future stnp path could stream there).
   Caller guarantees the region is already clamped to both the buffer and
   the screen extent. */

#if defined(__x86_64__)
/* A 32-bit framebuffer word that may alias the AxlGfxPixel byte array —
   lets the streaming copy read source pixels as words without
   strict-aliasing UB (the destination IS a genuine uint32_t*). */
typedef uint32_t __attribute__((may_alias)) fb_word;

/* Non-temporally copy n 32-bit words to the framebuffer: ordinary stores
   until dst is 16-byte aligned (MOVNTDQ needs alignment), MOVNTDQ for the
   aligned middle, MOVNTI for the tail. */
static inline void
fb_stream_words(
    uint32_t       *dst,
    const fb_word  *src,
    uint32_t        n
    )
{
    uint32_t i = 0;
    while (i < n && ((uintptr_t)(dst + i) & 15u) != 0) {
        _mm_stream_si32((int *)&dst[i], (int)src[i]);
        i++;
    }
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i *)&src[i]);
        _mm_stream_si128((__m128i *)&dst[i], v);
    }
    for (; i < n; i++) {
        _mm_stream_si32((int *)&dst[i], (int)src[i]);
    }
}
#endif

static void
fb_present_rect(
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *g,
    const AxlGfxBuffer            *buf,
    AxlGfxPixelOrder               order,
    uint32_t                       dst_x,
    uint32_t                       dst_y,
    uint32_t                       src_x,
    uint32_t                       src_y,
    uint32_t                       w,
    uint32_t                       h
    )
{
    uint32_t  fb_stride = g->Mode->Info->PixelsPerScanLine;
    uint32_t *fb        = (uint32_t *)(uintptr_t)g->Mode->FrameBufferBase;

    for (uint32_t row = 0; row < h; row++) {
        const AxlGfxPixel *srow = &buf->pixels[(size_t)(src_y + row) * buf->w + src_x];
        uint32_t          *drow = &fb[(size_t)(dst_y + row) * fb_stride + dst_x];
#if defined(__x86_64__)
        if (order == AXL_GFX_PIXEL_BGRA) {
            fb_stream_words(drow, (const fb_word *)srow, w);   /* identity */
        } else {
            for (uint32_t col = 0; col < w; col++) {
                _mm_stream_si32((int *)&drow[col],
                                (int)axl_gfx_pack_pixel(srow[col], order));
            }
        }
#else
        if (order == AXL_GFX_PIXEL_BGRA) {
            /* Identity packing — copy the row's 32-bit words verbatim. */
            axl_memcpy(drow, srow, (size_t)w * sizeof(uint32_t));
        } else {
            for (uint32_t col = 0; col < w; col++) {
                drow[col] = axl_gfx_pack_pixel(srow[col], order);
            }
        }
#endif
    }
#if defined(__x86_64__)
    _mm_sfence();   /* flush WC streaming stores before the FB is read */
#endif
}

/* Shared present path for the whole-buffer, region, and damage variants.
   Clamps the source region to the buffer, then to the screen extent,
   then either writes the framebuffer directly (8-bit BGR/RGB formats
   with a real FrameBufferBase) or falls back to GOP Blt (BitMask /
   BltOnly, or no linear framebuffer). */
static int
buffer_present_region(
    const AxlGfxBuffer  *buf,
    uint32_t             dst_x,
    uint32_t             dst_y,
    uint32_t             src_x,
    uint32_t             src_y,
    uint32_t             w,
    uint32_t             h
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
        axl_debug("buffer present called while target_buf != NULL - "
                  "did you forget axl_gfx_target_buffer(NULL)?");
    }

    /* Clamp the source region to the buffer. */
    if (src_x >= buf->w || src_y >= buf->h) {
        return AXL_OK;  /* origin past the buffer — nothing to present */
    }
    if (w > buf->w - src_x) w = buf->w - src_x;
    if (h > buf->h - src_y) h = buf->h - src_y;

    /* Clamp the destination region to the screen. */
    uint32_t scr_w = g->Mode->Info->HorizontalResolution;
    uint32_t scr_h = g->Mode->Info->VerticalResolution;
    if (dst_x >= scr_w || dst_y >= scr_h) {
        return AXL_OK;  /* fully off-screen */
    }
    if (w > scr_w - dst_x) w = scr_w - dst_x;
    if (h > scr_h - dst_y) h = scr_h - dst_y;
    if (w == 0 || h == 0) {
        return AXL_OK;  /* nothing left after clamping */
    }

    /* Fast path: write the linear framebuffer directly for the two
       8-bit-per-channel formats.  Falls back to Blt for BitMask /
       BltOnly modes, or when no linear framebuffer is exposed. */
    AxlGfxPixelOrder order;
    if (g->Mode->FrameBufferBase != 0
        && fb_order_from_gop(g->Mode->Info->PixelFormat, &order)) {
        fb_present_rect(g, buf, order, dst_x, dst_y, src_x, src_y, w, h);
        return AXL_OK;
    }

    EFI_STATUS status = g->Blt(
        g, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)buf->pixels,
        EfiBltBufferToVideo,
        src_x, src_y,                                  /* source origin */
        dst_x, dst_y,                                  /* destination */
        w, h,                                          /* size */
        buf->w * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL) /* row stride */
        );
    return (status == 0) ? AXL_OK : AXL_ERR;
}

int
axl_gfx_buffer_present(
    const AxlGfxBuffer  *buf,
    uint32_t             dst_x,
    uint32_t             dst_y
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    return buffer_present_region(buf, dst_x, dst_y, 0, 0, buf->w, buf->h);
}

int
axl_gfx_buffer_present_rect(
    const AxlGfxBuffer  *buf,
    uint32_t             dst_x,
    uint32_t             dst_y,
    uint32_t             src_x,
    uint32_t             src_y,
    uint32_t             w,
    uint32_t             h
    )
{
    return buffer_present_region(buf, dst_x, dst_y, src_x, src_y, w, h);
}

// ===================================================================
// Damage tracking (Phase G18) — per-buffer dirty-region accumulator
// ===================================================================

int
axl_gfx_buffer_add_damage(
    AxlGfxBuffer  *buf,
    AxlGfxClip     rect
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    /* Clamp the incoming rect to the buffer extent (int64 math so a
       negative origin plus a large width can't overflow). */
    int64_t x0 = rect.x < 0 ? 0 : rect.x;
    int64_t y0 = rect.y < 0 ? 0 : rect.y;
    int64_t x1 = (int64_t)rect.x + (int64_t)rect.w;
    int64_t y1 = (int64_t)rect.y + (int64_t)rect.h;
    if (x1 > buf->w) x1 = buf->w;
    if (y1 > buf->h) y1 = buf->h;
    if (x1 <= x0 || y1 <= y0) {
        return AXL_OK;  /* empty / fully out of bounds — no contribution */
    }

    if (buf->damage.w == 0 || buf->damage.h == 0) {
        /* First damage since clean — seed the bbox. */
        buf->damage = (AxlGfxClip){
            (int32_t)x0, (int32_t)y0,
            (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)
        };
        return AXL_OK;
    }

    /* Union with the existing bbox. */
    int64_t cx0 = buf->damage.x;
    int64_t cy0 = buf->damage.y;
    int64_t cx1 = (int64_t)buf->damage.x + buf->damage.w;
    int64_t cy1 = (int64_t)buf->damage.y + buf->damage.h;
    if (x0 < cx0) cx0 = x0;
    if (y0 < cy0) cy0 = y0;
    if (x1 > cx1) cx1 = x1;
    if (y1 > cy1) cy1 = y1;
    buf->damage = (AxlGfxClip){
        (int32_t)cx0, (int32_t)cy0,
        (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0)
    };
    return AXL_OK;
}

int
axl_gfx_buffer_get_damage(
    const AxlGfxBuffer  *buf,
    AxlGfxClip          *out
    )
{
    if (buf == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (buf->damage.w == 0 || buf->damage.h == 0) {
        return AXL_ERR;  /* clean — nothing to report */
    }
    *out = buf->damage;
    return AXL_OK;
}

int
axl_gfx_buffer_clear_damage(
    AxlGfxBuffer  *buf
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    buf->damage = (AxlGfxClip){0, 0, 0, 0};
    return AXL_OK;
}

int
axl_gfx_buffer_present_damage(
    AxlGfxBuffer  *buf,
    uint32_t       dst_x,
    uint32_t       dst_y
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    if (buf->damage.w == 0 || buf->damage.h == 0) {
        /* No damage — verify GOP availability for a consistent error
           contract, but otherwise nothing to do. */
        return (gop_get() != NULL) ? AXL_OK : AXL_ERR;
    }
    AxlGfxClip d = buf->damage;
    int rc = buffer_present_region(buf,
                                   dst_x + (uint32_t)d.x, dst_y + (uint32_t)d.y,
                                   (uint32_t)d.x, (uint32_t)d.y, d.w, d.h);
    if (rc == AXL_OK) {
        buf->damage = (AxlGfxClip){0, 0, 0, 0};
    }
    return rc;
}

/* Active blend mode — module-global compositing state (like the draw
 * target and clip stack). Defined here so the buffer writers below can
 * see it. */
static AxlGfxBlendMode blend_mode_current = AXL_GFX_BLEND_OVER;

/* Separable blend function B(Cb, Cs) for one 8-bit channel. */
static uint8_t
blend_chan(
    uint32_t         cb,    /* backdrop */
    uint32_t         cs,    /* source   */
    AxlGfxBlendMode  mode
    )
{
    switch (mode) {
    case AXL_GFX_BLEND_MULTIPLY:
        return (uint8_t)((cb * cs + 127u) / 255u);
    case AXL_GFX_BLEND_SCREEN:
        return (uint8_t)(255u - ((255u - cb) * (255u - cs) + 127u) / 255u);
    case AXL_GFX_BLEND_OVERLAY:
        return (cb < 128u)
            ? (uint8_t)((2u * cb * cs + 127u) / 255u)
            : (uint8_t)(255u - (2u * (255u - cb) * (255u - cs) + 127u) / 255u);
    case AXL_GFX_BLEND_DARKEN:
        return (uint8_t)(cb < cs ? cb : cs);
    case AXL_GFX_BLEND_LIGHTEN:
        return (uint8_t)(cb > cs ? cb : cs);
    case AXL_GFX_BLEND_ADD: {
        uint32_t s = cb + cs;
        return (uint8_t)(s > 255u ? 255u : s);
    }
    case AXL_GFX_BLEND_OVER:
    default:
        return (uint8_t)cs;
    }
}

AxlGfxPixel
axl_gfx_blend_ex(
    AxlGfxPixel      dst,
    AxlGfxPixel      src,
    AxlGfxBlendMode  mode
    )
{
    /* Apply the blend function per channel, then composite over the
     * (opaque) backdrop by the source alpha, in 8-bit integer math:
     *   out.rgb = (B(Cb,Cs) * a + Cb * (255 - a) + 127) / 255
     * Result alpha is always 0xFF (the destination is treated as
     * opaque). For AXL_GFX_BLEND_OVER, B = Cs and this reduces to plain
     * source-over. */
    uint32_t a  = src.alpha;
    uint32_t ia = 255u - a;
    uint32_t bb = blend_chan(dst.blue,  src.blue,  mode);
    uint32_t bg = blend_chan(dst.green, src.green, mode);
    uint32_t br = blend_chan(dst.red,   src.red,   mode);
    AxlGfxPixel out;
    out.blue  = (uint8_t)((bb * a + dst.blue  * ia + 127u) / 255u);
    out.green = (uint8_t)((bg * a + dst.green * ia + 127u) / 255u);
    out.red   = (uint8_t)((br * a + dst.red   * ia + 127u) / 255u);
    out.alpha = 0xFF;
    return out;
}

AxlGfxPixel
axl_gfx_blend(
    AxlGfxPixel  dst,
    AxlGfxPixel  src
    )
{
    return axl_gfx_blend_ex(dst, src, AXL_GFX_BLEND_OVER);
}

// ===================================================================
// Gamma-correct (linear-light) compositing — Phase G15
// ===================================================================

/* Off by default; toggled by axl_gfx_set_gamma_correct.  Module-global
 * compositing state, like blend_mode_current. */
static bool gamma_correct_current = false;

float
axl_gfx_srgb_to_linear(
    uint8_t  srgb
    )
{
    double c = (double)srgb / 255.0;
    double lin = (c <= 0.04045) ? (c / 12.92)
                                : axl_pow((c + 0.055) / 1.055, 2.4);
    return (float)lin;
}

uint8_t
axl_gfx_linear_to_srgb(
    float  linear
    )
{
    double l = (double)linear;
    if (l <= 0.0)  return 0;
    if (l >= 1.0)  return 255;
    double s = (l <= 0.0031308) ? (l * 12.92)
                                : (1.055 * axl_pow(l, 1.0 / 2.4) - 0.055);
    int v = (int)(s * 255.0 + 0.5);
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* Lookup tables for the per-channel linear composite, built once on
 * first gamma-enabled draw.  Decode: 8-bit sRGB -> 16-bit linear
 * (0..65535).  Encode: 12-bit linear (sRGB16 >> 4) -> 8-bit sRGB. */
static uint16_t g_srgb2lin[256];
static uint8_t  g_lin2srgb[4096];
static bool     g_gamma_lut_ready = false;

static void
gamma_lut_init(void)
{
    if (g_gamma_lut_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        float lin = axl_gfx_srgb_to_linear((uint8_t)i);
        int v = (int)(lin * 65535.0f + 0.5f);
        g_srgb2lin[i] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
    }
    for (int j = 0; j < 4096; j++) {
        /* bucket j spans 16-bit linear [j*16, j*16+15]; sample its
           CENTER (+8) so the round-trip error is centered, not biased. */
        g_lin2srgb[j] = axl_gfx_linear_to_srgb((float)((j << 4) + 8) / 65535.0f);
    }
    g_gamma_lut_ready = true;
}

/* Composite one channel in linear light: decode src/dst from sRGB,
 * source-over by alpha in linear, re-encode. */
static inline uint8_t
gamma_composite_chan(
    uint8_t   dst_c,
    uint8_t   src_c,
    uint32_t  a,
    uint32_t  ia
    )
{
    uint32_t dl  = g_srgb2lin[dst_c];
    uint32_t sl  = g_srgb2lin[src_c];
    uint32_t lin = (sl * a + dl * ia + 127u) / 255u;   /* 0..65535 */
    return g_lin2srgb[lin >> 4];
}

/* The drawing layer's compositor.  In sRGB (default) it is exactly
 * axl_gfx_blend_ex (which stays a pure, gamma-agnostic public helper);
 * when gamma-correct mode is on, the source-over alpha composite runs
 * in linear light.  The separable blend FUNCTION B(Cb,Cs) is still
 * evaluated in sRGB — only the final over-composite is linearized. */
static AxlGfxPixel
composite(
    AxlGfxPixel      dst,
    AxlGfxPixel      src,
    AxlGfxBlendMode  mode
    )
{
    if (!gamma_correct_current) {
        return axl_gfx_blend_ex(dst, src, mode);
    }
    /* LUTs are built when gamma mode is enabled (below); this is just a
       cheap ready-check guarding any unexpected entry. */
    gamma_lut_init();
    uint32_t a  = src.alpha;
    uint32_t ia = 255u - a;
    uint8_t  bb = blend_chan(dst.blue,  src.blue,  mode);
    uint8_t  bg = blend_chan(dst.green, src.green, mode);
    uint8_t  br = blend_chan(dst.red,   src.red,   mode);
    AxlGfxPixel out;
    out.blue  = gamma_composite_chan(dst.blue,  bb, a, ia);
    out.green = gamma_composite_chan(dst.green, bg, a, ia);
    out.red   = gamma_composite_chan(dst.red,   br, a, ia);
    out.alpha = 0xFF;
    return out;
}

AxlGfxPixel
axl_gfx_composite(
    AxlGfxPixel  dst,
    AxlGfxPixel  src
    )
{
    /* Public, gamma-aware source-over (honors gamma_correct_current). */
    return composite(dst, src, AXL_GFX_BLEND_OVER);
}

void
axl_gfx_set_gamma_correct(
    bool  enable
    )
{
    if (enable) {
        gamma_lut_init();   /* build the LUTs up front, not per-pixel */
    }
    gamma_correct_current = enable;
}

bool
axl_gfx_get_gamma_correct(void)
{
    return gamma_correct_current;
}

void
axl_gfx_reset_gamma_correct(void)
{
    gamma_correct_current = false;
}

void
axl_gfx_set_blend_mode(
    AxlGfxBlendMode  mode
    )
{
    blend_mode_current = mode;
}

AxlGfxBlendMode
axl_gfx_get_blend_mode(void)
{
    return blend_mode_current;
}

void
axl_gfx_reset_blend_mode(void)
{
    blend_mode_current = AXL_GFX_BLEND_OVER;
}

/* A source pixel takes the fast overwrite path only when it is opaque
 * AND the blend mode is plain source-over; otherwise it routes through
 * axl_gfx_blend_ex so the mode (and/or partial alpha) is honored. */
static bool
blend_is_overwrite(
    AxlGfxPixel  c
    )
{
    return c.alpha == 0xFF && blend_mode_current == AXL_GFX_BLEND_OVER;
}

int
axl_gfx_color_parse(
    const char   *str,
    AxlGfxPixel  *out
    )
{
    if (str == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (str[0] != '#') {
        return AXL_ERR;
    }

    /* Decode the hex body into nibbles up front; this both validates
     * (axl_hex_nibble returns -1 for a non-hex digit) and bounds the
     * length to the four accepted CSS forms.  Nothing is written to
     * *out until every nibble is known good. */
    const char *hex = str + 1;
    size_t      n   = axl_strlen(hex);
    if (n != 3 && n != 4 && n != 6 && n != 8) {
        return AXL_ERR;
    }

    int nib[8];
    for (size_t i = 0; i < n; i++) {
        nib[i] = axl_hex_nibble((unsigned char)hex[i]);
        if (nib[i] < 0) {
            return AXL_ERR;
        }
    }

    uint8_t r, g, b, a;
    if (n == 3 || n == 4) {
        /* Short form: each nibble is doubled (0xN -> 0xNN == N * 17). */
        r = (uint8_t)(nib[0] * 17);
        g = (uint8_t)(nib[1] * 17);
        b = (uint8_t)(nib[2] * 17);
        a = (n == 4) ? (uint8_t)(nib[3] * 17) : 0xFF;
    } else {
        r = (uint8_t)((nib[0] << 4) | nib[1]);
        g = (uint8_t)((nib[2] << 4) | nib[3]);
        b = (uint8_t)((nib[4] << 4) | nib[5]);
        a = (n == 8) ? (uint8_t)((nib[6] << 4) | nib[7]) : 0xFF;
    }

    /* Commit only after full validation — out stays untouched on error.
     * AxlGfxPixel is BGRA in memory; assign by field name (matches the
     * AXL_GFX_RGB macro's r->.red, g->.green, b->.blue convention). */
    out->red   = r;
    out->green = g;
    out->blue  = b;
    out->alpha = a;
    return AXL_OK;
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

/* Blend a constant @a color source-over a contiguous run of @a n
 * destination pixels (scalar reference; the SIMD variants below are
 * bit-identical to it).  out = (color.c*a + dst.c*(255-a) + 127)/255,
 * a = color.alpha, out.alpha = 0xFF — exactly axl_gfx_blend(). */
static void
blend_row_over_scalar(
    AxlGfxPixel        *line,
    uint32_t            n,
    AxlGfxPixel         color
    )
{
    for (uint32_t col = 0; col < n; col++) {
        line[col] = axl_gfx_blend(line[col], color);
    }
}

/* The numerator Y = color.c*a + dst.c*(255-a) + 127 is <= 255*255 + 127
 * = 65152 (a convex combination of two <=255 values, scaled by 255),
 * so it fits a 16-bit lane and floor(Y/255) == (Y + (Y>>8) + 1) >> 8
 * exactly — the identity used by the SIMD paths to divide by 255 with
 * no integer-divide instruction and no rounding drift vs the scalar. */

#if defined(__x86_64__)

/* SSE2 (x86-64 baseline): 2 pixels / iter in 8×u16 lanes.  pmullw /
 * paddw / psrlw / packuswb are all SSE2, so this is the universal x86
 * path (no SSE4.1 needed). */
static void
blend_row_over_sse2(
    AxlGfxPixel  *line,
    uint32_t      n,
    AxlGfxPixel   color
    )
{
    uint16_t a  = color.alpha;
    uint16_t ia = (uint16_t)(255u - a);
    uint16_t kb = (uint16_t)(color.blue  * a + 127u);
    uint16_t kg = (uint16_t)(color.green * a + 127u);
    uint16_t kr = (uint16_t)(color.red   * a + 127u);
    __m128i  v_ia = _mm_set1_epi16((short)ia);
    /* per-channel constant K in BGRA lane order, replicated for 2 px;
       the alpha lane (127) is discarded — alpha is forced to 0xFF. */
    __m128i  v_k  = _mm_setr_epi16((short)kb, (short)kg, (short)kr, 127,
                                   (short)kb, (short)kg, (short)kr, 127);
    __m128i  zero = _mm_setzero_si128();
    __m128i  one  = _mm_set1_epi16(1);
    __m128i  am   = _mm_set1_epi32((int)0xFF000000u);  /* force alpha */
    __m128i  rgbm = _mm_set1_epi32(0x00FFFFFF);

    uint32_t col = 0;
    for (; col + 1 < n; col += 2) {
        __m128i d   = _mm_loadl_epi64((const __m128i *)&line[col]); /* 2 px */
        __m128i d16 = _mm_unpacklo_epi8(d, zero);                   /* 8×u16 */
        __m128i y   = _mm_add_epi16(_mm_mullo_epi16(d16, v_ia), v_k);
        __m128i q   = _mm_srli_epi16(
                          _mm_add_epi16(_mm_add_epi16(y, _mm_srli_epi16(y, 8)), one), 8);
        __m128i p   = _mm_packus_epi16(q, q);            /* low 8 bytes = 2 px */
        p = _mm_or_si128(_mm_and_si128(p, rgbm), am);    /* alpha := 0xFF */
        _mm_storel_epi64((__m128i *)&line[col], p);
    }
    if (col < n) {
        line[col] = axl_gfx_blend(line[col], color);     /* odd tail pixel */
    }
}

/* AVX2: 4 pixels / iter in 16×u16 lanes. */
__attribute__((target("avx2")))
static void
blend_row_over_avx2(
    AxlGfxPixel  *line,
    uint32_t      n,
    AxlGfxPixel   color
    )
{
    uint16_t a  = color.alpha;
    uint16_t ia = (uint16_t)(255u - a);
    short    kb = (short)(color.blue  * a + 127u);
    short    kg = (short)(color.green * a + 127u);
    short    kr = (short)(color.red   * a + 127u);
    __m256i  v_ia = _mm256_set1_epi16((short)ia);
    __m256i  v_k  = _mm256_setr_epi16(kb, kg, kr, 127, kb, kg, kr, 127,
                                      kb, kg, kr, 127, kb, kg, kr, 127);
    __m256i  one  = _mm256_set1_epi16(1);
    __m128i  am   = _mm_set1_epi32((int)0xFF000000u);  /* force alpha */
    __m128i  rgbm = _mm_set1_epi32(0x00FFFFFF);

    uint32_t col = 0;
    for (; col + 3 < n; col += 4) {
        __m128i d   = _mm_loadu_si128((const __m128i *)&line[col]); /* 4 px */
        __m256i d16 = _mm256_cvtepu8_epi16(d);                      /* 16×u16 */
        __m256i y   = _mm256_add_epi16(_mm256_mullo_epi16(d16, v_ia), v_k);
        __m256i q   = _mm256_srli_epi16(
                          _mm256_add_epi16(_mm256_add_epi16(y, _mm256_srli_epi16(y, 8)), one), 8);
        /* packus interleaves the two 128-bit halves; permute back to
           linear pixel order before extracting the low 16 bytes. */
        __m256i pk  = _mm256_packus_epi16(q, q);
        pk = _mm256_permute4x64_epi64(pk, 0xD8);   /* 11 01 10 00 */
        __m128i p   = _mm256_castsi256_si128(pk);  /* 16 bytes = 4 px */
        p = _mm_or_si128(_mm_and_si128(p, rgbm), am);   /* alpha := 0xFF */
        _mm_storeu_si128((__m128i *)&line[col], p);
    }
    for (; col < n; col++) {
        line[col] = axl_gfx_blend(line[col], color);
    }
}

#elif defined(__aarch64__)

/* NEON: 2 pixels / iter in 8×u16 lanes (vmull/vmla-style widen). */
static void
blend_row_over_neon(
    AxlGfxPixel  *line,
    uint32_t      n,
    AxlGfxPixel   color
    )
{
    uint16_t a  = color.alpha;
    uint16_t ia = (uint16_t)(255u - a);
    uint16_t kb = (uint16_t)(color.blue  * a + 127u);
    uint16_t kg = (uint16_t)(color.green * a + 127u);
    uint16_t kr = (uint16_t)(color.red   * a + 127u);
    uint16x8_t v_ia = vdupq_n_u16(ia);
    uint16_t   kbuf[8] = { kb, kg, kr, 127, kb, kg, kr, 127 };
    uint16x8_t v_k  = vld1q_u16(kbuf);
    uint16x8_t one  = vdupq_n_u16(1);
    uint32x4_t am   = vdupq_n_u32(0xFF000000u);
    uint32x4_t rgbm = vdupq_n_u32(0x00FFFFFFu);

    uint32_t col = 0;
    for (; col + 1 < n; col += 2) {
        uint8x8_t  d8  = vreinterpret_u8_u32(vld1_u32((const uint32_t *)&line[col]));
        uint16x8_t d16 = vmovl_u8(d8);                       /* 8×u16, 2 px */
        uint16x8_t y   = vaddq_u16(vmulq_u16(d16, v_ia), v_k);
        uint16x8_t q   = vshrq_n_u16(vaddq_u16(vaddq_u16(y, vshrq_n_u16(y, 8)), one), 8);
        uint8x8_t  p8  = vqmovn_u16(q);                      /* 8×u8 = 2 px */
        uint32x4_t p32 = vreinterpretq_u32_u8(vcombine_u8(p8, p8));
        p32 = vorrq_u32(vandq_u32(p32, rgbm), am);           /* alpha := 0xFF */
        vst1_u32((uint32_t *)&line[col], vget_low_u32(p32));
    }
    if (col < n) {
        line[col] = axl_gfx_blend(line[col], color);
    }
}

#endif

/* A source-over constant-color row kernel. Bit-identical output on every
 * variant; they differ only in width. */
typedef void (*BlendRowOverFn)(
    AxlGfxPixel  *line,
    uint32_t      n,
    AxlGfxPixel   color
);

/* Pick the best row kernel for the CALLING core.
 *
 * Resolve this ONCE PER OPERATION, never per row. axl_cpu_simd_tier() is
 * a deliberately live query -- it answers for the core asking, which is
 * what keeps dispatch correct on a hybrid part where the core running
 * this blit has less ISA than the machine does. Live means CPUID, and
 * together with axl_cpu_enable_avx() one call here executes 4 CPUID
 * leaves plus 2 XGETBV.
 *
 * Under virtualisation CPUID is unconditionally intercepted, so each of
 * those is a VM exit (~1 us under KVM). Called per row, a 1280x800 blend
 * paid ~800 x 6 exits -- and it did, until now: a consumer bisected
 * their compositor's post-key repaint latency going 0 -> 1.75s at 6-way
 * parallelism to exactly this call sitting inside the row loop. Their
 * suite had a 1.5s settle, so the regression presented three repos away
 * as a flaky screenshot harness.
 *
 * Once per operation is EXACTLY as correct as once per row here: UEFI
 * boot services has no preemption and no scheduler, so the BSP cannot
 * migrate cores mid-call, and the answer cannot change underneath a
 * single blit. */
static BlendRowOverFn
blend_row_over_kernel(void)
{
#if defined(__x86_64__)
    if (axl_cpu_enable_avx() && axl_cpu_simd_tier() >= AXL_SIMD_AVX2) {
        return blend_row_over_avx2;
    }
    if (axl_cpu_features()->sse2) {   /* always true on x86-64 */
        return blend_row_over_sse2;
    }
#elif defined(__aarch64__)
    if (axl_cpu_features()->neon) {   /* always true on ARMv8-A */
        return blend_row_over_neon;
    }
#endif
    return blend_row_over_scalar;
}

/* CPU-side blend fill into the active target buffer.  Each destination
 * pixel is replaced with axl_gfx_blend_ex(existing, color, the active
 * blend mode).  The common source-over mode dispatches to a SIMD row
 * kernel; the separable PDF blend modes stay on the scalar path (their
 * per-channel blend function is mode-dependent and not hot). */
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
    /* Fast path: source-over in sRGB → the SIMD row kernel.  Gamma-
       correct mode and the non-OVER modes fall to the scalar composite
       (gamma decode/encode can't run through the sRGB SIMD kernel). */
    if (blend_mode_current == AXL_GFX_BLEND_OVER && !gamma_correct_current) {
        /* Resolved once for the whole rectangle -- see
           blend_row_over_kernel() for why per-row was a VM-exit storm and
           why once-per-operation is equally correct here. */
        const BlendRowOverFn blend_row = blend_row_over_kernel();
        for (uint32_t row = 0; row < h; row++) {
            blend_row(&b->pixels[(y + row) * b->w + x], w, color);
        }
        return;
    }
    for (uint32_t row = 0; row < h; row++) {
        AxlGfxPixel *line = &b->pixels[(y + row) * b->w + x];
        for (uint32_t col = 0; col < w; col++) {
            line[col] = composite(line[col], color, blend_mode_current);
        }
    }
}

/* Convex-quad + path-mask clip helpers (defined with the clip stack
 * below). */
static bool clip_quad_active(void);
static bool clip_quad_test(int32_t x, int32_t y);
static bool clip_mask_active(void);
static bool clip_pixel_visible(int32_t x, int32_t y);  /* quad AND mask */

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
    bool q = clip_quad_active() || clip_mask_active();
    for (uint32_t row = 0; row < h; row++) {
        AxlGfxPixel       *dst_line = &b->pixels[(y + row) * b->w + x];
        const AxlGfxPixel *src_line = &src[(src_dy + row) * src_stride + src_dx];
        for (uint32_t col = 0; col < w; col++) {
            if (q && !clip_pixel_visible((int32_t)(x + col), (int32_t)(y + row))) {
                continue;
            }
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

/* Convex-quad clip: each push_clip_quad contributes up to 4 inward
 * half-planes (inside == nx*X + ny*Y + c >= 0, sampled at pixel
 * centers).  Stored flat in stack order; clip_plane_count[d] is the
 * cumulative number of planes active when clip_depth == d (index 0 is
 * always 0).  A plain push_clip (rect) adds no planes and just carries
 * the parent count forward, so the rect fast path is untouched. */
typedef struct {
    float  nx, ny, c;
} GfxHalfPlane;

#define GFX_CLIP_MAX_PLANES  (AXL_GFX_CLIP_STACK_MAX * 4)
static GfxHalfPlane clip_planes[GFX_CLIP_MAX_PLANES];
static int          clip_plane_count[AXL_GFX_CLIP_STACK_MAX + 1];

/* Path clip (push_clip_path): an arbitrary shape can't be reduced to
 * half-planes, so it's stored as an 8-bit coverage mask over the path's
 * device bounding box.  clip_masks[d] holds the mask pushed at stack
 * index d (cov == NULL when that level is a rect/quad clip).  A pixel
 * passes when it lies inside, and >= 50% covered by, EVERY active mask.
 * The owning AABB rect (also pushed) keeps writers iterating only the
 * mask's bbox, so the per-pixel mask test runs on a tight region. */
typedef struct {
    uint8_t  *cov;        /* coverage[ (y-y0)*w + (x-x0) ], or NULL */
    int32_t   x0, y0;     /* device top-left of the mask */
    int32_t   w, h;       /* mask dimensions (stride == w) */
} GfxClipMask;
static GfxClipMask clip_masks[AXL_GFX_CLIP_STACK_MAX];

/* floor()/ceil() to int32 without libm.  (int32_t) truncates toward
 * zero, so adjust by one in the wrong direction. */
static int32_t
i_floor(double t)
{
    int32_t i = (int32_t)t;
    return ((double)i > t) ? i - 1 : i;
}

static int32_t
i_ceil(double t)
{
    int32_t i = (int32_t)t;
    return ((double)i < t) ? i + 1 : i;
}

/* True when a convex-quad clip narrows the current top of stack. */
static bool
clip_quad_active(void)
{
    return clip_depth > 0 && clip_plane_count[clip_depth] > 0;
}

/* Narrow a single scanline span [*x, *x + *len) at row @a y to the
 * active quad half-planes (pixel centers at (x+0.5, y+0.5)).  Returns
 * false if the row is fully clipped.  Caller has already applied the
 * AABB clip. */
static bool
clip_span_quad(int32_t y, int32_t *x, int32_t *len)
{
    int n = clip_plane_count[clip_depth];
    int32_t lo = *x;
    int32_t hi = *x + *len - 1;
    double  Y  = (double)y + 0.5;

    for (int i = 0; i < n; i++) {
        GfxHalfPlane p = clip_planes[i];
        double base = (double)p.ny * Y + (double)p.c;   /* ny*Y + c */
        if (p.nx > 1e-9) {
            /* x + 0.5 >= -base/nx */
            int32_t lb = i_ceil(-base / (double)p.nx - 0.5);
            if (lb > lo) { lo = lb; }
        } else if (p.nx < -1e-9) {
            /* x + 0.5 <= -base/nx  (divide by negative flips) */
            int32_t ub = i_floor(-base / (double)p.nx - 0.5);
            if (ub < hi) { hi = ub; }
        } else if (base < 0.0) {
            return false;   /* row entirely outside this edge */
        }
    }
    if (lo > hi) {
        return false;
    }
    *x   = lo;
    *len = hi - lo + 1;
    return true;
}

/* Per-pixel form of the quad test (pixel center at (x+0.5, y+0.5)).
 * For writers that loop pixels rather than spans.  Returns true (keep)
 * when no quad clip is active (the plane loop is empty).
 *
 * Must stay arithmetically aligned with clip_span_quad: this evaluates
 * the half-plane dot product directly, while clip_span_quad derives the
 * span endpoints by division — so on a single primitive use one or the
 * other, never both, to avoid a 1-px boundary disagreement. */
static bool
clip_quad_test(int32_t x, int32_t y)
{
    int    n = clip_plane_count[clip_depth];
    double X = (double)x + 0.5;
    double Y = (double)y + 0.5;
    for (int i = 0; i < n; i++) {
        GfxHalfPlane p = clip_planes[i];
        if ((double)p.nx * X + (double)p.ny * Y + (double)p.c < 0.0) {
            return false;
        }
    }
    return true;
}

/* True when any active stack level carries a path-clip mask. */
static bool
clip_mask_active(void)
{
    for (int d = 0; d < clip_depth; d++) {
        if (clip_masks[d].cov != NULL) {
            return true;
        }
    }
    return false;
}

/* True when pixel (x, y) is inside every active path mask (hard edge at
 * 50% coverage).  Outside a mask's bbox counts as outside that clip. */
static bool
clip_mask_test(int32_t x, int32_t y)
{
    for (int d = 0; d < clip_depth; d++) {
        const GfxClipMask *m = &clip_masks[d];
        if (m->cov == NULL) {
            continue;
        }
        if (x < m->x0 || y < m->y0 || x >= m->x0 + m->w || y >= m->y0 + m->h) {
            return false;
        }
        if (m->cov[(size_t)(y - m->y0) * (uint32_t)m->w + (uint32_t)(x - m->x0)] < 128) {
            return false;
        }
    }
    return true;
}

/* Combined per-pixel clip predicate: inside the quad half-planes AND
 * every path mask.  Used by writers that loop pixels. */
static bool
clip_pixel_visible(int32_t x, int32_t y)
{
    return clip_quad_test(x, y) && clip_mask_test(x, y);
}

/* AxlGfxSpanSink that records coverage into a clip mask (clamped to the
 * mask's bbox); @a user is the GfxClipMask being built. */
static void
clip_mask_span_sink(int32_t y, int32_t x, int32_t len, uint8_t coverage, void *user)
{
    GfxClipMask *m = user;
    if (y < m->y0 || y >= m->y0 + m->h) {
        return;
    }
    int32_t x0 = (x < m->x0) ? m->x0 : x;
    int32_t x1 = (x + len > m->x0 + m->w) ? (m->x0 + m->w) : (x + len);
    uint8_t *row = &m->cov[(size_t)(y - m->y0) * (uint32_t)m->w];
    for (int32_t px = x0; px < x1; px++) {
        row[px - m->x0] = coverage;
    }
}

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
    clip_plane_count[clip_depth + 1] = clip_plane_count[clip_depth];
    clip_depth++;
    return AXL_OK;
}

int
axl_gfx_push_clip_quad(
    const AxlGfxPointF  q[4]
    )
{
    if (q == NULL || clip_depth >= AXL_GFX_CLIP_STACK_MAX) {
        return AXL_ERR;
    }

    /* Bounding box, centroid, and signed area in one pass. */
    float  minx = q[0].x, maxx = q[0].x, miny = q[0].y, maxy = q[0].y;
    double gx = 0.0, gy = 0.0, area2 = 0.0;
    for (int i = 0; i < 4; i++) {
        if (q[i].x < minx) { minx = q[i].x; }
        if (q[i].x > maxx) { maxx = q[i].x; }
        if (q[i].y < miny) { miny = q[i].y; }
        if (q[i].y > maxy) { maxy = q[i].y; }
        gx += q[i].x;
        gy += q[i].y;
        int j = (i + 1) & 3;
        area2 += (double)q[i].x * q[j].y - (double)q[j].x * q[i].y;
    }
    gx *= 0.25;
    gy *= 0.25;

    /* Conservative integer AABB, intersected with the parent clip. */
    AxlGfxClip rect;
    int32_t rx = i_floor(minx), ry = i_floor(miny);
    int32_t rr = i_ceil(maxx),  rb = i_ceil(maxy);
    rect.x = rx;
    rect.y = ry;
    rect.w = (rr > rx) ? (uint32_t)(rr - rx) : 0;
    rect.h = (rb > ry) ? (uint32_t)(rb - ry) : 0;
    AxlGfxClip newrect = (clip_depth == 0)
        ? rect : clip_intersect(clip_stack[clip_depth - 1], rect);

    int base = clip_plane_count[clip_depth];

    /* Degenerate (zero-area) quad clips everything: empty rect, no
     * planes — clip_clamp_rect short-circuits on w/h == 0. */
    double area = area2 < 0.0 ? -area2 : area2;
    if (area < 1e-6) {
        newrect.w = 0;
        newrect.h = 0;
        clip_stack[clip_depth] = newrect;
        clip_plane_count[clip_depth + 1] = base;
        clip_depth++;
        return AXL_OK;
    }

    /* Four inward half-planes, oriented toward the centroid so either
     * winding works. */
    int added = 0;
    for (int i = 0; i < 4; i++) {
        int   j  = (i + 1) & 3;
        float ex = q[j].x - q[i].x;
        float ey = q[j].y - q[i].y;
        if (ex == 0.0f && ey == 0.0f) {
            continue;   /* skip a degenerate edge */
        }
        float nx = -ey, ny = ex;                 /* edge normal */
        float c  = -(nx * q[i].x + ny * q[i].y);
        double gv = (double)nx * gx + (double)ny * gy + (double)c;
        if (gv < 0.0) {                          /* point inward */
            nx = -nx; ny = -ny; c = -c;
        }
        if (base + added < GFX_CLIP_MAX_PLANES) {
            clip_planes[base + added] = (GfxHalfPlane){ nx, ny, c };
            added++;
        }
    }

    clip_stack[clip_depth] = newrect;
    clip_plane_count[clip_depth + 1] = base + added;
    clip_depth++;
    return AXL_OK;
}

int
axl_gfx_push_clip_rect_transformed(
    AxlRect             r,
    const AxlTransform  *m
    )
{
    if (m == NULL) {
        return AXL_ERR;
    }
    /* Map the rect corners through the same effective transform the
     * drawing primitives use (CTM × m), then push the device-space
     * quad.  An affine m gives a parallelogram, a projective m a convex
     * quad — both valid for push_clip_quad. */
    AxlTransform eff = axl_transform_multiply(*m, axl_gfx_internal_current_transform());
    AxlVec2 in[4] = {
        axl_vec2(r.x,       r.y),
        axl_vec2(r.x + r.w, r.y),
        axl_vec2(r.x + r.w, r.y + r.h),
        axl_vec2(r.x,       r.y + r.h),
    };
    AxlVec2 c[4];
    axl_transform_map_quad(eff, in, c);
    AxlGfxPointF q[4] = {
        { (float)c[0].x, (float)c[0].y },
        { (float)c[1].x, (float)c[1].y },
        { (float)c[2].x, (float)c[2].y },
        { (float)c[3].x, (float)c[3].y },
    };
    return axl_gfx_push_clip_quad(q);
}

int
axl_gfx_push_clip_path(
    const AxlGfxPath  *path
    )
{
    if (clip_depth >= AXL_GFX_CLIP_STACK_MAX) {
        return AXL_ERR;
    }
    size_t n = 0;
    const AxlGfxVertex *verts = axl_gfx_internal_path_verts(path, &n);
    if (verts == NULL || n < 3) {
        return AXL_ERR;   /* need >= 3 vertices to enclose area (as fill_path) */
    }

    /* Device bounding box of the path. */
    float minx = verts[0].x, maxx = verts[0].x;
    float miny = verts[0].y, maxy = verts[0].y;
    for (size_t i = 1; i < n; i++) {
        if (verts[i].x < minx) { minx = verts[i].x; }
        if (verts[i].x > maxx) { maxx = verts[i].x; }
        if (verts[i].y < miny) { miny = verts[i].y; }
        if (verts[i].y > maxy) { maxy = verts[i].y; }
    }
    /* Clamp to a finite range well inside int32 before the cast — a
     * non-finite or extreme vertex would otherwise make i_floor/i_ceil's
     * double->int32 cast undefined (same guard as axl_gfx_blit_transform).
     * NaN compares false above, so it stays at the seed vertex value. */
    float lo = -1.0e9f, hi = 1.0e9f;
    if (minx < lo) { minx = lo; }  if (maxx > hi) { maxx = hi; }
    if (miny < lo) { miny = lo; }  if (maxy > hi) { maxy = hi; }
    int32_t bx0 = i_floor(minx), by0 = i_floor(miny);
    int32_t bx1 = i_ceil(maxx),  by1 = i_ceil(maxy);

    /* Clamp to the target surface. */
    int32_t dw, dh;
    if (target_buf != NULL) {
        dw = (int32_t)target_buf->w;
        dh = (int32_t)target_buf->h;
    } else {
        AxlGfxInfo gi;
        if (axl_gfx_get_info(&gi) != AXL_OK) {
            return AXL_ERR;
        }
        dw = (int32_t)gi.width;
        dh = (int32_t)gi.height;
    }
    if (bx0 < 0) { bx0 = 0; }
    if (by0 < 0) { by0 = 0; }
    if (bx1 > dw) { bx1 = dw; }
    if (by1 > dh) { by1 = dh; }

    /* Intersect the path bbox with the parent clip so the mask covers
     * only the still-visible region. */
    AxlGfxClip rect = { bx0, by0,
                        (uint32_t)(bx1 > bx0 ? bx1 - bx0 : 0),
                        (uint32_t)(by1 > by0 ? by1 - by0 : 0) };
    AxlGfxClip newrect = (clip_depth == 0)
        ? rect : clip_intersect(clip_stack[clip_depth - 1], rect);

    /* Empty visible region → an empty clip (draws nothing); no mask. */
    if (newrect.w == 0 || newrect.h == 0) {
        clip_stack[clip_depth] = newrect;
        clip_plane_count[clip_depth + 1] = clip_plane_count[clip_depth];
        clip_masks[clip_depth].cov = NULL;
        clip_depth++;
        return AXL_OK;
    }

    /* Rasterize the path (even-odd, as axl_gfx_fill_path) into an 8-bit
     * coverage mask over newrect. */
    int32_t  mw  = (int32_t)newrect.w, mh = (int32_t)newrect.h;
    uint8_t *cov = axl_malloc((size_t)mw * (size_t)mh);
    if (cov == NULL) {
        return AXL_ERR;
    }
    axl_memset(cov, 0, (size_t)mw * (size_t)mh);

    GfxClipMask *m = &clip_masks[clip_depth];
    m->cov = cov;
    m->x0  = newrect.x;
    m->y0  = newrect.y;
    m->w   = mw;
    m->h   = mh;

    if (axl_gfx_rasterize_fill(verts, n, /*even_odd=*/true,
                               clip_mask_span_sink, m) != AXL_OK) {
        axl_free(cov);
        m->cov = NULL;
        return AXL_ERR;
    }

    clip_stack[clip_depth] = newrect;
    clip_plane_count[clip_depth + 1] = clip_plane_count[clip_depth];
    clip_depth++;
    return AXL_OK;
}

int
axl_gfx_pop_clip(void)
{
    if (clip_depth == 0) {
        return AXL_ERR;
    }
    /* Plane slots above the new top are left as-is — readers are bounded
     * by clip_plane_count[clip_depth], and the next push overwrites from
     * that offset, so stale entries are unreachable. */
    clip_depth--;
    if (clip_masks[clip_depth].cov != NULL) {
        axl_free(clip_masks[clip_depth].cov);
        clip_masks[clip_depth].cov = NULL;
    }
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
    for (int d = 0; d < clip_depth; d++) {
        if (clip_masks[d].cov != NULL) {
            axl_free(clip_masks[d].cov);
            clip_masks[d].cov = NULL;
        }
    }
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
static AxlTransform  transform_current = { .m = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0,
}};
static AxlTransform  transform_stack[AXL_GFX_TRANSFORM_STACK_MAX];
static int      transform_depth = 0;

void
axl_gfx_translate(
    double  tx,
    double  ty
    )
{
    transform_current = axl_transform_multiply(axl_transform_translate(tx, ty),
                                     transform_current);
}

void
axl_gfx_scale(
    double  sx,
    double  sy
    )
{
    transform_current = axl_transform_multiply(axl_transform_scale(sx, sy),
                                     transform_current);
}

void
axl_gfx_rotate(
    double  radians
    )
{
    transform_current = axl_transform_multiply(axl_transform_rotate(radians),
                                     transform_current);
}

void
axl_gfx_skew(
    double  sx,
    double  sy
    )
{
    transform_current = axl_transform_multiply(axl_transform_shear(sx, sy),
                                     transform_current);
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

AxlTransform
axl_gfx_get_transform(void)
{
    return transform_current;
}

void
axl_gfx_reset_transform(void)
{
    transform_current = axl_transform_identity();
    transform_depth   = 0;
}

/* Internal helpers exposed via axl-gfx-internal.h for the other
 * gfx-module source files (axl-gfx-path.c et al). */

AxlTransform
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
axl_gfx_get_pixel_format(
    AxlGfxPixelFormat  *out
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (out == NULL || g == NULL || g->Mode == NULL || g->Mode->Info == NULL) {
        return AXL_ERR;
    }
    return axl_gfx_internal_map_pixel_format(g->Mode->Info->PixelFormat, out)
           ? AXL_OK : AXL_ERR;
}

int
axl_gfx_get_pixel_bitmask(
    AxlGfxPixelBitmask  *out
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (out == NULL || g == NULL || g->Mode == NULL || g->Mode->Info == NULL
        || g->Mode->Info->PixelFormat != PixelBitMask) {
        return AXL_ERR;
    }
    const EFI_PIXEL_BITMASK *pi = &g->Mode->Info->PixelInformation;
    out->red_mask      = pi->RedMask;
    out->green_mask    = pi->GreenMask;
    out->blue_mask     = pi->BlueMask;
    out->reserved_mask = pi->ReservedMask;
    return AXL_OK;
}

int
axl_gfx_get_edid(
    const uint8_t  **bytes,
    size_t          *len
    )
{
    if (bytes == NULL || len == NULL) {
        return AXL_ERR;
    }
    /* EFI_EDID_DISCOVERED_PROTOCOL is installed per display by the GOP
       driver when the panel published EDID. Locate the first one — the
       single-display answer; per-output EDID is the multi-output API's
       job. Many GPUs / virtual displays never publish it. */
    EFI_GUID guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    EFI_EDID_DISCOVERED_PROTOCOL *edid = NULL;
    EFI_STATUS st = axl_bs()->LocateProtocol(&guid, NULL, (void **)&edid);
    if (st != 0 || edid == NULL || edid->Edid == NULL || edid->SizeOfEdid == 0) {
        return AXL_ERR;
    }
    *bytes = edid->Edid;
    *len   = edid->SizeOfEdid;
    return AXL_OK;
}

uint32_t
axl_gfx_mode_count(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || g->Mode == NULL) {
        return 0;
    }
    return g->Mode->MaxMode;
}

int
axl_gfx_query_mode(
    uint32_t     index,
    AxlGfxMode  *out
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || g->Mode == NULL || out == NULL
        || index >= g->Mode->MaxMode) {
        return AXL_ERR;
    }

    UINTN                                 size = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi   = NULL;
    EFI_STATUS status = g->QueryMode(g, index, &size, &mi);
    if (status != 0 || mi == NULL) {
        return AXL_ERR;
    }

    out->index  = index;
    out->width  = mi->HorizontalResolution;
    out->height = mi->VerticalResolution;
    out->stride = mi->PixelsPerScanLine;

    /* QueryMode allocates the info via AllocatePool; the caller owns it. */
    axl_bs()->FreePool(mi);  /* axl-pool-direct: GOP QueryMode info buffer */
    return AXL_OK;
}

int
axl_gfx_current_mode(
    uint32_t  *out_index
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || g->Mode == NULL || out_index == NULL) {
        return AXL_ERR;
    }
    *out_index = g->Mode->Mode;
    return AXL_OK;
}

int
axl_gfx_find_mode(
    uint32_t   width,
    uint32_t   height,
    uint32_t  *out_index
    )
{
    uint32_t n = axl_gfx_mode_count();
    if (out_index == NULL || n == 0) {
        return AXL_ERR;
    }
    for (uint32_t i = 0; i < n; i++) {
        AxlGfxMode m;
        if (axl_gfx_query_mode(i, &m) == AXL_OK
            && m.width == width && m.height == height) {
            *out_index = i;
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

int
axl_gfx_max_mode(
    AxlGfxMode  *out
    )
{
    uint32_t n = axl_gfx_mode_count();
    if (out == NULL || n == 0) {
        return AXL_ERR;
    }
    AxlGfxMode best = {0, 0, 0, 0};
    bool       have = false;
    for (uint32_t i = 0; i < n; i++) {
        AxlGfxMode m;
        if (axl_gfx_query_mode(i, &m) != AXL_OK) {
            continue;
        }
        uint64_t area      = (uint64_t)m.width    * (uint64_t)m.height;
        uint64_t best_area = (uint64_t)best.width * (uint64_t)best.height;
        if (!have || area > best_area
            || (area == best_area && m.width > best.width)) {
            best = m;
            have = true;
        }
    }
    if (!have) {
        return AXL_ERR;
    }
    *out = best;
    return AXL_OK;
}

int
axl_gfx_set_mode(
    uint32_t  index
    )
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL || g->Mode == NULL || index >= g->Mode->MaxMode) {
        return AXL_ERR;
    }
    /* SetMode reallocates the framebuffer + clears the screen; g->Mode->*
       (Info, FrameBufferBase) reflect the new mode on return. */
    EFI_STATUS status = g->SetMode(g, index);
    return (status == 0) ? AXL_OK : AXL_ERR;
}

int
axl_gfx_set_native_mode(void)
{
    /* Resolve the native mode index first; only switch once everything
       checks out, so a failure never disturbs the current mode. */
    const uint8_t *bytes = NULL;
    size_t         len   = 0;
    if (axl_gfx_get_edid(&bytes, &len) != AXL_OK) {
        return AXL_ERR;
    }
    AxlEdidInfo info;
    if (axl_edid_parse(bytes, len, &info) != AXL_OK
        || info.native_width == 0 || info.native_height == 0) {
        return AXL_ERR;
    }
    uint32_t idx = 0;
    if (axl_gfx_find_mode(info.native_width, info.native_height, &idx)
            != AXL_OK) {
        return AXL_ERR;
    }
    return axl_gfx_set_mode(idx);
}

int
axl_gfx_get_dpi(
    uint32_t  *dpi_x,
    uint32_t  *dpi_y
    )
{
    const uint8_t *bytes = NULL;
    size_t         len   = 0;
    if (axl_gfx_get_edid(&bytes, &len) != AXL_OK) {
        return AXL_ERR;
    }
    AxlEdidInfo info;
    if (axl_edid_parse(bytes, len, &info) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_edid_dpi(&info, dpi_x, dpi_y);
}

int
axl_gfx_scale_for_dpi(
    uint32_t  dpi
    )
{
    if (dpi >= 240) {
        return 3;
    }
    if (dpi >= 144) {
        return 2;
    }
    return 1;
}

int
axl_gfx_recommended_scale(void)
{
    uint32_t dx = 0, dy = 0;
    if (axl_gfx_get_dpi(&dx, &dy) != AXL_OK) {
        return 1;  /* unknown DPI → no scaling */
    }
    /* Conservative: scale by the smaller axis so a non-square-pixel
       panel doesn't get over-scaled. */
    return axl_gfx_scale_for_dpi(dx < dy ? dx : dy);
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
        if (clip_mask_active()) {
            /* Path-mask clip: arbitrary shape — test each pixel (the mask
             * AABB has already narrowed [x,y,w,h]).  clip_pixel_visible
             * also applies any quad planes. */
            bool over = blend_is_overwrite(color);
            for (uint32_t ry = y; ry < y + h; ry++) {
                for (uint32_t rx = x; rx < x + w; rx++) {
                    if (!clip_pixel_visible((int32_t)rx, (int32_t)ry)) {
                        continue;
                    }
                    size_t idx = (size_t)ry * target_buf->w + rx;
                    target_buf->pixels[idx] = over ? color
                        : composite(target_buf->pixels[idx], color,
                                    blend_mode_current);
                }
            }
        } else if (!clip_quad_active()) {
            if (blend_is_overwrite(color)) {
                buffer_fill_pixels(target_buf, x, y, w, h, color);
            } else {
                buffer_blend_pixels(target_buf, x, y, w, h, color);
            }
        } else {
            /* Quad clip: narrow each scanline to the half-plane span. */
            for (uint32_t ry = y; ry < y + h; ry++) {
                int32_t sx = (int32_t)x, slen = (int32_t)w;
                if (!clip_span_quad((int32_t)ry, &sx, &slen)) {
                    continue;
                }
                if (blend_is_overwrite(color)) {
                    buffer_fill_pixels(target_buf, (uint32_t)sx, ry,
                                       (uint32_t)slen, 1, color);
                } else {
                    buffer_blend_pixels(target_buf, (uint32_t)sx, ry,
                                        (uint32_t)slen, 1, color);
                }
            }
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

    if (clip_mask_active()) {
        /* Path-mask clip on a screen target: per-pixel Blt (rare path —
         * path clips are normally used on a back-buffer). */
        EFI_STATUS status = 0;
        for (uint32_t ry = y; ry < y + h && status == 0; ry++) {
            for (uint32_t rx = x; rx < x + w; rx++) {
                if (!clip_pixel_visible((int32_t)rx, (int32_t)ry)) {
                    continue;
                }
                status = g->Blt(g, &pixel, EfiBltVideoFill, 0, 0, rx, ry, 1, 1, 0);
                if (status != 0) {
                    break;
                }
            }
        }
        return (status == 0) ? AXL_OK : AXL_ERR;
    }

    if (!clip_quad_active()) {
        EFI_STATUS status = g->Blt(
            g, &pixel, EfiBltVideoFill,
            0, 0,   /* source (ignored for fill) */
            x, y,   /* destination */
            w, h,   /* size */
            0       /* delta (ignored for fill) */
            );
        return (status == 0) ? AXL_OK : AXL_ERR;
    }

    /* Quad clip: one Blt per scanline, narrowed to the half-plane span. */
    EFI_STATUS status = 0;
    for (uint32_t ry = y; ry < y + h && status == 0; ry++) {
        int32_t sx = (int32_t)x, slen = (int32_t)w;
        if (!clip_span_quad((int32_t)ry, &sx, &slen)) {
            continue;
        }
        status = g->Blt(g, &pixel, EfiBltVideoFill,
                        0, 0, (uint32_t)sx, ry, (uint32_t)slen, 1, 0);
    }
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
        if (clip_quad_active()) {
            int32_t sx = x, slen = 1;
            if (!clip_span_quad(y, &sx, &slen)) {
                return;
            }
        }
        if (clip_mask_active() && !clip_mask_test(x, y)) {
            return;
        }
        size_t idx = (size_t)(uint32_t)y * target_buf->w + (uint32_t)x;
        if (blend_is_overwrite(color)) {
            target_buf->pixels[idx] = color;
        } else {
            target_buf->pixels[idx] =
                composite(target_buf->pixels[idx], color, blend_mode_current);
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

/* Blit a source pixel buffer to the screen via GOP, honoring an active
 * quad clip by narrowing each scanline (the source x-offset shifts to
 * match).  Falls back to a single Blt when no quad clip is active. */
static EFI_STATUS
blt_buffer_to_video_quad(
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *g,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *src,
    uint32_t  src_stride,    /* pixels per source row */
    uint32_t  src_dx,
    uint32_t  src_dy,
    uint32_t  dst_x,
    uint32_t  dst_y,
    uint32_t  w,
    uint32_t  h
    )
{
    UINTN delta = (UINTN)src_stride * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
    if (clip_mask_active()) {
        /* Path-mask clip on a screen target: per-pixel Blt (rare — path
         * clips are normally used on a back-buffer). */
        EFI_STATUS status = 0;
        for (uint32_t r = 0; r < h && status == 0; r++) {
            for (uint32_t cx = 0; cx < w; cx++) {
                if (!clip_pixel_visible((int32_t)(dst_x + cx),
                                        (int32_t)(dst_y + r))) {
                    continue;
                }
                status = g->Blt(g, src, EfiBltBufferToVideo,
                                src_dx + cx, src_dy + r,
                                dst_x + cx, dst_y + r, 1, 1, delta);
                if (status != 0) {
                    break;
                }
            }
        }
        return status;
    }
    if (!clip_quad_active()) {
        return g->Blt(g, src, EfiBltBufferToVideo, src_dx, src_dy,
                      dst_x, dst_y, w, h, delta);
    }
    EFI_STATUS status = 0;
    for (uint32_t r = 0; r < h && status == 0; r++) {
        int32_t sx = (int32_t)dst_x, slen = (int32_t)w;
        if (!clip_span_quad((int32_t)(dst_y + r), &sx, &slen)) {
            continue;
        }
        uint32_t shift = (uint32_t)sx - dst_x;    /* span trimmed from left */
        status = g->Blt(g, src, EfiBltBufferToVideo,
                        src_dx + shift, src_dy + r,
                        (uint32_t)sx, dst_y + r,
                        (uint32_t)slen, 1, delta);
    }
    return status;
}

/* Shared core for axl_gfx_blit / axl_gfx_blit_rect: copy the w×h region
 * whose top-left in the source is (src_x, src_y), from a source of row
 * stride @a src_stride pixels, to (dst_x, dst_y) on the active target.
 * The destination is clipped to the target bounds and the active clip
 * stack; the clip-induced trim (src_dx, src_dy) is added to the source
 * origin so the correct texels are read after clipping. The caller
 * guarantees the source sub-rect lies within @a buffer.
 *
 * Lenient on zero w/h (no-op, returns AXL_OK) to preserve axl_gfx_blit's
 * historical fill-family contract; the public axl_gfx_blit_rect wrapper
 * layers the stricter zero-dim guard on top. */
static int
blit_impl(
    const AxlGfxPixel  *buffer,
    uint32_t            src_stride,
    uint32_t            src_x,
    uint32_t            src_y,
    uint32_t            dst_x,
    uint32_t            dst_y,
    uint32_t            w,
    uint32_t            h
    )
{
    uint32_t src_dx, src_dy;

    /* Buffer target: CPU memcpy rows. */
    if (target_buf != NULL) {
        if (dst_x >= target_buf->w || dst_y >= target_buf->h) {
            return AXL_OK;
        }
        if (dst_x + w > target_buf->w) w = target_buf->w - dst_x;
        if (dst_y + h > target_buf->h) h = target_buf->h - dst_y;
        if (!clip_clamp_rect(&dst_x, &dst_y, &w, &h, &src_dx, &src_dy)) {
            return AXL_OK;
        }
        buffer_blit_pixels(target_buf, buffer, src_x + src_dx, src_y + src_dy,
                           src_stride, dst_x, dst_y, w, h);
        return AXL_OK;
    }

    /* Screen target: GOP fast path. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g = gop_get();
    if (g == NULL) {
        return AXL_ERR;
    }
    if (!clip_clamp_rect(&dst_x, &dst_y, &w, &h, &src_dx, &src_dy)) {
        return AXL_OK;
    }
    EFI_STATUS status = blt_buffer_to_video_quad(
        g, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)buffer, src_stride,
        src_x + src_dx, src_y + src_dy, dst_x, dst_y, w, h);
    return (status == 0) ? AXL_OK : AXL_ERR;
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
    /* Whole buffer is one tight w×h image: stride = w, source origin 0. */
    return blit_impl(buffer, w, 0, 0, x, y, w, h);
}

int
axl_gfx_blit_rect(
    const AxlGfxPixel  *buffer,
    uint32_t            src_stride,
    uint32_t            src_x,
    uint32_t            src_y,
    uint32_t            dst_x,
    uint32_t            dst_y,
    uint32_t            w,
    uint32_t            h
    )
{
    if (buffer == NULL || w == 0 || h == 0) {
        return AXL_ERR;
    }
    return blit_impl(buffer, src_stride, src_x, src_y, dst_x, dst_y, w, h);
}

/* Bilinear-sample @a src at source coords (@a u, @a v) and composite
 * the result at device pixel (@a dx, @a dy).  Source coords outside the
 * source rectangle contribute nothing (transparent border).
 *
 * NOTE: a SIMD (AVX2, 4×f64) version of the per-pixel 4-channel combine
 * was prototyped and measured ~4.6x SLOWER than this scalar form — the
 * per-pixel set_pd marshalling plus AVX↔SSE transition penalties (a
 * 256-bit op interleaved with the surrounding scalar/SSE code) cost far
 * more than the four multiplies it saves, and the gather + put_pixel
 * dominate regardless.  Kept scalar deliberately. */
static void
blit_sample_put(
    const AxlGfxBuffer  *src,
    int32_t              sw,
    int32_t              sh,
    double               u,
    double               v,
    int32_t              dx,
    int32_t              dy
    )
{
    if (u < 0.0 || u >= (double)sw || v < 0.0 || v >= (double)sh) {
        return;   /* outside the source rectangle */
    }
    /* Bilinear sample; texel centers sit at integer + 0.5. */
    double  fu = u - 0.5, fv = v - 0.5;
    int32_t u0 = i_floor(fu), v0 = i_floor(fv);
    double  au = fu - (double)u0, av = fv - (double)v0;
    int32_t u1 = u0 + 1, v1 = v0 + 1;
    if (u0 < 0) { u0 = 0; }
    if (v0 < 0) { v0 = 0; }
    if (u1 > sw - 1) { u1 = sw - 1; }
    if (v1 > sh - 1) { v1 = sh - 1; }
    if (u0 > sw - 1) { u0 = sw - 1; }
    if (v0 > sh - 1) { v0 = sh - 1; }

    AxlGfxPixel p00 = src->pixels[(size_t)v0 * (uint32_t)sw + (uint32_t)u0];
    AxlGfxPixel p10 = src->pixels[(size_t)v0 * (uint32_t)sw + (uint32_t)u1];
    AxlGfxPixel p01 = src->pixels[(size_t)v1 * (uint32_t)sw + (uint32_t)u0];
    AxlGfxPixel p11 = src->pixels[(size_t)v1 * (uint32_t)sw + (uint32_t)u1];
    double w00 = (1.0 - au) * (1.0 - av), w10 = au * (1.0 - av);
    double w01 = (1.0 - au) * av,         w11 = au * av;

    AxlGfxPixel out;
    out.blue  = (uint8_t)(w00 * p00.blue  + w10 * p10.blue
                        + w01 * p01.blue  + w11 * p11.blue  + 0.5);
    out.green = (uint8_t)(w00 * p00.green + w10 * p10.green
                        + w01 * p01.green + w11 * p11.green + 0.5);
    out.red   = (uint8_t)(w00 * p00.red   + w10 * p10.red
                        + w01 * p01.red   + w11 * p11.red   + 0.5);
    out.alpha = (uint8_t)(w00 * p00.alpha + w10 * p10.alpha
                        + w01 * p01.alpha + w11 * p11.alpha + 0.5);
    put_pixel(dx, dy, out);
}

int
axl_gfx_blit_transform(
    const AxlGfxBuffer    *src,
    const AxlTransform    *m
    )
{
    if (src == NULL || m == NULL) {
        return AXL_ERR;
    }
    if (target_buf == NULL && gop_get() == NULL) {
        return AXL_ERR;
    }

    /* Effective map source->device = m, then the active CTM (cairo
     * a-first composition). */
    AxlTransform eff = axl_transform_multiply(*m, transform_current);

    /* Invert eff to map device->source. Singular -> nothing to draw. */
    AxlTransform inv;
    if (!axl_transform_invert(eff, &inv)) {
        return AXL_OK;
    }

    int32_t sw = (int32_t)src->w;
    int32_t sh = (int32_t)src->h;
    if (sw <= 0 || sh <= 0) {
        return AXL_OK;
    }

    /* Device bounding box from the four transformed source corners —
     * via map_point so the perspective divide is applied for a
     * projective eff (a non-finite corner from a horizon-crossing map is
     * simply ignored by the `<`/`>` scan and the box is clamped to the
     * surface below). */
    AxlVec2 corner[4] = {
        axl_transform_map_point(eff, axl_vec2(0.0,        0.0)),
        axl_transform_map_point(eff, axl_vec2((double)sw, 0.0)),
        axl_transform_map_point(eff, axl_vec2((double)sw, (double)sh)),
        axl_transform_map_point(eff, axl_vec2(0.0,        (double)sh)),
    };
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int k = 0; k < 4; k++) {
        float ex = (float)corner[k].x;
        float ey = (float)corner[k].y;
        if (ex < minx) { minx = ex; }
        if (ex > maxx) { maxx = ex; }
        if (ey < miny) { miny = ey; }
        if (ey > maxy) { maxy = ey; }
    }
    /* If every mapped corner was non-finite (a fully horizon-crossing
     * projection), the min/max never moved off their sentinels — there is
     * nothing on-surface to draw, and bailing here also keeps the
     * sentinel 1e30 out of the int32 cast below. */
    if (maxx < minx || maxy < miny) {
        return AXL_OK;
    }
    /* Clamp to a finite range well inside int32 before the cast: a single
     * horizon-crossing corner maps to ±inf, which would otherwise push the
     * box out of range and make i_floor/i_ceil's double->int32 cast
     * undefined.  (NaN corners are already skipped — NaN compares false in
     * the scan above.) */
    float lo = -1.0e9f, hi = 1.0e9f;
    if (minx < lo) { minx = lo; }  if (maxx > hi) { maxx = hi; }
    if (miny < lo) { miny = lo; }  if (maxy > hi) { maxy = hi; }
    int32_t bx0 = i_floor(minx), by0 = i_floor(miny);
    int32_t bx1 = i_ceil(maxx),  by1 = i_ceil(maxy);

    /* Clamp the iterated box to the target surface (put_pixel still
     * applies the clip stack and per-pixel bounds). */
    int32_t dw, dh;
    if (target_buf != NULL) {
        dw = (int32_t)target_buf->w;
        dh = (int32_t)target_buf->h;
    } else {
        AxlGfxInfo gi;
        if (axl_gfx_get_info(&gi) != AXL_OK) {
            return AXL_ERR;
        }
        dw = (int32_t)gi.width;
        dh = (int32_t)gi.height;
    }
    if (bx0 < 0) { bx0 = 0; }
    if (by0 < 0) { by0 = 0; }
    if (bx1 > dw) { bx1 = dw; }
    if (by1 > dh) { by1 = dh; }

    /* Affine fast path: inverse map is linear, no per-pixel divide.
     * Projective path: inverse-map each device pixel through the full
     * homography (perspective-correct sampling) and skip pixels behind
     * the horizon.  `front_sign` is the sign of the forward w over the
     * source, so the guard works for either orientation of eff. */
    bool affine = axl_transform_is_affine(eff);
    bool front_pos = (eff.m[6] * ((double)sw * 0.5)
                    + eff.m[7] * ((double)sh * 0.5) + eff.m[8]) > 0.0;
    for (int32_t dy = by0; dy < by1; dy++) {
        for (int32_t dx = bx0; dx < bx1; dx++) {
            double px = (double)dx + 0.5, py = (double)dy + 0.5;
            double u, v;
            if (affine) {
                u = inv.m[0] * px + inv.m[1] * py + inv.m[2];
                v = inv.m[3] * px + inv.m[4] * py + inv.m[5];
            } else {
                double w = inv.m[6] * px + inv.m[7] * py + inv.m[8];
                if ((w > 0.0) != front_pos) {
                    continue;   /* behind the horizon — no valid source */
                }
                double iw = 1.0 / w;
                u = (inv.m[0] * px + inv.m[1] * py + inv.m[2]) * iw;
                v = (inv.m[3] * px + inv.m[4] * py + inv.m[5]) * iw;
            }
            blit_sample_put(src, sw, sh, u, v, dx, dy);
        }
    }
    return AXL_OK;
}

int
axl_gfx_fill_pattern(
    int32_t              x,
    int32_t              y,
    int32_t              w,
    int32_t              h,
    const AxlGfxBuffer  *pattern,
    AxlGfxRepeat         repeat
    )
{
    if (pattern == NULL || pattern->w == 0 || pattern->h == 0) {
        return AXL_ERR;
    }
    if (w <= 0 || h <= 0) {
        return AXL_OK;   /* documented no-op */
    }
    /* Screen target with no GOP: nothing to draw to (buffer targets are
       always drawable).  Mirrors the other put_pixel-based primitives. */
    if (target_buf == NULL && gop_get() == NULL) {
        return AXL_ERR;
    }

    uint32_t           pw  = pattern->w;
    uint32_t           ph  = pattern->h;
    const AxlGfxPixel *pp  = pattern->pixels;
    bool tile_x = (repeat == AXL_GFX_REPEAT_BOTH || repeat == AXL_GFX_REPEAT_X);
    bool tile_y = (repeat == AXL_GFX_REPEAT_BOTH || repeat == AXL_GFX_REPEAT_Y);

    /* Offsets (ox, oy) are measured from the rect origin, so the pattern
       is anchored at (x, y): texel (0,0) lands there.  put_pixel applies
       the clip stack, target, blend mode, and per-texel alpha. */
    for (int32_t oy = 0; oy < h; oy++) {
        uint32_t sy;
        if (tile_y) {
            sy = (uint32_t)oy % ph;
        } else {
            if ((uint32_t)oy >= ph) {
                continue;   /* single band on the un-tiled axis */
            }
            sy = (uint32_t)oy;
        }
        const AxlGfxPixel *prow = &pp[(size_t)sy * pw];
        for (int32_t ox = 0; ox < w; ox++) {
            uint32_t sx;
            if (tile_x) {
                sx = (uint32_t)ox % pw;
            } else {
                if ((uint32_t)ox >= pw) {
                    continue;
                }
                sx = (uint32_t)ox;
            }
            put_pixel(x + ox, y + oy, prow[sx]);
        }
    }
    return AXL_OK;
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
    uint32_t           scale,
    bool               quad_clip   /* dst coords are target space: apply quad */
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
                        if (quad_clip && !clip_pixel_visible(px, py)) {
                            continue;
                        }
                        size_t idx = (size_t)(uint32_t)py * dst_stride + (uint32_t)px;
                        /* Honor source alpha: opaque → fast assign;
                           translucent → source-over composite against
                           the existing destination pixel.  Works for
                           both the screen path (captured background in
                           scratch) and the buffer path (live buffer
                           pixels). */
                        if (blend_is_overwrite(fg)) {
                            dst[idx] = fg;
                        } else {
                            dst[idx] = composite(dst[idx], fg, blend_mode_current);
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
            font, text, fg, scale, /* quad_clip */ true);
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
       represents the (x,y,total_w,total_h) screen region).  Screen
       targets do not support blend modes (the final scratch is raw-
       blitted via GOP) — composite source-over so screen text matches
       screen fills, which also ignore the mode.  Render to a buffer for
       blend modes. */
    AxlGfxBlendMode saved_mode = blend_mode_current;
    blend_mode_current = AXL_GFX_BLEND_OVER;
    render_text_glyphs(
        (AxlGfxPixel *)buf, total_w,
        /* origin */ 0, 0,
        /* bounds (scratch-local) */ 0, 0,
        (int32_t)total_w, (int32_t)total_h,
        font, text, fg, scale, /* quad_clip */ false);
    blend_mode_current = saved_mode;

    /* Blit the composited buffer to screen — apply active clip here
       so pixels outside the clip stay untouched. */
    uint32_t dst_x = x, dst_y = y, dst_w = total_w, dst_h = total_h;
    uint32_t src_dx, src_dy;
    if (!clip_clamp_rect(&dst_x, &dst_y, &dst_w, &dst_h, &src_dx, &src_dy)) {
        axl_free(buf);
        return AXL_OK;
    }
    status = blt_buffer_to_video_quad(
        g, buf, total_w, src_dx, src_dy, dst_x, dst_y, dst_w, dst_h);

    axl_free(buf);
    return (status == 0) ? AXL_OK : AXL_ERR;
}

