/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-math.c
    AxlMath — libm-free implementations of floor / ceil / fabs /
    sqrt / sin / cos / fmod.

    Lifted from previously-duplicated file-static helpers in
    src/gfx/axl-truetype.c (the `axl_ttf_*_` family) and
    src/gfx/axl-gfx-path.c (the `path_*_` family) into a shared
    module per [[dogfood]] + DRY.

    Each hardware-pathable primitive (sqrt / floor / ceil / fabs
    / fma-in-Horner) has both a compiler-builtin path that lowers
    to a single CPU instruction, and a libm-free manual fallback
    that's correct on any target.  Selection is compile-time per
    `-march` via the `AXL_MATH_HAS_HW_*` flags below — no runtime
    dispatch, no per-call branch.  `--gc-sections` strips the
    unused branch from the link.
**/

#include <float.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-math.h>

// ===================================================================
// Hardware-feature detection
// ===================================================================
//
// Compile-time switches selected by the preprocessor based on the
// active `-march` flag.  All four constants resolve to either 1 or 0
// so each function body can use a single `#if AXL_MATH_HAS_HW_*`
// without conditional-arch checks scattered through it.
//
// Coverage on the architectures axl-sdk targets:
//
//                 baseline x86-64    x86-64-v2 (Nehalem)   x86-64-v3 (Haswell)   AArch64 baseline
//   HW_SQRT       SSE2 ✓            ✓                     ✓                     FSQRT ✓
//   HW_FABS       SSE2 ✓            ✓                     ✓                     FABS  ✓
//   HW_ROUND      ✗                 ROUNDSD ✓             ✓                     FRINTM/P ✓
//   HW_FMA        ✗                 ✗                     VFMADD ✓              FMADD ✓

/// SQRTSD (x86 SSE2 baseline) / FSQRT (AArch64 baseline).
#if defined(__SSE2__) || defined(__aarch64__)
#  define AXL_MATH_HAS_HW_SQRT   1
#else
#  define AXL_MATH_HAS_HW_SQRT   0
#endif

/// ROUNDSD (x86 SSE4.1+) / FRINTM, FRINTP (AArch64 baseline).
#if defined(__SSE4_1__) || defined(__aarch64__)
#  define AXL_MATH_HAS_HW_ROUND  1
#else
#  define AXL_MATH_HAS_HW_ROUND  0
#endif

/// ANDPS-as-fabs (x86 SSE2 baseline) / FABS (AArch64 baseline).
#if defined(__SSE2__) || defined(__aarch64__)
#  define AXL_MATH_HAS_HW_FABS   1
#else
#  define AXL_MATH_HAS_HW_FABS   0
#endif

/// Fused multiply-add — VFMADD132SD / FMADD.  Each Horner step
/// becomes one instruction instead of mul+add, ~30% faster + one
/// extra bit of precision per term.
#if defined(__FMA__) || defined(__aarch64__)
#  define AXL_MATH_HAS_HW_FMA    1
#else
#  define AXL_MATH_HAS_HW_FMA    0
#endif

/// POPCNT (x86 SSE4.2+) / CNT (AArch64 baseline via NEON).  Without
/// hardware support, `__builtin_popcountll` emits a libgcc call
/// (`__popcountdi2`) which fails to link in a freestanding build —
/// fall back to a SWAR implementation that uses only basic int math.
#if defined(__SSE4_2__) || defined(__POPCNT__) || defined(__aarch64__)
#  define AXL_MATH_HAS_HW_POPCOUNT  1
#else
#  define AXL_MATH_HAS_HW_POPCOUNT  0
#endif

/* Internal helper macro.  Use `AXL_MATH_FMA(a, b, c)` in polynomial
 * evaluation; expands to a single fma instruction where available,
 * otherwise to plain mul + add.  Not exposed in the public header —
 * consumers don't need it; sin/cos/future-pow are the call sites. */
#if AXL_MATH_HAS_HW_FMA
#  define AXL_MATH_FMA(a, b, c)  __builtin_fma((a), (b), (c))
#else
#  define AXL_MATH_FMA(a, b, c)  ((a) * (b) + (c))
#endif

double
axl_floor(
    double  x
    )
{
#if AXL_MATH_HAS_HW_ROUND
    return __builtin_floor(x);
#else
    /* Integer-truncation toward zero + sign correction.  Safe for
     * values in the int64_t range; UI inputs are well inside. */
    int64_t i = (int64_t)x;
    if (x < (double)i) {
        return (double)(i - 1);
    }
    return (double)i;
#endif
}

double
axl_ceil(
    double  x
    )
{
#if AXL_MATH_HAS_HW_ROUND
    return __builtin_ceil(x);
#else
    int64_t i = (int64_t)x;
    if (x > (double)i) {
        return (double)(i + 1);
    }
    return (double)i;
#endif
}

int
axl_floori(
    double  x
    )
{
#if AXL_MATH_HAS_HW_ROUND
    return (int)__builtin_floor(x);
#else
    int i = (int)x;
    return (x < (double)i) ? i - 1 : i;
#endif
}

int
axl_ceili(
    double  x
    )
{
#if AXL_MATH_HAS_HW_ROUND
    return (int)__builtin_ceil(x);
#else
    int i = (int)x;
    return (x > (double)i) ? i + 1 : i;
#endif
}

double
axl_fabs(
    double  x
    )
{
#if AXL_MATH_HAS_HW_FABS
    return __builtin_fabs(x);
#else
    return x < 0.0 ? -x : x;
#endif
}

double
axl_sqrt(
    double  x
    )
{
    if (x <= 0.0) {
        return 0.0;
    }
#if AXL_MATH_HAS_HW_SQRT
    return __builtin_sqrt(x);
#else
    /* Newton's method.  Initial guess: x itself.  10 iterations
     * gives sub-1e-12 accuracy for typical inputs; we burn a few
     * extra iterations for safety since callers are not
     * hot-loop sqrt-bound. */
    double r = x;
    for (int i = 0; i < 10; i++) {
        r = (r + x / r) * 0.5;
    }
    return r;
#endif
}

double
axl_fmod(
    double  x,
    double  y
    )
{
    if (y == 0.0) {
        return 0.0;
    }
    return x - (double)(int64_t)(x / y) * y;
}

double
axl_wrap(
    double  x,
    double  n
    )
{
    /* Safe-default for non-positive period; mirrors axl_fmod's
     * zero-divisor convention.  A negative period would otherwise
     * produce a value in (n, 0] which is rarely what callers want
     * — clamp to 0 so the bug surfaces at the call site. */
    if (n <= 0.0) {
        return 0.0;
    }
    /* x - n * floor(x / n) — the floor (toward -∞) makes the result
     * land in [0, n) for any sign of x, unlike fmod's truncation
     * (toward zero) which preserves the sign of the dividend. */
    return x - n * axl_floor(x / n);
}

double
axl_sin(
    double  x
    )
{
    /* Step 1: reduce to (-π, π] in constant time.  The
     * `floor((x + π) / 2π) * 2π` term is the nearest multiple of
     * 2π — subtracting it lands x in one period regardless of
     * magnitude.  Behavior on NaN / ±∞ is implementation-defined
     * (the floor cast hits int64 overflow); UI inputs are bounded
     * well below that. */
    x -= AXL_MATH_TWO_PI * axl_floor((x + AXL_MATH_PI) / AXL_MATH_TWO_PI);

    /* Step 2: reduce to [-π/2, π/2] using identities
     *   sin(π - x) = sin(x)   for x ∈ (π/2, π]
     *   sin(x)     = -sin(π + x)  for x ∈ [-π, -π/2)
     */
    int negate = 0;
    if (x > AXL_MATH_HALF_PI) {
        x = AXL_MATH_PI - x;
    } else if (x < -AXL_MATH_HALF_PI) {
        x = AXL_MATH_PI + x;
        negate = 1;
    }

    /* Step 3: 6-term Taylor (through x¹¹/11!) evaluated as a Horner
     * polynomial in u = x².  Each step is one AXL_MATH_FMA — single
     * VFMADD132SD on x86-FMA / FMADD on AArch64, plain mul+add on
     * older x86.  Equivalent to:
     *   sin(x) ≈ x * (1 - u/6 + u²/120 - u³/5040
     *                  + u⁴/362880 - u⁵/39916800)
     * The constant divisions are compile-time folded. */
    double u = x * x;
    double p = -1.0 / 39916800.0;       /* -1/11! */
    p = AXL_MATH_FMA(p, u,  1.0 / 362880.0);   /* +1/9!  */
    p = AXL_MATH_FMA(p, u, -1.0 / 5040.0);     /* -1/7!  */
    p = AXL_MATH_FMA(p, u,  1.0 / 120.0);      /* +1/5!  */
    p = AXL_MATH_FMA(p, u, -1.0 / 6.0);        /* -1/3!  */
    p = AXL_MATH_FMA(p, u,  1.0);              /* +1     */
    double s = x * p;
    return negate ? -s : s;
}

double
axl_cos(
    double  x
    )
{
    return axl_sin(x + AXL_MATH_HALF_PI);
}

// ===================================================================
// Logarithmic / exponential
// ===================================================================

double
axl_ln(
    double  x
    )
{
    /* `!(x > 0)` catches negative, ±0, and NaN (NaN comparisons are
     * always false) in a single test. */
    if (!(x > 0.0)) {
        return 0.0;
    }
    /* Read the IEEE 754 fields directly — much faster than a
     * frexp-style loop and avoids any libm dependence. */
    union { double d; uint64_t u; } bits = { .d = x };
    int raw_exp = (int)((bits.u >> 52) & 0x7FFull);

    /* Reject non-normal inputs (subnormals: raw_exp == 0; +Inf and
     * +NaN-payload: raw_exp == 0x7FF).  Mathematically ln(subnormal)
     * is ~-744 and ln(+Inf) is +Inf; we follow the safe-default
     * convention rather than emit a sentinel.  Documented in the
     * header as "normal-double input range". */
    if (raw_exp == 0 || raw_exp == 0x7FF) {
        return 0.0;
    }
    /* Decompose into m * 2^e with m ∈ [1, 2) by forcing the
     * exponent field to the bias. */
    bits.u = (bits.u & 0x000FFFFFFFFFFFFFull) | (1023ull << 52);
    double m = bits.d;
    int    e = raw_exp - 1023;

    /* Mantissa log via the s = (m-1)/(m+1) substitution:
     *   ln(m) = 2 * (s + s³/3 + s⁵/5 + s⁷/7 + s⁹/9 + ...)
     * Convergence: |s| < 1/3 for m ∈ [1, 2), so terms decay fast.
     * 9 terms (through s¹⁷/17) gives error ≲ 1e-10 worst case. */
    double s  = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    /* Horner on s².  Coefficients are the odd-only reciprocals. */
    double p = 1.0 / 17.0;
    p = AXL_MATH_FMA(p, s2, 1.0 / 15.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 13.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 11.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 9.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 7.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 5.0);
    p = AXL_MATH_FMA(p, s2, 1.0 / 3.0);
    p = AXL_MATH_FMA(p, s2, 1.0);
    double ln_m = 2.0 * s * p;

    return ln_m + (double)e * AXL_MATH_LOG_2;
}

double
axl_exp(
    double  x
    )
{
    /* NaN passthrough — avoids UB from `(int)NaN` cast below.
     * `x != x` is the canonical NaN test that works without
     * pulling in <math.h>. */
    if (x != x) {
        return x;
    }
    /* Range-reduce: x = k * ln(2) + r with k integer rounded to
     * nearest, r ∈ [-ln(2)/2, ln(2)/2] ≈ [-0.347, 0.347].
     * `floor(x/ln(2) + 0.5)` is the round-to-nearest-int form. */
    double k_d = axl_floor(x / AXL_MATH_LOG_2 + 0.5);

    /* Saturate at the IEEE 754 double range boundaries.  k ∈
     * [-1022, 1023] covers the normal range; outside that the
     * 2^k reconstruction would write an out-of-range exponent
     * field, so we substitute the saturated value.  Bounding
     * k_d here also keeps the (int) cast below safe (covers ±Inf
     * automatically — comparison against finite always saturates). */
    if (k_d >= 1024.0) {
        return DBL_MAX;
    }
    if (k_d <= -1023.0) {
        return 0.0;
    }
    int    k = (int)k_d;
    double r = x - k_d * AXL_MATH_LOG_2;

    /* exp(r) via 10-term Taylor on r evaluated as Horner:
     *   1 + r + r²/2 + r³/6 + ... + r⁹/9!
     * With |r| < 0.347, the omitted r¹⁰/10! term is ≲ 5e-15
     * — comfortably below the FP-rounding floor.  Compound error
     * after the 2^k multiply remains < 1e-12 even for k near the
     * exponent-range limits.  Eight terms (the natural stopping
     * point for sin/cos) was marginal at |r| near 0.347 and
     * compounded above 1e-9 when multiplied by 2^k. */
    double p = 1.0 / 362880.0;
    p = AXL_MATH_FMA(p, r, 1.0 / 40320.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 5040.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 720.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 120.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 24.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 6.0);
    p = AXL_MATH_FMA(p, r, 1.0 / 2.0);
    p = AXL_MATH_FMA(p, r, 1.0);
    p = AXL_MATH_FMA(p, r, 1.0);
    /* p ≈ exp(r). */

    /* Build 2^k by writing the IEEE 754 exponent field directly.
     * k bounded to [-1022, 1023] above, so k+1023 ∈ [1, 2046] —
     * always a valid normal-double exponent encoding. */
    union { double d; uint64_t u; } two_k =
        { .u = (uint64_t)(k + 1023) << 52 };
    return p * two_k.d;
}

double
axl_pow(
    double  base,
    double  exponent
    )
{
    /* Edge cases first — fast-path the common shapes and avoid
     * sending degenerate values through ln + exp. */
    if (exponent == 0.0) {
        return 1.0;  /* x^0 = 1 for all x including 0, per IEEE 754. */
    }
    if (base == 1.0) {
        return 1.0;  /* 1^y = 1 for any finite y. */
    }
    if (base < 0.0) {
        return 0.0;  /* Documented safe-default; libm would give NaN. */
    }
    if (base == 0.0) {
        return 0.0;  /* 0^(positive) = 0; the y=0 case caught above. */
    }
    return axl_exp(exponent * axl_ln(base));
}

// ===================================================================
// Interpolation + easing
// ===================================================================

double
axl_lerp(
    double  a,
    double  b,
    double  t
    )
{
    /* Form `a + (b - a) * t` rather than `(1 - t) * a + t * b`:
     * the first preserves `t == 0 → a` and `t == 1 → b` exactly
     * (the multiplied form can drift by an FP ULP at t==1). */
    return a + (b - a) * t;
}

double
axl_ease_in_cubic(
    double  t
    )
{
    return t * t * t;
}

double
axl_ease_out_cubic(
    double  t
    )
{
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

double
axl_ease_in_out_cubic(
    double  t
    )
{
    /* Below midpoint: 4t³ (accelerating).  Above: 1 - (-2t+2)³/2
     * (decelerating mirror image).  Continuous + smooth at t=0.5. */
    if (t < 0.5) {
        return 4.0 * t * t * t;
    }
    double u = -2.0 * t + 2.0;
    return 1.0 - (u * u * u) / 2.0;
}

double
axl_ease_in_quint(
    double  t
    )
{
    double t2 = t * t;
    return t2 * t2 * t;
}

double
axl_ease_out_quint(
    double  t
    )
{
    double u = 1.0 - t;
    double u2 = u * u;
    return 1.0 - u2 * u2 * u;
}

double
axl_ease_in_out_quint(
    double  t
    )
{
    if (t < 0.5) {
        double t2 = t * t;
        return 16.0 * t2 * t2 * t;
    }
    double u = -2.0 * t + 2.0;
    double u2 = u * u;
    return 1.0 - (u2 * u2 * u) / 2.0;
}

/* Endpoint short-circuits on all three sine variants — axl_sin and
 * axl_cos are accurate to ~1e-7, but animation keyframes expect
 * f(0) = 0 and f(1) = 1 *exactly* so a long-running animation
 * doesn't accumulate per-cycle drift.  CSS easing libraries do the
 * same. */
double
axl_ease_in_sine(
    double  t
    )
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 1.0 - axl_cos(t * AXL_MATH_HALF_PI);
}

double
axl_ease_out_sine(
    double  t
    )
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return axl_sin(t * AXL_MATH_HALF_PI);
}

double
axl_ease_in_out_sine(
    double  t
    )
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return (1.0 - axl_cos(t * AXL_MATH_PI)) / 2.0;
}

// ===================================================================
// Bit math — thin wrappers over the GCC __builtin_* family
// ===================================================================
//
// All five primitives lower to a single CPU instruction on the
// architectures axl-sdk targets:
//   x86-64 baseline: BSR/BSF (LZCNT/TZCNT with -mbmi).
//   AArch64 baseline: CLZ + the RBIT+CLZ trick for ctz; POPCNT
//   needs +simd (always on per the AAPCS64 baseline).
// The 0-input guards bypass the __builtin's UB; consumers get a
// defined sentinel they can rely on.

int
axl_clz(
    uint64_t  x
    )
{
    if (x == 0) {
        return 64;
    }
    return __builtin_clzll(x);
}

int
axl_ctz(
    uint64_t  x
    )
{
    if (x == 0) {
        return 64;
    }
    return __builtin_ctzll(x);
}

int
axl_popcount(
    uint64_t  x
    )
{
#if AXL_MATH_HAS_HW_POPCOUNT
    /* __builtin_popcountll is defined for all inputs including 0. */
    return __builtin_popcountll(x);
#else
    /* SWAR popcount — no libgcc, no hardware POPCNT.  Classic
     * divide-and-conquer: pair up bits, sum, then pair of pairs,
     * etc.  Hot enough that the multiply-by-0x01...01 trick at the
     * end is worth it (folds 8 byte-sums into one in one mul). */
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL)
        + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

int
axl_log2i(
    uint64_t  x
    )
{
    /* floor(log2(x)) = 63 - clz(x) for x > 0. */
    if (x == 0) {
        return 0;
    }
    return 63 - __builtin_clzll(x);
}

uint64_t
axl_round_up_pow2(
    uint64_t  x
    )
{
    /* 0 and 1 both round up to 1 (the smallest power of two). */
    if (x <= 1) {
        return 1;
    }
    /* Overflow check: anything above 2^63 has no representable
     * power-of-two ceiling in uint64_t. */
    if (x > (1ULL << 63)) {
        return 0;
    }
    /* For x ∈ [2, 2^63], `1 << (64 - clz(x - 1))` is the next
     * power of two >= x.  Subtracting 1 first makes pow2 inputs
     * stay fixed (e.g., x=8: x-1=7, clz=61, shift=3, result=8). */
    return 1ULL << (64 - __builtin_clzll(x - 1));
}

// ===================================================================
// Saturated arithmetic
// ===================================================================

uint8_t
axl_sat_add_u8(
    uint8_t  a,
    uint8_t  b
    )
{
    /* `unsigned int` is ≥ 16 bits in C, but on every target axl-sdk
     * cares about it's 32-bit — the sum cannot overflow. */
    unsigned  sum = (unsigned)a + (unsigned)b;
    return sum > 0xFFu ? (uint8_t)0xFF : (uint8_t)sum;
}

uint8_t
axl_sat_sub_u8(
    uint8_t  a,
    uint8_t  b
    )
{
    return a > b ? (uint8_t)(a - b) : (uint8_t)0;
}

uint16_t
axl_sat_mul_u16(
    uint16_t  a,
    uint16_t  b
    )
{
    /* 0xFFFF * 0xFFFF = 0xFFFE0001 fits in uint32_t. */
    uint32_t  product = (uint32_t)a * (uint32_t)b;
    return product > 0xFFFFu ? (uint16_t)0xFFFF : (uint16_t)product;
}

// ===================================================================
// Linear algebra — Vec2 + Mat3
// ===================================================================

AxlVec2
axl_vec2(
    double  x,
    double  y
    )
{
    return (AxlVec2){ .x = x, .y = y };
}

AxlVec2
axl_vec2_add(
    AxlVec2  a,
    AxlVec2  b
    )
{
    return (AxlVec2){ .x = a.x + b.x, .y = a.y + b.y };
}

AxlVec2
axl_vec2_sub(
    AxlVec2  a,
    AxlVec2  b
    )
{
    return (AxlVec2){ .x = a.x - b.x, .y = a.y - b.y };
}

AxlVec2
axl_vec2_scale(
    AxlVec2  v,
    double   k
    )
{
    return (AxlVec2){ .x = v.x * k, .y = v.y * k };
}

double
axl_vec2_dot(
    AxlVec2  a,
    AxlVec2  b
    )
{
    return a.x * b.x + a.y * b.y;
}

double
axl_vec2_length(
    AxlVec2  v
    )
{
    return axl_sqrt(v.x * v.x + v.y * v.y);
}

AxlVec2
axl_vec2_normalize(
    AxlVec2  v
    )
{
    /* Pre-scale by max-component to keep the squared-sum in range
     * for huge-magnitude inputs.  Without this, `v = (1e200, 1e200)`
     * would overflow `v.x*v.x` to +Inf, `len = +Inf`, then `v.x/Inf`
     * is 0 and the result is indistinguishable from the zero-input
     * safe-default.  This is the textbook robust normalize. */
    double ax = v.x < 0.0 ? -v.x : v.x;
    double ay = v.y < 0.0 ? -v.y : v.y;
    double m  = ax > ay ? ax : ay;
    if (m == 0.0) {
        return (AxlVec2){ .x = 0.0, .y = 0.0 };
    }
    double nx = v.x / m;
    double ny = v.y / m;
    double len = axl_sqrt(nx * nx + ny * ny) * m;
    return (AxlVec2){ .x = v.x / len, .y = v.y / len };
}

AxlMat3
axl_mat3_identity(
    void
    )
{
    return (AxlMat3){ .m = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
}

AxlMat3
axl_mat3_translate(
    double  tx,
    double  ty
    )
{
    return (AxlMat3){ .m = {
        1.0, 0.0,  tx,
        0.0, 1.0,  ty,
        0.0, 0.0, 1.0,
    }};
}

AxlMat3
axl_mat3_scale(
    double  sx,
    double  sy
    )
{
    return (AxlMat3){ .m = {
         sx, 0.0, 0.0,
        0.0,  sy, 0.0,
        0.0, 0.0, 1.0,
    }};
}

AxlMat3
axl_mat3_rotate(
    double  radians
    )
{
    /* Standard 2D rotation in math-style y-up coords:
     *   [ cos θ   -sin θ   0 ]
     *   [ sin θ    cos θ   0 ]
     *   [   0        0     1 ]
     * Applied to a column vector, rotates counter-clockwise. */
    double c = axl_cos(radians);
    double s = axl_sin(radians);
    return (AxlMat3){ .m = {
          c,  -s, 0.0,
          s,   c, 0.0,
        0.0, 0.0, 1.0,
    }};
}

AxlMat3
axl_mat3_skew(
    double  sx,
    double  sy
    )
{
    /* Standard 2D shear matrix:
     *   [ 1  sx 0 ]
     *   [ sy 1  0 ]
     *   [ 0  0  1 ]
     * sx shears x as a function of y; sy shears y as a function of x. */
    return (AxlMat3){ .m = {
        1.0,  sx, 0.0,
         sy, 1.0, 0.0,
        0.0, 0.0, 1.0,
    }};
}

AxlMat3
axl_mat3_mul(
    AxlMat3  a,
    AxlMat3  b
    )
{
    /* Standard 3x3 matrix multiply: r[i,j] = Σ_k a[i,k] * b[k,j].
     * Row-major index: r[i,j] = r.m[i*3 + j]. */
    AxlMat3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double sum = 0.0;
            for (int k = 0; k < 3; k++) {
                sum += a.m[i * 3 + k] * b.m[k * 3 + j];
            }
            r.m[i * 3 + j] = sum;
        }
    }
    return r;
}

AxlVec2
axl_mat3_transform_point(
    AxlMat3  m,
    AxlVec2  p
    )
{
    /* [ m[0] m[1] m[2] ]   [ p.x ]   [ m[0]*p.x + m[1]*p.y + m[2] ]
     * [ m[3] m[4] m[5] ] * [ p.y ] = [ m[3]*p.x + m[4]*p.y + m[5] ]
     * [ m[6] m[7] m[8] ]   [  1  ]   [ m[6]*p.x + m[7]*p.y + m[8] ]
     * Affine input has bottom row [0 0 1] so we ignore the w
     * component (always 1). */
    return (AxlVec2){
        .x = m.m[0] * p.x + m.m[1] * p.y + m.m[2],
        .y = m.m[3] * p.x + m.m[4] * p.y + m.m[5],
    };
}

// ===================================================================
// Inverse trigonometry
// ===================================================================

double
axl_atan(
    double  x
    )
{
    /* Sign reduce: atan is odd. */
    int negate = 0;
    if (x < 0.0) {
        x = -x;
        negate = 1;
    }
    /* Step A: |x| > 1 → atan(x) = π/2 - atan(1/x).  Track via
     * `flip_pi_2` so the final reconstruction is one branch. */
    int flip_pi_2 = 0;
    if (x > 1.0) {
        x = 1.0 / x;
        flip_pi_2 = 1;
    }
    /* Step B: x ≥ 0.5 → atan(x) = π/4 + atan((x-1)/(x+1)).
     * After both reductions |x| ≤ 1/3 — Taylor converges fast. */
    int add_pi_4 = 0;
    if (x >= 0.5) {
        x = (x - 1.0) / (x + 1.0);
        add_pi_4 = 1;
    }
    /* Taylor: atan(x) = x - x³/3 + x⁵/5 - x⁷/7 + ...
     * 15 odd terms (through x²⁹/29).  At |x| = 0.5 (the boundary
     * when step B is bypassed), omitted x³¹/31 ≈ 1.5e-11; at
     * |x| = 1/3 (step B's worst), trivially below 1e-15. */
    double x2 = x * x;
    double p = 1.0 / 29.0;
    p = AXL_MATH_FMA(p, x2, -1.0 / 27.0);
    p = AXL_MATH_FMA(p, x2,  1.0 / 25.0);
    p = AXL_MATH_FMA(p, x2, -1.0 / 23.0);
    p = AXL_MATH_FMA(p, x2,  1.0 / 21.0);
    p = AXL_MATH_FMA(p, x2, -1.0 / 19.0);
    p = AXL_MATH_FMA(p, x2,  1.0 / 17.0);
    p = AXL_MATH_FMA(p, x2, -1.0 / 15.0);
    p = AXL_MATH_FMA(p, x2,  1.0 / 13.0);
    p = AXL_MATH_FMA(p, x2, -1.0 / 11.0);
    p = AXL_MATH_FMA(p, x2,  1.0 /  9.0);
    p = AXL_MATH_FMA(p, x2, -1.0 /  7.0);
    p = AXL_MATH_FMA(p, x2,  1.0 /  5.0);
    p = AXL_MATH_FMA(p, x2, -1.0 /  3.0);
    p = AXL_MATH_FMA(p, x2,  1.0);
    double r = x * p;

    /* Undo step B: r currently atan(reduced); want π/4 + r. */
    if (add_pi_4) {
        r = AXL_MATH_PI / 4.0 + r;
    }
    /* Undo step A: r currently atan(1/x_orig); want π/2 - r. */
    if (flip_pi_2) {
        r = AXL_MATH_HALF_PI - r;
    }
    return negate ? -r : r;
}

double
axl_atan2(
    double  y,
    double  x
    )
{
    /* Origin: mathematically undefined; we return 0 per the
     * safe-default convention.  Documented in the header. */
    if (x == 0.0 && y == 0.0) {
        return 0.0;
    }
    /* On the y-axis: tan would be ±∞; short-circuit to avoid
     * division-by-zero in axl_atan. */
    if (x == 0.0) {
        return y > 0.0 ? AXL_MATH_HALF_PI : -AXL_MATH_HALF_PI;
    }
    double a = axl_atan(y / x);
    if (x > 0.0) {
        return a;  /* Quadrants I & IV: result in (-π/2, π/2). */
    }
    /* Quadrants II & III: shift by ±π so result lands in (-π, -π/2)
     * or (π/2, π].  Sign chosen by y: positive y → +π (upper half),
     * negative y → -π (lower half).  y == 0 with x < 0 takes the
     * +π branch (giving exactly π — the "west" direction). */
    return y >= 0.0 ? a + AXL_MATH_PI : a - AXL_MATH_PI;
}

double
axl_asin(
    double  x
    )
{
    /* Out-of-domain: safe-default 0. */
    if (x > 1.0 || x < -1.0) {
        return 0.0;
    }
    /* Exact endpoints — short-circuit before the 1/sqrt(0) blowup. */
    if (x == 1.0) {
        return AXL_MATH_HALF_PI;
    }
    if (x == -1.0) {
        return -AXL_MATH_HALF_PI;
    }
    /* asin(x) = atan(x / √(1 - x²)).  Factor 1 - x² as (1-x)(1+x)
     * to preserve relative precision near x = ±1 (same cancellation
     * trick as axl_acos). */
    double s2 = (1.0 - x) * (1.0 + x);
    return axl_atan(x / axl_sqrt(s2));
}

double
axl_acos(
    double  x
    )
{
    /* Out-of-domain: safe-default 0 (same convention as asin). */
    if (x > 1.0 || x < -1.0) {
        return 0.0;
    }
    /* `atan2(√(1-x²), x)` instead of `π/2 - asin(x)` so the
     * result near x=±1 is accurate (the subtractive form loses
     * ~half the bits to cancellation when both terms approach
     * π/2).  Also makes the asin+acos identity test load-bearing
     * by forcing acos through a different code path.
     *
     * Compute `1 - x²` as `(1-x)(1+x)` to dodge a second
     * subtractive cancellation: for x near 1, `x*x` already loses
     * many bits, and the subsequent `1 - x*x` magnifies the loss
     * by a factor of ~1/(1-x).  The factored form preserves
     * relative precision in both subterms. */
    double s2 = (1.0 - x) * (1.0 + x);
    return axl_atan2(axl_sqrt(s2), x);
}

// ===================================================================
// Geometry helpers — rect, segment, circle
// ===================================================================

bool
axl_point_in_rect(
    AxlVec2  p,
    AxlRect  r
    )
{
    /* Half-open: include top/left edges, exclude bottom/right.
     * Adjacent rects don't both claim a shared edge — standard
     * convention used by HTML canvas, X11, Cairo. */
    return p.x >= r.x
        && p.x <  r.x + r.w
        && p.y >= r.y
        && p.y <  r.y + r.h;
}

AxlRect
axl_rect_intersect(
    AxlRect  a,
    AxlRect  b
    )
{
    double x1 = axl_max(a.x, b.x);
    double y1 = axl_max(a.y, b.y);
    double x2 = axl_min(a.x + a.w, b.x + b.w);
    double y2 = axl_min(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1) {
        return (AxlRect){ 0.0, 0.0, 0.0, 0.0 };
    }
    return (AxlRect){ .x = x1, .y = y1, .w = x2 - x1, .h = y2 - y1 };
}

AxlRect
axl_rect_union(
    AxlRect  a,
    AxlRect  b
    )
{
    /* Empty input on either side: return the other.  Avoids the
     * (0,0,0,0) sentinel pulling the bounding box back to origin.
     * Both empty → returns `b` (second short-circuit fires). */
    if (a.w <= 0.0 || a.h <= 0.0) {
        return b;
    }
    if (b.w <= 0.0 || b.h <= 0.0) {
        return a;
    }
    double x1 = axl_min(a.x, b.x);
    double y1 = axl_min(a.y, b.y);
    double x2 = axl_max(a.x + a.w, b.x + b.w);
    double y2 = axl_max(a.y + a.h, b.y + b.h);
    return (AxlRect){ .x = x1, .y = y1, .w = x2 - x1, .h = y2 - y1 };
}

bool
axl_segment_intersect(
    AxlVec2   a1,
    AxlVec2   a2,
    AxlVec2   b1,
    AxlVec2   b2,
    AxlVec2  *out
    )
{
    /* Parametric form: a1 + t*(a2-a1) = b1 + s*(b2-b1).
     * Solve via 2D cross product.  denom == 0 → parallel (the
     * collinear-overlap case is documented as false-return). */
    double ax = a2.x - a1.x;
    double ay = a2.y - a1.y;
    double bx = b2.x - b1.x;
    double by = b2.y - b1.y;
    double denom = ax * by - ay * bx;
    if (denom == 0.0) {
        return false;
    }
    double cx = b1.x - a1.x;
    double cy = b1.y - a1.y;
    double t = (cx * by - cy * bx) / denom;
    double s = (cx * ay - cy * ax) / denom;
    if (t < 0.0 || t > 1.0 || s < 0.0 || s > 1.0) {
        return false;
    }
    if (out != NULL) {
        out->x = a1.x + t * ax;
        out->y = a1.y + t * ay;
    }
    return true;
}

double
axl_distance_point_to_segment(
    AxlVec2  p,
    AxlVec2  a,
    AxlVec2  b
    )
{
    double abx = b.x - a.x;
    double aby = b.y - a.y;
    double apx = p.x - a.x;
    double apy = p.y - a.y;
    double len_sq = abx * abx + aby * aby;
    if (len_sq == 0.0) {
        /* Degenerate: a == b, distance is just |p - a|. */
        return axl_sqrt(apx * apx + apy * apy);
    }
    /* Project ap onto ab, parameter t in [0, 1] for the segment. */
    double t = (apx * abx + apy * aby) / len_sq;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    double dx = p.x - (a.x + t * abx);
    double dy = p.y - (a.y + t * aby);
    return axl_sqrt(dx * dx + dy * dy);
}

bool
axl_circle_circle_intersect(
    AxlCircle  a,
    AxlCircle  b
    )
{
    /* Squared distance vs squared sum-of-radii — both quantities
     * are non-negative, comparison preserves ordering, and we skip
     * the sqrt. */
    double dx = a.center.x - b.center.x;
    double dy = a.center.y - b.center.y;
    double dist_sq = dx * dx + dy * dy;
    double r_sum  = a.radius + b.radius;
    return dist_sq <= r_sum * r_sum;
}

double
axl_clamp(
    double  x,
    double  lo,
    double  hi
    )
{
    /* Branch order matters when lo > hi (contractually
     * implementation-defined per the header).  Checking `x > hi`
     * first makes the upper clamp win — for (x=5, lo=10, hi=0) we
     * return hi=0 rather than lo=10.  Documented so consumers can
     * rely on it. */
    if (x > hi) {
        return hi;
    }
    if (x < lo) {
        return lo;
    }
    return x;
}

double
axl_min(
    double  a,
    double  b
    )
{
    return a < b ? a : b;
}

double
axl_max(
    double  a,
    double  b
    )
{
    return a > b ? a : b;
}

double
axl_remap(
    double  x,
    double  in_min,
    double  in_max,
    double  out_min,
    double  out_max
    )
{
    /* Degenerate input range — return out_min to avoid div-by-zero.
     * Documented in the header so consumers can rely on it. */
    double in_span = in_max - in_min;
    if (in_span == 0.0) {
        return out_min;
    }
    double t = (x - in_min) / in_span;
    return out_min + t * (out_max - out_min);
}

double
axl_step(
    double  edge,
    double  x
    )
{
    /* GLSL spec: returns 0 iff x < edge, else 1.  The boundary
     * value x == edge falls into the `else` branch and returns 1. */
    return x < edge ? 0.0 : 1.0;
}

double
axl_smoothstep(
    double  edge0,
    double  edge1,
    double  x
    )
{
    /* Cubic Hermite: t² * (3 - 2t) where t is the clamped linear
     * interpolation parameter.  Dogfoods axl_clamp.  Degenerate
     * (edge0 == edge1) falls through axl_clamp's lo > hi branch
     * cleanly: t becomes ±inf, clamped to edge1's side, giving 0 or
     * 1 — no NaN escapes. */
    double span = edge1 - edge0;
    if (span == 0.0) {
        /* Step-like fallback when the smoothing window collapses. */
        return x < edge0 ? 0.0 : 1.0;
    }
    double t = axl_clamp((x - edge0) / span, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
