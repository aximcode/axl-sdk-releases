/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-effects.c
    AxlGfx raster effects (Phase G6): buffer blur.

    Blur is a separable triangular-kernel convolution — the triangular
    (tent) kernel of half-width r is the standard single-pass Gaussian
    approximation (it equals two box blurs convolved). Run as a
    horizontal pass then a vertical pass, with clamp-to-edge sampling
    so borders don't darken. All four channels (incl. alpha) are
    blurred so the same routine works on shadow/alpha masks.

    The inner loop is a direct O(r) accumulation per pixel (O(w*h*r)
    overall). A future optimization can make it O(1)-per-pixel with the
    incremental stack-blur sliding sums; the kernel/result is identical,
    so that swap is a behavior-preserving refactor under the existing
    tests.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-gfx-effects.h>
#include <axl/axl-gfx-surface.h>
#include <axl/axl-gfx-draw.h>     /* axl_gfx_fill_rect_i — per-pixel composite */
#include <axl/axl-cpu.h>          /* axl_cpu_simd_tier — runtime SIMD dispatch */
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Convolve one axis with a triangular kernel of half-width r.
 * Reads `src`, writes `dst` (both w*h, row-major). `horizontal`
 * selects the axis. Weight for offset k is (r+1 - |k|); the weights
 * sum to (r+1)^2 = div, so the kernel is normalized (energy-
 * preserving) and edge clamp doesn't brighten or darken.
 *
 * This is the scalar reference: every SIMD variant below produces
 * BIT-IDENTICAL output (same per-tap integer products, same
 * (sum+half)/div rounding) — only the per-tap 4-channel multiply-
 * accumulate is vectorized; the index clamp and final divide stay
 * scalar.  blur_pass_() dispatches to the best variant for the CPU. */
static void
blur_pass_scalar(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    bool                horizontal
    )
{
    /* int64 so 255 * (r+1)^2 can't overflow at large radius regardless
     * of the platform's `long` width. */
    int64_t div = (int64_t)(r + 1) * (int64_t)(r + 1);
    int64_t half = div / 2;   /* round-to-nearest */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int64_t sb = 0, sg = 0, sr = 0, sa = 0;
            for (int k = -r; k <= r; k++) {
                int64_t sx = (int64_t)x;
                int64_t sy = (int64_t)y;
                if (horizontal) {
                    sx += k;
                    if (sx < 0)            sx = 0;
                    if (sx > (int64_t)w - 1)  sx = (int64_t)w - 1;
                } else {
                    sy += k;
                    if (sy < 0)            sy = 0;
                    if (sy > (int64_t)h - 1)  sy = (int64_t)h - 1;
                }
                const AxlGfxPixel *s = &src[(size_t)sy * w + (size_t)sx];
                int wgt = (r + 1) - (k < 0 ? -k : k);
                sb += (int64_t)s->blue  * wgt;
                sg += (int64_t)s->green * wgt;
                sr += (int64_t)s->red   * wgt;
                sa += (int64_t)s->alpha * wgt;
            }
            AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x];
            d->blue  = (uint8_t)((sb + half) / div);
            d->green = (uint8_t)((sg + half) / div);
            d->red   = (uint8_t)((sr + half) / div);
            d->alpha = (uint8_t)((sa + half) / div);
        }
    }
}

/* Clamp a 1-D source coordinate for the active axis (shared by the
 * SIMD inner loops; the compiler inlines it). */
static inline int64_t
blur_clamp(
    int64_t  v,
    int64_t  hi
    )
{
    if (v < 0)  return 0;
    if (v > hi) return hi;
    return v;
}

/* A 32-bit view of a pixel's 4 bytes for the SIMD loads.  `may_alias`
 * makes reading an AxlGfxPixel (a struct of uint8_t) through this type
 * well-defined under strict aliasing, while still compiling to a single
 * aligned load (vs a memcpy). */
typedef uint32_t __attribute__((may_alias)) blur_pixword;

#if defined(__x86_64__)

/* SSE4.1 variant: the 4 BGRA channels of one output pixel accumulate
 * in 4×u32 lanes (pmovzxbd widens the 4 source bytes, pmulld does the
 * 32-bit weight multiply — both SSE4.1).  One SIMD MAC replaces four
 * scalar MACs per tap; the accumulator fits u32 for any r the
 * dispatcher allows.  Output is bit-identical to blur_pass_scalar. */
__attribute__((target("sse4.1")))
static void
blur_pass_sse41(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    bool                horizontal
    )
{
    int64_t div = (int64_t)(r + 1) * (int64_t)(r + 1);
    int64_t half = div / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            __m128i acc = _mm_setzero_si128();
            for (int k = -r; k <= r; k++) {
                int64_t sx = horizontal ? blur_clamp((int64_t)x + k, (int64_t)w - 1) : (int64_t)x;
                int64_t sy = horizontal ? (int64_t)y : blur_clamp((int64_t)y + k, (int64_t)h - 1);
                const AxlGfxPixel *s = &src[(size_t)sy * w + (size_t)sx];
                int wgt = (r + 1) - (k < 0 ? -k : k);
                uint32_t word = *(const blur_pixword *)s;   /* [b,g,r,a] LE */
                __m128i  pix  = _mm_cvtepu8_epi32(_mm_cvtsi32_si128((int32_t)word));
                acc = _mm_add_epi32(acc, _mm_mullo_epi32(pix, _mm_set1_epi32(wgt)));
            }
            uint32_t lane[4];
            _mm_storeu_si128((__m128i *)lane, acc);
            AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x];
            d->blue  = (uint8_t)(((int64_t)lane[0] + half) / div);
            d->green = (uint8_t)(((int64_t)lane[1] + half) / div);
            d->red   = (uint8_t)(((int64_t)lane[2] + half) / div);
            d->alpha = (uint8_t)(((int64_t)lane[3] + half) / div);
        }
    }
}

/* AVX2 variant: two adjacent output pixels' 8 channels accumulate in
 * 8×u32 lanes (vpmovzxbd over an 8-byte two-pixel load, vpmulld).
 * Twice the pixels per iteration vs SSE4.1.  An odd trailing pixel is
 * handled by the scalar tail.  Bit-identical to blur_pass_scalar. */
__attribute__((target("avx2")))
static void
blur_pass_avx2(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    bool                horizontal
    )
{
    int64_t div = (int64_t)(r + 1) * (int64_t)(r + 1);
    int64_t half = div / 2;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t x = 0;
        for (; x + 1 < w; x += 2) {
            __m256i acc = _mm256_setzero_si256();
            for (int k = -r; k <= r; k++) {
                int wgt = (r + 1) - (k < 0 ? -k : k);
                int64_t sy0, sy1, sx0, sx1;
                if (horizontal) {
                    sy0 = sy1 = (int64_t)y;
                    sx0 = blur_clamp((int64_t)x + k,     (int64_t)w - 1);
                    sx1 = blur_clamp((int64_t)x + 1 + k, (int64_t)w - 1);
                } else {
                    sx0 = (int64_t)x;
                    sx1 = (int64_t)x + 1;
                    sy0 = sy1 = blur_clamp((int64_t)y + k, (int64_t)h - 1);
                }
                uint32_t w0 = *(const blur_pixword *)&src[(size_t)sy0 * w + (size_t)sx0];
                uint32_t w1 = *(const blur_pixword *)&src[(size_t)sy1 * w + (size_t)sx1];
                /* low 8 bytes = [pixel0 bgra, pixel1 bgra] -> 8×u32 */
                __m128i  packed = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int32_t)w0),
                                                     _mm_cvtsi32_si128((int32_t)w1));
                __m256i  pix    = _mm256_cvtepu8_epi32(packed);
                acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(pix, _mm256_set1_epi32(wgt)));
            }
            uint32_t lane[8];
            _mm256_storeu_si256((__m256i *)lane, acc);
            for (int p = 0; p < 2; p++) {
                AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x + p];
                d->blue  = (uint8_t)(((int64_t)lane[p * 4 + 0] + half) / div);
                d->green = (uint8_t)(((int64_t)lane[p * 4 + 1] + half) / div);
                d->red   = (uint8_t)(((int64_t)lane[p * 4 + 2] + half) / div);
                d->alpha = (uint8_t)(((int64_t)lane[p * 4 + 3] + half) / div);
            }
        }
        /* odd trailing column — scalar (one pixel). */
        for (; x < w; x++) {
            int64_t sb = 0, sg = 0, sr = 0, sa = 0;
            for (int k = -r; k <= r; k++) {
                int64_t sx = horizontal ? blur_clamp((int64_t)x + k, (int64_t)w - 1) : (int64_t)x;
                int64_t sy = horizontal ? (int64_t)y : blur_clamp((int64_t)y + k, (int64_t)h - 1);
                const AxlGfxPixel *s = &src[(size_t)sy * w + (size_t)sx];
                int wgt = (r + 1) - (k < 0 ? -k : k);
                sb += (int64_t)s->blue  * wgt; sg += (int64_t)s->green * wgt;
                sr += (int64_t)s->red   * wgt; sa += (int64_t)s->alpha * wgt;
            }
            AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x];
            d->blue  = (uint8_t)((sb + half) / div); d->green = (uint8_t)((sg + half) / div);
            d->red   = (uint8_t)((sr + half) / div); d->alpha = (uint8_t)((sa + half) / div);
        }
    }
    _mm256_zeroupper();
}

#elif defined(__aarch64__)

/* NEON variant: 4 BGRA channels of one pixel in 4×u32 lanes
 * (vmovl widen, vmla multiply-accumulate).  Bit-identical to
 * blur_pass_scalar. */
static void
blur_pass_neon(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    bool                horizontal
    )
{
    int64_t div = (int64_t)(r + 1) * (int64_t)(r + 1);
    int64_t half = div / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32x4_t acc = vdupq_n_u32(0);
            for (int k = -r; k <= r; k++) {
                int64_t sx = horizontal ? blur_clamp((int64_t)x + k, (int64_t)w - 1) : (int64_t)x;
                int64_t sy = horizontal ? (int64_t)y : blur_clamp((int64_t)y + k, (int64_t)h - 1);
                const AxlGfxPixel *s = &src[(size_t)sy * w + (size_t)sx];
                uint32_t wgt = (uint32_t)((r + 1) - (k < 0 ? -k : k));
                /* widen 4 bytes -> 4×u32 (b,g,r,a). */
                uint32_t word = *(const blur_pixword *)s;
                uint8x8_t  b8  = vreinterpret_u8_u32(vdup_n_u32(word));
                uint16x4_t b16 = vget_low_u16(vmovl_u8(b8));
                uint32x4_t pix = vmovl_u16(b16);
                acc = vmlaq_u32(acc, pix, vdupq_n_u32(wgt));
            }
            uint32_t lane[4];
            vst1q_u32(lane, acc);
            AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x];
            d->blue  = (uint8_t)(((int64_t)lane[0] + half) / div);
            d->green = (uint8_t)(((int64_t)lane[1] + half) / div);
            d->red   = (uint8_t)(((int64_t)lane[2] + half) / div);
            d->alpha = (uint8_t)(((int64_t)lane[3] + half) / div);
        }
    }
}

#endif

/* Dispatch the per-axis convolution to the best SIMD variant the CPU
 * offers.  The u32-lane SIMD accumulators stay exact only while
 * 255*(r+1)^2 < 2^32 (r < 4096); beyond that — and on any CPU without
 * a vector path — fall back to the int64 scalar reference. */
static void
blur_pass_(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    bool                horizontal
    )
{
    if (r < 4096) {
#if defined(__x86_64__)
        if (axl_cpu_enable_avx() && axl_cpu_features()->avx2) {
            blur_pass_avx2(src, dst, w, h, r, horizontal);
            return;
        }
        if (axl_cpu_features()->sse41) {
            blur_pass_sse41(src, dst, w, h, r, horizontal);
            return;
        }
#elif defined(__aarch64__)
        if (axl_cpu_features()->neon) {
            blur_pass_neon(src, dst, w, h, r, horizontal);
            return;
        }
#endif
    }
    blur_pass_scalar(src, dst, w, h, r, horizontal);
}

int
axl_gfx_buffer_blur(
    AxlGfxBuffer  *buf,
    uint32_t       radius
    )
{
    if (buf == NULL) {
        return AXL_ERR;
    }
    if (radius == 0) {
        return AXL_OK;   /* identity */
    }

    uint32_t w = 0, h = 0;
    if (axl_gfx_buffer_get_info(buf, &w, &h) != AXL_OK || w == 0 || h == 0) {
        return AXL_OK;   /* nothing to blur */
    }
    AxlGfxPixel *px = axl_gfx_buffer_pixels(buf);
    if (px == NULL) {
        return AXL_ERR;
    }

    /* Clamp the radius so we never loop past the buffer (clamp-to-edge
     * already makes larger radii meaningless). */
    uint32_t max_dim = (w > h) ? w : h;
    int r = (radius > max_dim) ? (int)max_dim : (int)radius;

    AxlGfxPixel *tmp = axl_malloc((size_t)w * h * sizeof(AxlGfxPixel));
    if (tmp == NULL) {
        return AXL_ERR;
    }

    blur_pass_(px, tmp, w, h, r, true);    /* horizontal: px  -> tmp */
    blur_pass_(tmp, px, w, h, r, false);   /* vertical:   tmp -> px  */

    axl_free(tmp);
    return AXL_OK;
}

int
axl_gfx_draw_shadow(
    const AxlGfxBuffer  *src,
    int32_t              x,
    int32_t              y,
    AxlGfxPixel          color,
    uint32_t             radius
    )
{
    if (src == NULL) {
        return AXL_ERR;
    }
    /* Screen target with no GOP: nothing to composite onto (matches
     * the fill_rect_i contract a buffer target is always drawable). */
    if (axl_gfx_get_current_target() == NULL && !axl_gfx_available()) {
        return AXL_ERR;
    }

    uint32_t sw = 0, sh = 0;
    if (axl_gfx_buffer_get_info(src, &sw, &sh) != AXL_OK || sw == 0 || sh == 0) {
        return AXL_OK;
    }

    /* Pad by the blur radius so the shadow can softly extend past the
     * source's edges. Clamp the pad so the padded buffer can't blow up
     * (consistent with axl_gfx_buffer_blur's own radius clamp). */
    uint32_t max_dim = (sw > sh) ? sw : sh;
    uint32_t pad = (radius > max_dim) ? max_dim : radius;
    uint32_t tw = sw + 2u * pad;
    uint32_t th = sh + 2u * pad;

    AxlGfxBuffer *tmp = axl_gfx_buffer_new(tw, th);
    if (tmp == NULL) {
        return AXL_ERR;
    }

    /* Fill the whole temp with the shadow RGB at zero alpha. Keeping
     * RGB uniform means the blur (which runs on all four channels)
     * leaves color untouched and only softens the alpha mask — no
     * edge darkening. Only the alpha channel carries the shape. */
    AxlGfxPixel base = { color.blue, color.green, color.red, 0 };
    axl_gfx_buffer_clear(tmp, base);

    /* src is read-only here; the pixel accessor takes non-const, so
     * cast away const for the read only (we never modify src). */
    const AxlGfxPixel *sp = axl_gfx_buffer_pixels((AxlGfxBuffer *)src);
    AxlGfxPixel       *tp = axl_gfx_buffer_pixels(tmp);
    if (sp == NULL || tp == NULL) {
        axl_gfx_buffer_free(tmp);
        return AXL_ERR;
    }
    for (uint32_t sy = 0; sy < sh; sy++) {
        for (uint32_t sx = 0; sx < sw; sx++) {
            uint32_t a = ((uint32_t)sp[(size_t)sy * sw + sx].alpha
                          * (uint32_t)color.alpha + 127u) / 255u;
            tp[(size_t)(sy + pad) * tw + (sx + pad)].alpha = (uint8_t)a;
        }
    }

    axl_gfx_buffer_blur(tmp, radius);

    /* Composite the blurred, tinted shadow into the active target at
     * (x - pad, y - pad) so the un-padded content aligns with src@(x,y)
     * and the soft tail extends `pad` beyond. fill_rect_i handles clip,
     * target, and source-over blend. */
    for (uint32_t ty = 0; ty < th; ty++) {
        for (uint32_t tx = 0; tx < tw; tx++) {
            AxlGfxPixel pix = tp[(size_t)ty * tw + tx];
            if (pix.alpha != 0) {
                axl_gfx_fill_rect_i(x - (int32_t)pad + (int32_t)tx,
                                    y - (int32_t)pad + (int32_t)ty,
                                    1, 1, pix);
            }
        }
    }

    axl_gfx_buffer_free(tmp);
    return AXL_OK;
}
