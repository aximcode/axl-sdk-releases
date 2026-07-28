/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-gfx-effects.c
    AxlGfx raster effects (Phase G6): buffer blur.

    Blur is a separable triangular-kernel convolution — the triangular
    (tent) kernel of half-width r is the standard single-pass Gaussian
    approximation. Run as one axis pass then the other, with
    clamp-to-edge sampling so borders don't darken. All four channels
    (incl. alpha) are blurred so the same routine works on shadow/alpha
    masks.

    The tent kernel factors EXACTLY into two box blurs of width r+1
    convolved, and a box blur is a sliding-window running sum — O(1) per
    sample regardless of r. So each axis pass runs as two running-sum box
    passes: O(w*h) per pass, INDEPENDENT of the radius. (The old direct
    O(w*h*r) tent convolution pegged a core on full-screen backdrop
    veils. Note a UEFI app is NOT stuck on the scalar path: firmware
    already enables XMM for the x64 calling convention, so SSE is usable
    as-is, and AVX needs only axl_cpu_enable_avx.)
    Carrying the box-box intermediate at full int64 precision — no
    rounding between the two boxes — reproduces the tent's weighted sum
    bit-for-bit, so the output is byte-identical to the old kernel.

    Three things then cut the constant factor, all BIT-EXACT: the
    per-pixel round-and-divide is a multiply-shift reciprocal rather
    than four int64 divisions (see blur_recip_make), the second axis
    runs as a transpose plus another ROW pass (a strided column walk
    touches one pixel per cache line — see blur_transpose), and the row
    pass itself is SIMD-dispatched over the four BGRA channels (see
    blur_pass_boxbox_sse41). All of it is validated against an
    independent tent reference by test/integration/gfx-simd-selftest.c.
**/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-gfx-effects.h>
#include <axl/axl-gfx-surface.h>
#include <axl/axl-gfx-draw.h>     /* axl_gfx_fill_rect_i — per-pixel composite */
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__x86_64__) || defined(__aarch64__)
#include <axl/axl-cpu.h>          /* axl_cpu_features — runtime SIMD dispatch */
#define AXL_BLUR_SIMD 1
#endif

/* Clamp an index to [0, hi] for edge-replicated (clamp-to-edge)
 * sampling. Force-inlined: it is called per index in the blur's hot loop and
 * -Os (the release build) otherwise leaves it a real call. */
__attribute__((always_inline)) static inline int64_t
blur_clampi(
    int64_t  v,
    int64_t  hi
    )
{
    if (v < 0)  return 0;
    if (v > hi) return hi;
    return v;
}

/* Exact stand-in for `n / div` over the value range this kernel produces.
 *
 * The final round-and-divide was the single most expensive thing in the blur
 * — FOUR int64 divisions per output pixel, and `div` is a runtime value so
 * the compiler cannot strength-reduce it (measured at ~60% of a full-screen
 * 1280x800 blur). A multiply-shift reciprocal replaces each with one multiply
 * and one shift.
 *
 * Exactness (Granlund-Montgomery): with M = floor(2^S / div) + 1 and
 * e = M*div - 2^S (so 1 <= e <= div), floor(n*M / 2^S) == floor(n / div) for
 * every 0 <= n with n*e < 2^S. `blur_recip_make` checks that against the
 * largest n the kernel can present, and also that n*M cannot overflow 64
 * bits; if either fails (only for a preposterous radius — div > ~1.2e7, i.e.
 * a radius past 3443 on an image at least that wide) it leaves `mul` 0 and
 * the plain division stands. So the result is bit-identical either way, which
 * is what the tent oracle in gfx-simd-selftest.c pins. */
#define BLUR_RECIP_SHIFT 55

typedef struct {
    uint64_t  mul;   /* floor(2^BLUR_RECIP_SHIFT / div) + 1; 0 = divide instead */
    int64_t   div;
} BlurRecip;

static BlurRecip
blur_recip_make(
    int64_t  div,
    int64_t  nmax    ///< largest dividend the caller can present
    )
{
    BlurRecip rp = { 0, div };
    const uint64_t one = (uint64_t)1 << BLUR_RECIP_SHIFT;
    uint64_t m = one / (uint64_t)div + 1;
    uint64_t e = m * (uint64_t)div - one;          /* 1 <= e <= div */
    if ((uint64_t)nmax <= (one - 1) / e && (uint64_t)nmax <= UINT64_MAX / m) {
        rp.mul = m;
    }
    return rp;
}

/* Force-inlined: one per channel per output pixel, and -Os would otherwise
 * leave it a real call. */
__attribute__((always_inline)) static inline uint8_t
blur_round(
    int64_t           n,
    const BlurRecip  *rp
    )
{
    if (rp->mul != 0) {
        return (uint8_t)(((uint64_t)n * rp->mul) >> BLUR_RECIP_SHIFT);
    }
    return (uint8_t)(n / rp->div);
}

/* One ROW pass, bit-exact with a triangular (tent) kernel of half-width
 * r but O(w) per row instead of O(w*r).
 *
 * The tent of half-width r equals box(r+1) convolved with box(r+1)
 * (weights [1,2,..,r,r+1,r,..,2,1], summing to (r+1)^2 = div). Each box
 * is a sliding window: stepping one sample adds the entering value and
 * subtracts the leaving one. Two boxes per axis reproduce the tent; the
 * intermediate I1[] is kept at full int64 precision (no rounding between
 * the boxes) so the composition equals the tent's raw weighted sum, then
 * the single (sum+half)/div round matches the old kernel EXACTLY.
 *
 * Edge exactness is the one subtlety: box2 sums I1[x-r .. x], which for x
 * near 0 reaches I1[t] at t down to -r. Those entries are the box1 window
 * sums of the EDGE-CLAMPED source at t = -r .. -1 — NOT I1[0] re-clamped.
 * So i1[] carries r extra leading entries (index p = t + r, t in
 * [-r, w-1]); re-clamping an I1 index instead would drop/duplicate the
 * wrong samples and differ from the tent by a few LSB within r of a border.
 *
 * Rows only: the SECOND axis is run by transposing and calling this again
 * (see axl_gfx_buffer_blur), so every access here is unit-stride. `i1` is
 * caller scratch of at least (max(w,h) + r) * 4 int32 (4 interleaved
 * channels), reused across lines and both passes. */
static void
blur_pass_boxbox(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    int32_t            *i1
    )
{
    const int64_t   div  = (int64_t)(r + 1) * (int64_t)(r + 1);
    const int64_t   half = div / 2;                 /* round-to-nearest */
    const int64_t   hi   = (int64_t)w - 1;
    const uint32_t  nI1  = w + (uint32_t)r;         /* I1 entries: t in [-r, w-1] */
    /* A box2 sum is at most 255*div (weights summing to div, samples <= 255). */
    const BlurRecip rp   = blur_recip_make(div, 255 * div + half);

    for (uint32_t l = 0; l < h; l++) {
        const AxlGfxPixel *s0 = &src[(size_t)l * w];
        AxlGfxPixel       *d0 = &dst[(size_t)l * w];

        /* box1: I1[t] = sum_{j=0..r} S[clamp(t+j)], stored at i1[(t+r)*4].
         * Seed the r+1-wide window at t = -r, then slide by one. */
        int64_t cb = 0, cg = 0, cr = 0, ca = 0;
        for (int j = 0; j <= r; j++) {
            const AxlGfxPixel *s = &s0[blur_clampi((int64_t)j - r, hi)];
            cb += s->blue; cg += s->green; cr += s->red; ca += s->alpha;
        }
        for (uint32_t p = 0; p < nI1; p++) {
            i1[(size_t)p * 4 + 0] = (int32_t)cb;
            i1[(size_t)p * 4 + 1] = (int32_t)cg;
            i1[(size_t)p * 4 + 2] = (int32_t)cr;
            i1[(size_t)p * 4 + 3] = (int32_t)ca;
            if (p + 1 < nI1) {
                /* t -> t+1 (t = p - r): + S[clamp(p+1)], - S[clamp(p-r)] */
                const AxlGfxPixel *a = &s0[blur_clampi((int64_t)p + 1, hi)];
                const AxlGfxPixel *e = &s0[blur_clampi((int64_t)p - r, hi)];
                cb += (int64_t)a->blue  - e->blue;
                cg += (int64_t)a->green - e->green;
                cr += (int64_t)a->red   - e->red;
                ca += (int64_t)a->alpha - e->alpha;
            }
        }

        /* box2: out[x] = sum_{p=x..x+r} i1[p], rounded once. Seed at x = 0
         * (p in [0, r]), then slide. */
        int64_t bb = 0, bg = 0, br = 0, ba = 0;
        for (uint32_t p = 0; p <= (uint32_t)r; p++) {
            bb += i1[(size_t)p * 4 + 0]; bg += i1[(size_t)p * 4 + 1];
            br += i1[(size_t)p * 4 + 2]; ba += i1[(size_t)p * 4 + 3];
        }
        for (uint32_t x = 0; x < w; x++) {
            AxlGfxPixel *d = &d0[x];
            d->blue  = blur_round(bb + half, &rp);
            d->green = blur_round(bg + half, &rp);
            d->red   = blur_round(br + half, &rp);
            d->alpha = blur_round(ba + half, &rp);
            if (x + 1 < w) {
                /* box2[x+1] = box2[x] + i1[x+1+r] - i1[x] */
                uint32_t pa = x + 1 + (uint32_t)r;
                bb += i1[(size_t)pa * 4 + 0] - i1[(size_t)x * 4 + 0];
                bg += i1[(size_t)pa * 4 + 1] - i1[(size_t)x * 4 + 1];
                br += i1[(size_t)pa * 4 + 2] - i1[(size_t)x * 4 + 2];
                ba += i1[(size_t)pa * 4 + 3] - i1[(size_t)x * 4 + 3];
            }
        }
    }
}

#if defined(__aarch64__)
/* A pixel's 4 bytes widened into 4 int32 lanes [b, g, r, a] — the NEON
 * counterpart of `pmovzxbd`. The u32 view of the 4 bytes needs may_alias: the
 * project builds without -fno-strict-aliasing. */
static inline int32x4_t
blur_neon_load_px(const AxlGfxPixel *p)
{
    uint32_t   word = *(const uint32_t __attribute__((may_alias)) *)p;
    uint16x4_t v16  = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(word))));
    return vreinterpretq_s32_u32(vmovl_u16(v16));
}
#endif

#if defined(__x86_64__)
/* SSE4.1 row pass — same running sums, four channels at a time.
 *
 * The box-box has a SERIAL dependency along the pass axis (each sum steps from
 * its predecessor), so unlike the old tent kernel there is no vectorising
 * across neighbouring pixels: the parallel axis is the four BGRA channels,
 * which is exactly one 128-bit register of int32 lanes. That also means AVX2
 * would buy nothing here without restructuring to run several rows in lockstep
 * — hence SSE4.1 only.
 *
 * The rounding divide uses a 32-bit reciprocal so the high half of a
 * `pmuludq` IS the quotient: with M = floor(2^32/div) + 1 and e = M*div - 2^32,
 * floor(n*M / 2^32) == floor(n/div) whenever nmax*e < 2^32. The caller gates on
 * that (and on the accumulators fitting int32 lanes), so this path is EXACT —
 * it agrees with the scalar reciprocal because both equal the true quotient.
 *
 * `pmovzxbd` (a pixel's 4 bytes -> 4 int32 lanes) and `pmulld`/`packusdw` are
 * SSE4.1, which a UEFI app can use with no enable dance (firmware already
 * turned XMM on for the x64 calling convention). */
__attribute__((target("sse4.1"))) static void
blur_pass_boxbox_sse41(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    int32_t            *i1,
    uint32_t            recip      ///< M = floor(2^32/div) + 1
    )
{
    const int32_t  half = (int32_t)(((int64_t)(r + 1) * (int64_t)(r + 1)) / 2);
    const int64_t  hi   = (int64_t)w - 1;
    const uint32_t nI1  = w + (uint32_t)r;
    const __m128i  vhalf = _mm_set1_epi32(half);
    const __m128i  vmul  = _mm_set1_epi32((int32_t)recip);

    for (uint32_t l = 0; l < h; l++) {
        const AxlGfxPixel *s0 = &src[(size_t)l * w];
        AxlGfxPixel       *d0 = &dst[(size_t)l * w];

        /* box1: window sums of the edge-clamped source, 4 channels per lane. */
        __m128i acc = _mm_setzero_si128();
        for (int j = 0; j <= r; j++) {
            const AxlGfxPixel *s = &s0[blur_clampi((int64_t)j - r, hi)];
            acc = _mm_add_epi32(acc, _mm_cvtepu8_epi32(_mm_cvtsi32_si128(
                      (int32_t)*(const uint32_t __attribute__((may_alias)) *)s)));
        }
        for (uint32_t p = 0; p < nI1; p++) {
            _mm_storeu_si128((__m128i *)&i1[(size_t)p * 4], acc);
            if (p + 1 < nI1) {
                const AxlGfxPixel *a = &s0[blur_clampi((int64_t)p + 1, hi)];
                const AxlGfxPixel *e = &s0[blur_clampi((int64_t)p - r, hi)];
                acc = _mm_add_epi32(acc, _mm_cvtepu8_epi32(_mm_cvtsi32_si128(
                          (int32_t)*(const uint32_t __attribute__((may_alias)) *)a)));
                acc = _mm_sub_epi32(acc, _mm_cvtepu8_epi32(_mm_cvtsi32_si128(
                          (int32_t)*(const uint32_t __attribute__((may_alias)) *)e)));
            }
        }

        /* box2: sum r+1 of those, round once, pack back to one pixel. */
        __m128i acc2 = _mm_setzero_si128();
        for (uint32_t p = 0; p <= (uint32_t)r; p++) {
            acc2 = _mm_add_epi32(acc2, _mm_loadu_si128((const __m128i *)&i1[(size_t)p * 4]));
        }
        for (uint32_t x = 0; x < w; x++) {
            __m128i n = _mm_add_epi32(acc2, vhalf);
            /* floor(n*M / 2^32) per lane: pmuludq does lanes 0,2 -> two 64-bit
             * products whose HIGH dwords land in slots 1,3. Do 0,2 then 1,3. */
            __m128i p02 = _mm_mul_epu32(n, vmul);
            __m128i p13 = _mm_mul_epu32(_mm_srli_si128(n, 4), vmul);
            /* Gather the four high dwords back into lanes 0..3. */
            __m128i q = _mm_castps_si128(_mm_shuffle_ps(
                            _mm_castsi128_ps(p02), _mm_castsi128_ps(p13),
                            _MM_SHUFFLE(3, 1, 3, 1)));
            q = _mm_shuffle_epi32(q, _MM_SHUFFLE(3, 1, 2, 0));
            __m128i packed = _mm_packus_epi16(_mm_packus_epi32(q, q), _mm_setzero_si128());
            *(uint32_t __attribute__((may_alias)) *)&d0[x] =
                (uint32_t)_mm_cvtsi128_si32(packed);
            if (x + 1 < w) {
                uint32_t pa = x + 1 + (uint32_t)r;
                acc2 = _mm_add_epi32(acc2,
                           _mm_loadu_si128((const __m128i *)&i1[(size_t)pa * 4]));
                acc2 = _mm_sub_epi32(acc2,
                           _mm_loadu_si128((const __m128i *)&i1[(size_t)x * 4]));
            }
        }
    }
}

#endif /* __x86_64__ */

#if defined(__aarch64__)
/* NEON row pass — the SSE4.1 kernel's twin; see its comment for why the four
 * BGRA channels are the only parallel axis here. NEON is ARMv8-A baseline, so
 * this needs no target attribute and no enable dance. */
static void
blur_pass_boxbox_neon(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h,
    int                 r,
    int32_t            *i1,
    uint32_t            recip      ///< M = floor(2^32/div) + 1
    )
{
    const int32_t   half  = (int32_t)(((int64_t)(r + 1) * (int64_t)(r + 1)) / 2);
    const int64_t   hi    = (int64_t)w - 1;
    const uint32_t  nI1   = w + (uint32_t)r;
    const int32x4_t vhalf = vdupq_n_s32(half);
    const uint32x4_t vmul = vdupq_n_u32(recip);

    for (uint32_t l = 0; l < h; l++) {
        const AxlGfxPixel *s0 = &src[(size_t)l * w];
        AxlGfxPixel       *d0 = &dst[(size_t)l * w];

        int32x4_t acc = vdupq_n_s32(0);
        for (int j = 0; j <= r; j++) {
            acc = vaddq_s32(acc, blur_neon_load_px(&s0[blur_clampi((int64_t)j - r, hi)]));
        }
        for (uint32_t p = 0; p < nI1; p++) {
            vst1q_s32(&i1[(size_t)p * 4], acc);
            if (p + 1 < nI1) {
                acc = vaddq_s32(acc, blur_neon_load_px(&s0[blur_clampi((int64_t)p + 1, hi)]));
                acc = vsubq_s32(acc, blur_neon_load_px(&s0[blur_clampi((int64_t)p - r, hi)]));
            }
        }

        int32x4_t acc2 = vdupq_n_s32(0);
        for (uint32_t p = 0; p <= (uint32_t)r; p++) {
            acc2 = vaddq_s32(acc2, vld1q_s32(&i1[(size_t)p * 4]));
        }
        for (uint32_t x = 0; x < w; x++) {
            uint32x4_t n = vreinterpretq_u32_s32(vaddq_s32(acc2, vhalf));
            /* floor(n*M / 2^32) per lane: widen-multiply to 64 bits, then
             * shift-and-narrow back, which IS the high dword. */
            uint32x2_t q_lo = vshrn_n_u64(vmull_u32(vget_low_u32(n),
                                                    vget_low_u32(vmul)), 32);
            uint32x2_t q_hi = vshrn_n_u64(vmull_u32(vget_high_u32(n),
                                                    vget_high_u32(vmul)), 32);
            uint16x4_t q16  = vmovn_u32(vcombine_u32(q_lo, q_hi));
            uint8x8_t  q8   = vmovn_u16(vcombine_u16(q16, q16));
            *(uint32_t __attribute__((may_alias)) *)&d0[x] =
                vget_lane_u32(vreinterpret_u32_u8(q8), 0);
            if (x + 1 < w) {
                uint32_t pa = x + 1 + (uint32_t)r;
                acc2 = vaddq_s32(acc2, vld1q_s32(&i1[(size_t)pa * 4]));
                acc2 = vsubq_s32(acc2, vld1q_s32(&i1[(size_t)x * 4]));
            }
        }
    }
}
#endif /* __aarch64__ */

#if defined(AXL_BLUR_SIMD)
/* The 32-bit reciprocal the SIMD passes need, or 0 if this radius cannot use
 * it exactly (then the scalar pass runs). Conditions: the Granlund-Montgomery
 * bound nmax*e < 2^32, and every accumulator fitting a signed 32-bit lane. */
static uint32_t
blur_recip32(int64_t div, int64_t nmax)
{
    if (nmax > INT32_MAX) {
        return 0;                      /* box2 sums would overflow a lane */
    }
    uint64_t m = ((uint64_t)1 << 32) / (uint64_t)div + 1;
    uint64_t e = m * (uint64_t)div - ((uint64_t)1 << 32);
    if (m > UINT32_MAX || (uint64_t)nmax > (((uint64_t)1 << 32) - 1) / e) {
        return 0;
    }
    return (uint32_t)m;
}
#endif /* AXL_BLUR_SIMD */

/* Transpose @src (w wide, h tall) into @dst (h wide, w tall), in cache
 * tiles: dst[x][y] = src[y][x].
 *
 * This is what makes the second axis cheap. Run directly, the vertical
 * pass strides down a column — one useful pixel per 64-byte cache line,
 * so a full-screen veil streams the image through the cache once PER
 * COLUMN and the blur goes memory-bound (it was 97% of a 1280x800 frame).
 * Transposing first turns that pass into another unit-stride row pass;
 * the two extra passes over the image cost far less than the misses they
 * remove. A BLUR_TILE-square block of both images is a few KB, so a tile
 * stays in L1 while it is being scattered. Pure data movement — the blur
 * result is unchanged bit-for-bit. */
#define BLUR_TILE 32u

static void
blur_transpose(
    const AxlGfxPixel  *src,
    AxlGfxPixel        *dst,
    uint32_t            w,
    uint32_t            h
    )
{
    for (uint32_t y0 = 0; y0 < h; y0 += BLUR_TILE) {
        const uint32_t ymax = (h - y0 < BLUR_TILE) ? h : y0 + BLUR_TILE;
        for (uint32_t x0 = 0; x0 < w; x0 += BLUR_TILE) {
            const uint32_t xmax = (w - x0 < BLUR_TILE) ? w : x0 + BLUR_TILE;
            for (uint32_t y = y0; y < ymax; y++) {
                const AxlGfxPixel *s = &src[(size_t)y * w];
                for (uint32_t x = x0; x < xmax; x++) {
                    dst[(size_t)x * h + y] = s[x];
                }
            }
        }
    }
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
    /* Scratch for the box-box intermediate: one line's I1[], edge-extended
     * by r leading entries, 4 interleaved channels — reused across every line
     * and both axis passes. int32 (not int64): a box1 window sum is at most
     * 255*(r+1), and r is clamped to max_dim, so it fits int32 for any buffer
     * up to ~8M px per side — far past any real display. Halving this array's
     * width halves the blur's dominant memory traffic. */
    int32_t *i1 = axl_malloc(((size_t)max_dim + (size_t)r) * 4 * sizeof(int32_t));
    if (i1 == NULL) {
        axl_free(tmp);
        return AXL_ERR;
    }

    /* Both axes as ROW passes, with a transpose between them (and one back).
       `px` and `tmp` are the same w*h pixels either way round, so the
       transposed intermediate needs no extra allocation. */
    /* SIMD rung: the four BGRA channels in one vector register. Bit-identical
       to the scalar pass (both reciprocals compute the exact quotient), so the
       dispatch is invisible to callers — gfx-simd-selftest.c pins that on every
       CPU model. Falls through to scalar when the radius cannot use the 32-bit
       reciprocal exactly, or the rung is absent. */
#if defined(AXL_BLUR_SIMD)
    const int64_t  bdiv = (int64_t)(r + 1) * (int64_t)(r + 1);
    const uint32_t m32  = blur_recip32(bdiv, 255 * bdiv + bdiv / 2);
#if defined(__x86_64__)
    const bool simd_ok = m32 != 0 && axl_cpu_features()->sse41;
#else
    const bool simd_ok = m32 != 0 && axl_cpu_features()->neon;
#endif
    if (simd_ok) {
#if defined(__x86_64__)
#define BLUR_PASS_SIMD blur_pass_boxbox_sse41
#else
#define BLUR_PASS_SIMD blur_pass_boxbox_neon
#endif
        BLUR_PASS_SIMD(px, tmp, w, h, r, i1, m32);
        blur_transpose(tmp, px, w, h);
        BLUR_PASS_SIMD(px, tmp, h, w, r, i1, m32);
        blur_transpose(tmp, px, h, w);
#undef BLUR_PASS_SIMD
        axl_free(i1);
        axl_free(tmp);
        return AXL_OK;
    }
#endif
    blur_pass_boxbox(px, tmp, w, h, r, i1);   /* rows:    px  -> tmp        */
    blur_transpose(tmp, px, w, h);            /* px is now h wide, w tall   */
    blur_pass_boxbox(px, tmp, h, w, r, i1);   /* columns: px  -> tmp        */
    blur_transpose(tmp, px, h, w);            /* back to w wide, h tall     */

    axl_free(i1);
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
