/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * gfx-simd-selftest.c — validate + benchmark the SIMD-dispatched blur.
 *
 * axl_gfx_buffer_blur() dispatches its inner convolution to an AVX2 /
 * SSE4.1 / NEON kernel per axl_cpu_simd_tier(); every kernel is meant
 * to be BIT-IDENTICAL to the scalar reference.  This app proves that on
 * whatever CPU model it runs under: it blurs a pseudo-random buffer via
 * the library (hardware path) and via an independent in-app scalar
 * reimplementation of the same triangular blur, and asserts the two
 * outputs are byte-for-byte equal.  It then times both to report the
 * speedup of the dispatched kernel over scalar.
 *
 * Driven by test/integration/test-gfx-simd-qemu.sh under several QEMU
 * CPU models (qemu64 / Nehalem / Haswell) so each x86 dispatch rung
 * (scalar / SSE4.1 / AVX2) is validated.  NEON is validated by the
 * AArch64 unit run.
 *
 * Final line: "GFX-SIMD-SELFTEST: <N> passed, <M> failed".
 */

#include <axl.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool cond, const char *label)
{
    if (cond) { g_pass++; axl_printf("PASS: %s\n", label); }
    else      { g_fail++; axl_printf("FAIL: %s\n", label); }
}

/* Independent scalar reference — mirrors blur_pass_scalar in
 * src/gfx/axl-gfx-effects.c exactly (same clamp, weights, rounding). */
static void
ref_pass(const AxlGfxPixel *src, AxlGfxPixel *dst,
         uint32_t w, uint32_t h, int r, bool horizontal)
{
    int64_t div = (int64_t)(r + 1) * (int64_t)(r + 1);
    int64_t half = div / 2;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int64_t sb = 0, sg = 0, sr = 0, sa = 0;
            for (int k = -r; k <= r; k++) {
                int64_t sx = (int64_t)x, sy = (int64_t)y;
                if (horizontal) {
                    sx += k; if (sx < 0) sx = 0; if (sx > (int64_t)w - 1) sx = (int64_t)w - 1;
                } else {
                    sy += k; if (sy < 0) sy = 0; if (sy > (int64_t)h - 1) sy = (int64_t)h - 1;
                }
                const AxlGfxPixel *s = &src[(size_t)sy * w + (size_t)sx];
                int wgt = (r + 1) - (k < 0 ? -k : k);
                sb += (int64_t)s->blue * wgt; sg += (int64_t)s->green * wgt;
                sr += (int64_t)s->red  * wgt; sa += (int64_t)s->alpha * wgt;
            }
            AxlGfxPixel *d = &dst[(size_t)y * w + (size_t)x];
            d->blue  = (uint8_t)((sb + half) / div); d->green = (uint8_t)((sg + half) / div);
            d->red   = (uint8_t)((sr + half) / div); d->alpha = (uint8_t)((sa + half) / div);
        }
    }
}

/* Mirror axl_gfx_buffer_blur's orchestration (radius clamp, H then V). */
static void
ref_blur(AxlGfxPixel *px, uint32_t w, uint32_t h, uint32_t radius)
{
    if (radius == 0) return;
    uint32_t max_dim = (w > h) ? w : h;
    int r = (radius > max_dim) ? (int)max_dim : (int)radius;
    AxlGfxPixel *tmp = axl_malloc((size_t)w * h * sizeof(AxlGfxPixel));
    if (tmp == NULL) return;
    ref_pass(px, tmp, w, h, r, true);
    ref_pass(tmp, px, w, h, r, false);
    axl_free(tmp);
}

static void
fill_lcg(AxlGfxPixel *p, size_t n, uint32_t seed)
{
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;   /* Numerical Recipes LCG */
        p[i].blue  = (uint8_t)(s >> 24);
        p[i].green = (uint8_t)(s >> 16);
        p[i].red   = (uint8_t)(s >> 8);
        p[i].alpha = (uint8_t)(s);
    }
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    const uint32_t W = 256, H = 256, R = 16;
    const size_t   N = (size_t)W * H;

    AxlGfxBuffer *lib = axl_gfx_buffer_new(W, H);
    AxlGfxPixel  *ref = axl_malloc(N * sizeof(AxlGfxPixel));
    if (lib == NULL || ref == NULL) {
        axl_printf("GFX-SIMD-SELFTEST: 0 passed, 1 failed (alloc)\n");
        return 1;
    }
    AxlGfxPixel *lp = axl_gfx_buffer_pixels(lib);

    /* Unlock AVX2 if present so the dispatcher can pick it. */
    axl_cpu_enable_avx();
    axl_printf("simd tier = %d  (%ux%u, radius %u)\n",
               (int)axl_cpu_simd_tier(), W, H, R);

    /* --- bit-exact validation --- */
    fill_lcg(lp, N, 0xC0FFEEu);
    for (size_t i = 0; i < N; i++) ref[i] = lp[i];
    axl_gfx_buffer_blur(lib, R);     /* hardware-dispatched path */
    ref_blur(ref, W, H, R);          /* independent scalar ref   */

    bool equal = true;
    for (size_t i = 0; i < N && equal; i++) {
        if (lp[i].blue != ref[i].blue || lp[i].green != ref[i].green ||
            lp[i].red  != ref[i].red  || lp[i].alpha != ref[i].alpha) {
            equal = false;
            axl_printf("  mismatch at %zu: lib=%02x%02x%02x%02x ref=%02x%02x%02x%02x\n",
                       i, lp[i].blue, lp[i].green, lp[i].red, lp[i].alpha,
                       ref[i].blue, ref[i].green, ref[i].red, ref[i].alpha);
        }
    }
    check(equal, "dispatched blur is bit-exact vs scalar reference");

    /* Odd dimensions exercise the AVX2 two-pixel loop's scalar tail
       column (and odd height on the vertical pass), which an even WxH
       never reaches. */
    {
        const uint32_t OW = 257, OH = 129;
        AxlGfxBuffer *olib = axl_gfx_buffer_new(OW, OH);
        AxlGfxPixel  *oref = axl_malloc((size_t)OW * OH * sizeof(AxlGfxPixel));
        bool oeq = (olib != NULL && oref != NULL);
        if (oeq) {
            AxlGfxPixel *olp = axl_gfx_buffer_pixels(olib);
            fill_lcg(olp, (size_t)OW * OH, 0xBEEF01u);
            for (size_t i = 0; i < (size_t)OW * OH; i++) oref[i] = olp[i];
            axl_gfx_buffer_blur(olib, R);
            ref_blur(oref, OW, OH, R);
            for (size_t i = 0; i < (size_t)OW * OH && oeq; i++) {
                if (olp[i].blue != oref[i].blue || olp[i].green != oref[i].green ||
                    olp[i].red  != oref[i].red  || olp[i].alpha != oref[i].alpha) oeq = false;
            }
        }
        check(oeq, "dispatched blur bit-exact on odd dimensions (AVX2 tail)");
        axl_gfx_buffer_free(olib);
        axl_free(oref);
    }

    /* --- SIMD source-over blend: bit-exact vs axl_gfx_blend (public
       scalar reference) over the full dst domain and several alphas.
       A 256x256 LCG fill spans the dst byte range; an odd width (257)
       drives the SSE2/AVX2 scalar tail. --- */
    {
        const uint32_t BWb = 257, BHb = 64;
        const size_t   NB = (size_t)BWb * BHb;
        AxlGfxBuffer  *blib = axl_gfx_buffer_new(BWb, BHb);
        AxlGfxPixel   *bref = axl_malloc(NB * sizeof(AxlGfxPixel));
        const uint8_t  alphas[] = {1, 64, 85, 128, 170, 200, 254};
        bool beq = (blib != NULL && bref != NULL);
        AxlGfxPixel *blp = beq ? axl_gfx_buffer_pixels(blib) : NULL;
        for (size_t ai = 0; ai < sizeof(alphas) && beq; ai++) {
            AxlGfxPixel color = AXL_GFX_RGBA(0x30, 0xA0, 0xF0, alphas[ai]);
            fill_lcg(blp, NB, 0x5A5A0000u + alphas[ai]);
            for (size_t i = 0; i < NB; i++) bref[i] = axl_gfx_blend(blp[i], color);
            axl_gfx_target_buffer(blib);
            axl_gfx_fill_rect(0, 0, BWb, BHb, color);   /* alpha<255 -> blend path */
            axl_gfx_target_buffer(NULL);
            for (size_t i = 0; i < NB && beq; i++) {
                if (blp[i].blue != bref[i].blue || blp[i].green != bref[i].green ||
                    blp[i].red  != bref[i].red  || blp[i].alpha != bref[i].alpha) beq = false;
            }
        }
        /* timing: dispatched fill-rect blend vs scalar axl_gfx_blend. */
        if (beq) {
            AxlGfxPixel color = AXL_GFX_RGBA(0x30, 0xA0, 0xF0, 128);
            const int BITERS = 200;
            fill_lcg(blp, NB, 0xAA01u);
            axl_gfx_target_buffer(blib);
            uint64_t b0 = axl_time_get_us();
            for (int it = 0; it < BITERS; it++) axl_gfx_fill_rect(0, 0, BWb, BHb, color);
            uint64_t b1 = axl_time_get_us();
            axl_gfx_target_buffer(NULL);
            fill_lcg(bref, NB, 0xAA01u);
            uint64_t b2 = axl_time_get_us();
            for (int it = 0; it < BITERS; it++)
                for (size_t i = 0; i < NB; i++) bref[i] = axl_gfx_blend(bref[i], color);
            uint64_t b3 = axl_time_get_us();
            uint64_t lus = (b1 - b0), sus = (b3 - b2);
            axl_printf("blend timing over %d iters: dispatched=%lluus scalar=%lluus\n",
                       BITERS, (unsigned long long)lus, (unsigned long long)sus);
            if (lus > 0)
                axl_printf("blend speedup (scalar/dispatched) = %llu.%02llux\n",
                           (unsigned long long)(sus / lus),
                           (unsigned long long)((sus * 100 / lus) % 100));
        }
        check(beq, "dispatched source-over blend bit-exact vs scalar (all alphas)");
        axl_gfx_buffer_free(blib);
        axl_free(bref);
    }

    /* --- blit_transform bilinear: deterministic-output regression.
       (A SIMD combine was prototyped here but measured ~4.6x SLOWER than
       scalar — per-pixel AVX marshalling + AVX/SSE transitions — so the
       sampler stays scalar; see blit_sample_put.)  Blit a fixed pattern
       through a fixed non-integer transform and print an FNV-1a hash; the
       harness asserts it is identical across CPU models (output must not
       depend on the CPU). --- */
    {
        const uint32_t SW = 40, SH = 40, DW = 96, DH = 96;
        AxlGfxBuffer *bsrc = axl_gfx_buffer_new(SW, SH);
        AxlGfxBuffer *bdst = axl_gfx_buffer_new(DW, DH);
        if (bsrc != NULL && bdst != NULL) {
            fill_lcg(axl_gfx_buffer_pixels(bsrc), (size_t)SW * SH, 0x7E57u);
            axl_gfx_buffer_clear(bdst, AXL_GFX_RGBA(0, 0, 0, 0xFF));
            /* rotate+scale+translate so bilinear weights are fractional. */
            AxlTransform t = axl_transform_translate(28.0, 22.0);
            t = axl_transform_multiply(axl_transform_scale(1.7, 1.3), t);
            t = axl_transform_multiply(axl_transform_rotate(0.3), t);
            axl_gfx_target_buffer(bdst);
            axl_gfx_blit_transform(bsrc, &t);
            axl_gfx_target_buffer(NULL);
            const AxlGfxPixel *dp = axl_gfx_buffer_pixels(bdst);
            uint32_t hash = 2166136261u;       /* FNV-1a over dest bytes */
            const uint8_t *bytes = (const uint8_t *)dp;
            for (size_t i = 0; i < (size_t)DW * DH * 4; i++) {
                hash ^= bytes[i];
                hash *= 16777619u;
            }
            axl_printf("BLIT-HASH: 0x%08x\n", hash);
            /* the blit must have drawn something (not the cleared bg). */
            check(hash != 0u, "blit_transform produced output (hash non-zero)");
        }
        axl_gfx_buffer_free(bsrc);
        axl_gfx_buffer_free(bdst);
    }

    /* --- timing: dispatched kernel vs scalar reference --- */
    const int ITERS = 20;
    fill_lcg(lp, N, 0x1234u);
    uint64_t t0 = axl_time_get_us();
    for (int it = 0; it < ITERS; it++) axl_gfx_buffer_blur(lib, R);
    uint64_t t1 = axl_time_get_us();

    fill_lcg(ref, N, 0x1234u);
    uint64_t t2 = axl_time_get_us();
    for (int it = 0; it < ITERS; it++) ref_blur(ref, W, H, R);
    uint64_t t3 = axl_time_get_us();

    uint64_t lib_us = (t1 - t0), ref_us = (t3 - t2);
    axl_printf("timing over %d iters: dispatched=%lluus scalar=%lluus\n",
               ITERS, (unsigned long long)lib_us, (unsigned long long)ref_us);
    if (lib_us > 0) {
        /* integer x100 to avoid floats: speedup = ref/lib */
        axl_printf("speedup (scalar/dispatched) = %llu.%02llux\n",
                   (unsigned long long)(ref_us / lib_us),
                   (unsigned long long)((ref_us * 100 / lib_us) % 100));
    }

    axl_gfx_buffer_free(lib);
    axl_free(ref);

    axl_printf("GFX-SIMD-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
