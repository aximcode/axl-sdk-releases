/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * cpu-simd-selftest.c — exercise AxlCpu feature detection + the AVX
 * state-enable path against a chosen QEMU CPU model.
 *
 * The unit suite boots qemu64, which has no AVX, so the privileged
 * CR4.OSXSAVE + XSETBV sequence in axl_cpu_enable_avx() never executes
 * there.  This app prints the detected feature set and dispatch tier,
 * then — if the CPU advertises AVX2 — enables it and runs a real AVX2
 * instruction (a guarded 256-bit integer add via inline asm) to prove
 * the YMM state is actually usable (no #UD) after the enable.
 *
 * Driven by test/integration/test-cpu-simd-qemu.sh under two models:
 *   qemu64  → no AVX: tier SSE2/SSE4.1, enable=false, no AVX2 op.
 *   Haswell → AVX2:   tier AVX2 after enable, AVX2 op runs.
 *
 * Final line: "CPU-SIMD-SELFTEST: <N> passed, <M> failed".
 */

#include <axl.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

#if defined(__x86_64__)
/* 256-bit integer add via AVX2 VPADDD, isolated in a target-attributed
   function so the compiler only emits AVX2 here (the rest of the TU
   stays baseline).  Must only be CALLED after axl_cpu_enable_avx(). */
__attribute__((target("avx2")))
static uint32_t
avx2_add_smoke(void)
{
    /* a = {1,2,3,4,5,6,7,8}, add a+a, return lane 7 (== 16). */
    uint32_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t out[8];
    __asm__ volatile (
        "vmovdqu %1, %%ymm0\n\t"
        "vpaddd  %%ymm0, %%ymm0, %%ymm1\n\t"
        "vmovdqu %%ymm1, %0\n\t"
        "vzeroupper\n\t"
        : "=m"(out)
        : "m"(a)
        : "ymm0", "ymm1", "memory");
    return out[7];
}

/* 512-bit integer add via AVX-512F VPADDD on ZMM — must only be called
   after axl_cpu_enable_avx512(). Returns lane 15 (== 32). */
__attribute__((target("avx512f")))
static uint32_t
avx512_add_smoke(void)
{
    uint32_t a[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint32_t out[16];
    __asm__ volatile (
        "vmovdqu32 %1, %%zmm0\n\t"
        "vpaddd    %%zmm0, %%zmm0, %%zmm1\n\t"
        "vmovdqu32 %%zmm1, %0\n\t"
        "vzeroupper\n\t"
        : "=m"(out)
        : "m"(a)
        : "zmm0", "zmm1", "memory");
    return out[15];
}
#endif

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    const AxlCpuFeatures *f = axl_cpu_features();
    check(f != NULL, "axl_cpu_features returns non-NULL");
    if (f == NULL) {
        axl_printf("CPU-SIMD-SELFTEST: 0 passed, 1 failed\n");
        return 1;
    }

    axl_printf("features: sse2=%d sse3=%d ssse3=%d sse41=%d sse42=%d "
               "popcnt=%d xsave=%d avx=%d avx2=%d fma=%d neon=%d\n",
               f->sse2, f->sse3, f->ssse3, f->sse41, f->sse42,
               f->popcnt, f->xsave, f->avx, f->avx2, f->fma, f->neon);
    axl_printf("crypto: aes=%d pclmulqdq=%d sha=%d vaes=%d  rng: rdrand=%d rdseed=%d\n",
               f->aes, f->pclmulqdq, f->sha, f->vaes, f->rdrand, f->rdseed);
    axl_printf("bits: bmi1=%d bmi2=%d lzcnt=%d movbe=%d f16c=%d adx=%d\n",
               f->bmi1, f->bmi2, f->lzcnt, f->movbe, f->f16c, f->adx);
    axl_printf("avx512: f=%d dq=%d bw=%d vl=%d cd=%d vnni=%d\n",
               f->avx512f, f->avx512dq, f->avx512bw, f->avx512vl,
               f->avx512cd, f->avx512vnni);
    axl_printf("aarch64: fp16=%d lse=%d crc32=%d aes=%d pmull=%d sha1=%d "
               "sha2=%d sha512=%d sha3=%d dotprod=%d sve=%d\n",
               f->fp16, f->atomics, f->crc32, f->aes_a64, f->pmull, f->sha1,
               f->sha2, f->sha512, f->sha3, f->dotprod, f->sve);

    AxlSimdTier pre = axl_cpu_simd_tier();
    axl_printf("tier(before enable)=%d\n", (int)pre);
    /* Before enabling AVX, the tier can never be AVX2 even on an
       AVX2 CPU — enable is the gate. */
    check(pre != AXL_SIMD_AVX2, "tier is not AVX2 before enable");

    bool enabled = axl_cpu_enable_avx();
    check(enabled == f->avx, "enable_avx succeeds iff CPU has AVX");

    AxlSimdTier post = axl_cpu_simd_tier();
    axl_printf("tier(after enable)=%d  enabled=%d\n", (int)post, enabled);

    if (f->avx2 && enabled) {
        check(post == AXL_SIMD_AVX2, "tier is AVX2 after enabling on AVX2 CPU");
#if defined(__x86_64__)
        /* The payoff: actually run an AVX2 instruction.  If the enable
           sequence were wrong, this faults with #UD instead of
           returning 16. */
        uint32_t r = avx2_add_smoke();
        check(r == 16, "AVX2 VPADDD executes and computes 8+8 in lane 7 = 16");
#else
        check(true, "AVX2 op n/a on this arch (balance)");
#endif
    } else {
        /* No usable AVX2 (qemu64 / aarch64): tier stays at the SSE/NEON
           rung, and we must not have claimed AVX2. */
        check(post <= AXL_SIMD_SSE41, "tier stays <= SSE4.1 without usable AVX2");
        check(post != AXL_SIMD_AVX2, "tier not AVX2 without enable+avx2");
    }

    /* --- AVX-512 enable + smoke (x86, when present) --- */
    bool en512 = axl_cpu_enable_avx512();
    check(en512 == f->avx512f, "enable_avx512 succeeds iff CPU has AVX-512F");
    if (en512) {
#if defined(__x86_64__)
        uint32_t r5 = avx512_add_smoke();
        check(r5 == 32, "AVX-512 VPADDD on ZMM executes (16+16 in lane 15 = 32)");
#else
        check(true, "AVX-512 op n/a on this arch (balance)");
#endif
    } else {
        check(!f->avx512f, "no AVX-512 enabled when CPU lacks avx512f");
    }

    axl_printf("CPU-SIMD-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
